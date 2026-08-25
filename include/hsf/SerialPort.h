#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsf/ConfigManager.h"

namespace hsf {

// Cross-platform serial port reader/writer. POSIX (Linux/macOS) is
// implemented with termios in SerialPort_posix.cpp; Windows is implemented
// with the Win32 API in SerialPort_win.cpp. Reading happens on a dedicated
// background thread that does two independent things with incoming bytes:
//   - Splits them into CR/LF-delimited lines and forwards each complete
//     line to the data callback. This assumes a pure-text, one-line-per-
//     message device (mirrors how the Citizen ID card reader pushes a text
//     block per swipe) — any CR/LF byte appearing *within* a message, not
//     just at its end, will truncate it here.
//   - Separately, appends every raw byte to an accumulator with no framing
//     assumptions at all, drained by ReadAvailable(). This is what the Lua
//     Serial.Read() binding uses, so scripts talking to devices that don't
//     follow the CR/LF-per-message convention (e.g. fixed-length binary
//     frames) aren't silently truncated by the line-splitting above.
class SerialPort {
 public:
  using DataCallback = std::function<void(const std::string& line)>;

  SerialPort();
  ~SerialPort();

  // Reported state, for Serial.Status() in Lua and the Web UI
  // (docs/FixSerial.md §23). RECONNECTING is the interesting one: the port was
  // configured and the worker is retrying in the background, so a script gets
  // `false` from IsOpen() and nil from reads without anything being wedged.
  enum class State { Closed, Open, Error, Reconnecting };
  static const char* StateName(State state);

  // Opening a port that is ALREADY open with these exact settings is a no-op
  // that returns true, not a close-and-reopen. Scripts call this defensively
  // (config/scripts/main.lua reopens after every card), and a teardown costs
  // real time: Close() has to wait for the read thread to come out of a
  // blocking read before it can join it.
  bool Open(const SerialConfig& config);
  void Close();
  bool IsOpen() const;
  State GetState() const { return state_.load(); }

  // When enabled, a port that drops (USB unplugged, driver error) is reopened
  // by the read worker on a 1s/2s/5s backoff, and a failed Open() leaves that
  // worker retrying in the background instead of giving up
  // (docs/FixSerial.md §22). Off by default so nothing changes for callers that
  // manage the port themselves.
  void SetAutoReopen(bool enable);

  bool Write(const std::string& text);

  void SetDataCallback(DataCallback callback);

  // Returns and clears whatever raw bytes have arrived since the last call
  // (empty if none). No line-splitting or other framing is applied.
  std::string ReadAvailable();

  // Lists candidate serial device names present on this host (e.g.
  // /dev/ttyUSB0 on Linux, /dev/cu.* on macOS, COM3 on Windows). Used by the
  // Configuration page's "Scan" button; doesn't open or otherwise touch any
  // of the returned ports.
  static std::vector<std::string> ScanPorts();

 private:
  void ReadLoop();

  // Platform-specific: opens/reads/writes/closes the underlying handle.
  bool PlatformOpen(const SerialConfig& config);
  void PlatformClose();
  // Returns number of bytes read (0 on timeout, <0 on error).
  int PlatformRead(char* buffer, size_t bufferSize);
  bool PlatformWrite(const std::string& text);
  // Unblocks a read that is currently waiting, so Close() doesn't have to sit
  // out the read timeout before it can join the read thread. Best-effort: on
  // POSIX the termios VTIME already bounds the wait, so it is a no-op there.
  void PlatformCancelIo();

  // Settings the port was last opened with, for the no-op fast path in Open()
  // and for the worker's reconnect attempts.
  bool SameOpenConfig(const SerialConfig& config) const;
  SerialConfig openConfig_;

  // Starts the read/reconnect worker if it isn't already running.
  void StartWorker();
  // Joins it, cancelling any in-flight read first so this is bounded.
  void StopWorker();

  std::atomic<bool> open_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> autoReopen_{false};
  std::atomic<State> state_{State::Closed};
  std::thread readThread_;
  std::mutex callbackMutex_;
  DataCallback callback_;
  std::string lineBuffer_;

  std::mutex rawBufferMutex_;
  std::string rawBuffer_;

#if defined(_WIN32)
  void* handle_ = nullptr;  // HANDLE
#else
  int fd_ = -1;
#endif
};

}  // namespace hsf
