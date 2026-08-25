#include "hsf/SerialPort.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "hsf/Logger.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <glob.h>
#endif

namespace hsf {

SerialPort::SerialPort() = default;

SerialPort::~SerialPort() { Close(); }

const char* SerialPort::StateName(State state) {
  switch (state) {
    case State::Closed: return "CLOSED";
    case State::Open: return "OPEN";
    case State::Error: return "ERROR";
    case State::Reconnecting: return "RECONNECTING";
  }
  return "CLOSED";
}

bool SerialPort::SameOpenConfig(const SerialConfig& config) const {
  return openConfig_.port == config.port && openConfig_.baudrate == config.baudrate &&
         openConfig_.data_bits == config.data_bits && openConfig_.stop_bits == config.stop_bits &&
         openConfig_.parity == config.parity;
}

void SerialPort::SetAutoReopen(bool enable) { autoReopen_.store(enable); }

void SerialPort::StartWorker() {
  if (readThread_.joinable()) return;
  stopRequested_.store(false);
  readThread_ = std::thread(&SerialPort::ReadLoop, this);
}

void SerialPort::StopWorker() {
  stopRequested_.store(true);
  // Cancel the in-flight read first: otherwise the join waits for the worker to
  // time out of a blocking read that has no reason to finish (an idle port
  // produces nothing), which is dead time in whatever asked to close.
  PlatformCancelIo();
  if (readThread_.joinable()) readThread_.join();
}

bool SerialPort::Open(const SerialConfig& config) {
  // Already open on the same port with the same settings: there is nothing to
  // do, and doing it anyway is expensive. Close() must wait for the worker to
  // return from a blocking read before joining it, so the old unconditional
  // close-then-reopen made every defensive Serial.Open() call cost most of a
  // read timeout -- for a script that reopens after each card, that delay
  // landed in the middle of the workflow (docs/FixSerial.md §9).
  if (open_.load() && SameOpenConfig(config)) return true;

  // A retry worker for a DIFFERENT port (or one still trying to reach this one)
  // has to go before the handle changes underneath it.
  StopWorker();
  if (open_.load()) {
    PlatformClose();
    open_.store(false);
  }

  openConfig_ = config;

  const auto start = std::chrono::steady_clock::now();
  const bool opened = PlatformOpen(config);
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

  if (!opened) {
    state_.store(autoReopen_.load() ? State::Reconnecting : State::Error);
    Logger::Instance().Error(LogCategory::Serial, "Failed to open serial port " + config.port + " after " +
                                                       std::to_string(elapsedMs) + " ms");
    // Keep trying in the background rather than leaving the device unusable
    // until something calls Open() again (docs/FixSerial.md §22). The worker
    // opens the port itself, so callers see IsOpen() flip when it succeeds.
    if (autoReopen_.load()) StartWorker();
    return false;
  }

  open_.store(true);
  state_.store(State::Open);
  StartWorker();

  // Info, not Debug: an open that takes noticeably long is exactly what
  // docs/FixSerial.md set out to find, and the per-step Debug breakdown from
  // PlatformOpen sits right above this line in the log.
  Logger::Instance().Info(LogCategory::Serial, "Opened serial port " + config.port + " @ " +
                                                    std::to_string(config.baudrate) + " in " +
                                                    std::to_string(elapsedMs) + " ms");
  return true;
}

void SerialPort::Close() {
  // Also stops a background retry worker on a port that never opened, which is
  // why this no longer returns early on !open_.
  StopWorker();
  if (open_.load()) {
    PlatformClose();
    open_.store(false);
  }
  state_.store(State::Closed);
}

bool SerialPort::IsOpen() const { return open_.load(); }

bool SerialPort::Write(const std::string& text) {
  if (!open_.load()) return false;
  return PlatformWrite(text);
}

void SerialPort::SetDataCallback(DataCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  callback_ = std::move(callback);
}

std::vector<std::string> SerialPort::ScanPorts() {
  std::vector<std::string> ports;

#if defined(_WIN32)
  HKEY key;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
    char valueName[256];
    char data[256];
    DWORD index = 0;
    while (true) {
      DWORD valueNameSize = sizeof(valueName);
      DWORD dataSize = sizeof(data);
      DWORD type = 0;
      if (RegEnumValueA(key, index, valueName, &valueNameSize, nullptr, &type,
                         reinterpret_cast<BYTE*>(data), &dataSize) != ERROR_SUCCESS) {
        break;
      }
      if (type == REG_SZ) ports.emplace_back(data);
      ++index;
    }
    RegCloseKey(key);
  }
#else
  const char* patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*", "/dev/ttyAMA*", "/dev/tty.*", "/dev/cu.*"};
  for (const char* pattern : patterns) {
    glob_t globResult{};
    if (glob(pattern, GLOB_NOSORT, nullptr, &globResult) == 0) {
      for (size_t i = 0; i < globResult.gl_pathc; ++i) {
        ports.emplace_back(globResult.gl_pathv[i]);
      }
    }
    globfree(&globResult);
  }
  std::sort(ports.begin(), ports.end());
  ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
#endif

  return ports;
}

std::string SerialPort::ReadAvailable() {
  std::lock_guard<std::mutex> lock(rawBufferMutex_);
  std::string data;
  data.swap(rawBuffer_);
  return data;
}

void SerialPort::ReadLoop() {
  char buffer[512];

  // Reconnect backoff (docs/FixSerial.md §22). Short first, then longer: a USB
  // reader that was momentarily re-enumerated comes back within a second, while
  // one that is genuinely unplugged shouldn't be retried at 1Hz forever.
  static constexpr int kBackoffMs[] = {1000, 2000, 5000};
  int backoffIndex = 0;

  while (!stopRequested_.load()) {

    // Not open: either Open() failed and left this worker to keep trying, or the
    // port dropped mid-run below. Either way the retry happens HERE, on this
    // thread, so nothing on the Lua or web side ever waits for a reconnect.
    if (!open_.load()) {
      if (!autoReopen_.load()) break;

      state_.store(State::Reconnecting);

      if (PlatformOpen(openConfig_)) {
        open_.store(true);
        state_.store(State::Open);
        backoffIndex = 0;
        Logger::Instance().Info(LogCategory::Serial, "Serial port " + openConfig_.port + " reconnected");
        continue;
      }

      const int waitMs = kBackoffMs[backoffIndex];
      if (backoffIndex + 1 < static_cast<int>(sizeof(kBackoffMs) / sizeof(kBackoffMs[0]))) ++backoffIndex;

      // Slept in slices so Close() doesn't have to wait out a 5s backoff.
      for (int slept = 0; slept < waitMs && !stopRequested_.load(); slept += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }

    int n = PlatformRead(buffer, sizeof(buffer));
    if (n < 0) {
      // A cancelled read is how Close() unblocks this thread, not a fault --
      // reporting it as one would put a warning in the log every time a port
      // is closed normally.
      if (stopRequested_.load()) break;

      Logger::Instance().Warning(LogCategory::Serial,
                                  "Serial read error on " + openConfig_.port +
                                      (autoReopen_.load() ? "; reconnecting" : ""));

      // Drop the handle and let the block above take over. Without auto-reopen
      // that ends the worker, which is the old behaviour.
      PlatformClose();
      open_.store(false);
      state_.store(autoReopen_.load() ? State::Reconnecting : State::Error);
      if (!autoReopen_.load()) break;
      continue;
    }
    if (n == 0) continue;  // read timeout, loop again

    {
      std::lock_guard<std::mutex> lock(rawBufferMutex_);
      rawBuffer_.append(buffer, static_cast<size_t>(n));
    }

    for (int i = 0; i < n; ++i) {
      char c = buffer[i];
      if (c == '\n' || c == '\r') {
        if (!lineBuffer_.empty()) {
          std::string line = lineBuffer_;
          lineBuffer_.clear();
          std::lock_guard<std::mutex> lock(callbackMutex_);
          if (callback_) callback_(line);
        }
      } else {
        lineBuffer_.push_back(c);
      }
    }
  }
}

}  // namespace hsf
