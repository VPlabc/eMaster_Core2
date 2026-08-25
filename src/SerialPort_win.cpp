#include <windows.h>

#include <chrono>
#include <string>

#include "hsf/Logger.h"
#include "hsf/SerialPort.h"

namespace hsf {

namespace {
std::wstring ToWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Per-step timing for the open path (docs/FixSerial.md §6). Kept permanently and
// at Debug level: which Win32 call is slow is device- and driver-specific, so
// the next time a port takes seconds to open the answer should be in the log
// already rather than needing a rebuild to find out.
class StepTimer {
 public:
  explicit StepTimer(const char* step) : step_(step), start_(std::chrono::steady_clock::now()) {}

  ~StepTimer() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_)
                        .count();
    Logger::Instance().Debug(LogCategory::Serial, std::string("Serial open: ") + step_ + " took " +
                                                       std::to_string(ms) + " ms");
  }

 private:
  const char* step_;
  std::chrono::steady_clock::time_point start_;
};

std::string NormalizePortName(const std::string& port) {
  // COM ports above 9 need the \\.\ prefix to work with CreateFile.
  if (port.rfind("\\\\.\\", 0) == 0) return port;
  return "\\\\.\\" + port;
}
}  // namespace

bool SerialPort::PlatformOpen(const SerialConfig& config) {
  std::string fullName = NormalizePortName(config.port);

  HANDLE handle = INVALID_HANDLE_VALUE;
  {
    StepTimer timer("CreateFile");
    handle = CreateFileW(ToWide(fullName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (handle == INVALID_HANDLE_VALUE) return false;

  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  {
    StepTimer timer("GetCommState");
    if (!GetCommState(handle, &dcb)) {
      CloseHandle(handle);
      return false;
    }
  }

  dcb.BaudRate = static_cast<DWORD>(config.baudrate);
  dcb.ByteSize = static_cast<BYTE>(config.data_bits);
  dcb.StopBits = config.stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
  switch (config.parity) {
    case 'E': dcb.Parity = EVENPARITY; break;
    case 'O': dcb.Parity = ODDPARITY; break;
    default: dcb.Parity = NOPARITY; break;
  }
  dcb.fBinary = TRUE;
  dcb.fParity = dcb.Parity != NOPARITY;

  // Flow control is set EXPLICITLY, not inherited from GetCommState above.
  // Whatever the driver reports is whatever the port was last left as, and on
  // several USB-serial drivers that is CTS/DSR handshaking. Writing to a device
  // that never raises those lines -- the TDM-800 LED panel, wired TX/RX/GND --
  // then blocks in WriteFile for the whole write timeout and finally reports a
  // short write, so every Led.Show() cost the best part of a second and
  // returned false. None of the devices here use hardware or XON/XOFF flow
  // control, so all of it is turned off and DTR/RTS are simply asserted (some
  // adapters need DTR high to transmit at all).
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fTXContinueOnXoff = TRUE;
  dcb.fAbortOnError = FALSE;
  // Received 0x00 bytes must survive: card and LED payloads are binary, and
  // fNull would silently discard them.
  dcb.fNull = FALSE;
  dcb.fErrorChar = FALSE;

  {
    // The usual suspect when an open is slow: some USB-serial drivers
    // renegotiate with the device here, and it is the one step that can take
    // seconds on hardware that is present but unhappy.
    StepTimer timer("SetCommState");
    if (!SetCommState(handle, &dcb)) {
      CloseHandle(handle);
      return false;
    }
  }

  {
    // Anything already sitting in the driver buffers belongs to whoever had the
    // port before us -- a stale half-line here would be parsed as the next card.
    StepTimer timer("PurgeComm");
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
  }

  COMMTIMEOUTS timeouts{};
  // Read: the worker blocks here, so this is also how long a Close() can be
  // delayed if a driver ignores CancelIoEx.
  //
  // ReadTotalTimeoutMultiplier MUST be 0. Windows computes the deadline as
  // multiplier * bytes_REQUESTED + constant -- requested, not received -- and
  // the worker asks for 512 bytes a time. A multiplier of 10 therefore meant
  // 10 * 512 + 500 = 5,620 ms of blocking per data-less read, and since Close()
  // joins that thread, every Lua Open/Read/Close cycle paid it. Two of those is
  // the 10-15 second freeze reported in docs/FixSerial.md. With the multiplier
  // at 0 the deadline is a flat 100 ms no matter how large the buffer gets.
  timeouts.ReadIntervalTimeout = 50;
  timeouts.ReadTotalTimeoutConstant = 100;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  // Write: a bound, not a budget. With flow control off the driver takes the
  // bytes immediately, so this only caps a genuinely stuck port. The multiplier
  // is per byte actually being sent here, so a small one is harmless -- 2ms/byte
  // covers 9600 baud (~1ms/byte) with margin.
  timeouts.WriteTotalTimeoutConstant = 200;
  timeouts.WriteTotalTimeoutMultiplier = 2;
  SetCommTimeouts(handle, &timeouts);

  handle_ = handle;
  return true;
}

void SerialPort::PlatformCancelIo() {
  if (handle_) CancelIoEx(static_cast<HANDLE>(handle_), nullptr);
}

void SerialPort::PlatformClose() {
  if (handle_) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

int SerialPort::PlatformRead(char* buffer, size_t bufferSize) {
  if (!handle_) return -1;
  DWORD bytesRead = 0;
  if (!ReadFile(static_cast<HANDLE>(handle_), buffer, static_cast<DWORD>(bufferSize), &bytesRead, nullptr)) {
    return -1;
  }
  return static_cast<int>(bytesRead);  // 0 means the read timeout elapsed with no data
}

bool SerialPort::PlatformWrite(const std::string& text) {
  if (!handle_) return false;
  DWORD bytesWritten = 0;
  BOOL ok = WriteFile(static_cast<HANDLE>(handle_), text.data(), static_cast<DWORD>(text.size()), &bytesWritten,
                       nullptr);
  return ok && bytesWritten == text.size();
}

}  // namespace hsf
