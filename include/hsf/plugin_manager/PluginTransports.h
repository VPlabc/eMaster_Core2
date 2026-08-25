#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/TcpSocket.h"
#include "hsf/plugin.h"

namespace hsf {

// Host-side transports: the real implementations behind HSFTransportVTable
// (plan §8, Phase 3).
//
// A driver says "send these bytes, tell me when more arrive" and must not know
// whether they travel over TCP, a USB serial port, the main-board UART, or a
// mock. These are what make that true.
//
// All of them are NON-BLOCKING, because the ABI is. See transport.h for why
// that decision cannot be walked back: a blocking transport interface forces
// one thread per driver forever.

// TCP over the gateway's existing TcpSocket, using the non-blocking surface
// added there for this purpose.
//
// Reuses TcpSocket rather than opening sockets directly: that class already
// handles the BSD-vs-Winsock differences, the connect-with-timeout dance and
// the SO_ERROR read-back that a non-blocking connect needs, and all of it is
// proven by five existing callers.
class TcpTransport {
 public:
  // `host` and `port` come from the plugin's configuration, never from the
  // driver -- a driver that parsed an address would have escaped the
  // abstraction.
  TcpTransport(std::string host, int port, int connect_timeout_ms);

  HSFTransportRef Ref();

 private:
  static TcpTransport* Me(HSFTransport* t) { return reinterpret_cast<TcpTransport*>(t); }
  static const TcpTransport* Me(const HSFTransport* t) {
    return reinterpret_cast<const TcpTransport*>(t);
  }

  mutable std::mutex mutex_;
  TcpSocket          socket_;
  std::string        host_;
  int                port_;
  int                connect_timeout_ms_;
  mutable std::string last_error_;
};

// Serial, over the platform code in SerialPort_posix.cpp / SerialPort_win.cpp.
//
// WHY THIS DOES NOT USE SerialPort. That class is a serial *port*, not a serial
// *transport*: it owns a read thread and delivers complete LINES through a
// single callback slot, a shape driven by the citizen-ID card reader. A protocol
// driver needs raw bytes and needs to be told when they are available, not to be
// handed newline-delimited strings on someone else's thread. Wrapping it would
// mean fighting both of those.
//
// So this opens the device itself, with no thread and no line assembly. The
// platform specifics are small enough (termios / DCB, both already written once
// in SerialPort_*.cpp) that duplicating the open call is cheaper than making
// SerialPort serve two contradictory contracts.
class SerialTransport {
 public:
  struct Settings {
    std::string device;      // "/dev/ttyUSB0", "COM3" -- from configuration
    int baud = 9600;
    int data_bits = 8;
    int stop_bits = 1;
    char parity = 'N';       // 'N', 'E', 'O'
    bool rs485 = false;      // reserved: half-duplex turnaround handling
  };

  explicit SerialTransport(Settings settings);
  ~SerialTransport();

  HSFTransportRef Ref();

 private:
  static SerialTransport* Me(HSFTransport* t) { return reinterpret_cast<SerialTransport*>(t); }
  static const SerialTransport* Me(const HSFTransport* t) {
    return reinterpret_cast<const SerialTransport*>(t);
  }

  bool OpenDevice();
  void CloseDevice();

  mutable std::mutex mutex_;
  Settings           settings_;
  // intptr_t rather than int/HANDLE so the header needs no platform headers.
  // -1 is "closed" on both.
  intptr_t           handle_ = -1;
  mutable std::string last_error_;
};

// Builds transports from the `transport` block of a plugin's configuration, and
// hands the plugin an HSFTransportFactoryRef so a driver can open a second
// connection at runtime -- a Modbus gateway fronting several RTU sub-buses --
// without knowing what it is opening.
//
// The factory OWNS every transport it creates: a plugin gets a borrowed ref and
// must not free it. Host and plugin do not share an allocator, so ownership has
// to sit on one side, and the side that outlives the plugin is the host.
class TransportFactory {
 public:
  TransportFactory();
  ~TransportFactory();

  HSFTransportFactoryRef Ref();

  // Creates from a spec like
  //   {"type":"tcp","host":"10.0.0.5","port":502,"timeout_ms":2000}
  //   {"type":"serial","device":"/dev/ttyUSB0","baudrate":9600,"parity":"even"}
  //   {"type":"mock"}
  // Returns an invalid ref and sets `error` when the spec is unusable.
  HSFTransportRef Create(const nlohmann::json& spec, std::string& error);

  // Destroys one previously created transport. Safe on an unknown ref.
  void Destroy(HSFTransportRef ref);

  static bool Supports(HSFTransportKind kind);

 private:
  struct Owned;
  mutable std::mutex                  mutex_;
  std::vector<std::unique_ptr<Owned>> owned_;
};

}  // namespace hsf
