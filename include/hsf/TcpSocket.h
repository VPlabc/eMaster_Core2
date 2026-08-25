#pragma once

#include <cstdint>
#include <string>

namespace hsf {

// Small blocking TCP socket helper shared by RfidClient and the Lua Tcp.*
// bindings, so BSD-socket-vs-Winsock differences only need handling once.
class TcpSocket {
 public:
  TcpSocket();
  ~TcpSocket();

  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  bool Connect(const std::string& ip, int port, int timeoutMs = 3000);
  void Close();
  bool IsOpen() const;

  bool Send(const std::string& data);

  // Blocks up to timeoutMs waiting for data. Returns false (with data
  // untouched) on timeout, socket error, or if the socket isn't open.
  bool Receive(std::string& data, size_t maxBytes, int timeoutMs);

  // --- non-blocking surface, added for the plugin Transport API ------------
  //
  // WHY THIS IS SEPARATE FROM Send/Receive RATHER THAN REPLACING THEM.
  // Send() and Receive() both collapse timeout, peer-close and socket error
  // into a single `false`, and Send() treats any short write as fatal -- both
  // of which are correct only because Connect() leaves the socket in BLOCKING
  // mode. Five modules depend on that (ModbusClient, RfidClient, C3Client,
  // MqClient, the Lua Tcp.* bindings), so the blocking behaviour is left
  // exactly as it was.
  //
  // The plugin Transport API needs the distinction those three lose: "nothing
  // yet" and "never again" have to be different answers, or a driver cannot
  // tell a quiet link from a dead one. So this is an opt-in second surface on
  // the same class, and a socket only becomes non-blocking when its owner asks.
  enum class IoStatus {
    kOk,          // progress made; *transferred > 0
    kClosed,      // peer closed cleanly; *transferred == 0
    kWouldBlock,  // nothing happened, retry after Wait()
    kError        // socket is unusable; it has been closed
  };

  // Off by default, so nothing changes for existing callers. A socket shared
  // with blocking code must not be switched.
  bool SetBlocking(bool blocking);

  // Never block, regardless of the socket's mode: EWOULDBLOCK is reported as
  // kWouldBlock rather than treated as an error.
  IoStatus TryReceive(void* buffer, size_t capacity, size_t* received);
  IoStatus TrySend(const void* buffer, size_t length, size_t* sent);

  // Waits for readability/writability. timeoutMs < 0 waits indefinitely, 0
  // polls. Returns false on error or timeout with both flags cleared.
  bool Wait(bool wantRead, bool wantWrite, int timeoutMs, bool* readable, bool* writable);

  // The OS handle (fd on POSIX, SOCKET on Windows), or -1 when not open.
  // Exposed ONLY so a host event loop can add it to its own poll set.
  intptr_t NativeHandle() const;

 private:
#if defined(_WIN32)
  uintptr_t socket_;
#else
  int socket_;
#endif
};

}  // namespace hsf
