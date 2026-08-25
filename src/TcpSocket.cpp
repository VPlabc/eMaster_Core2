#include "hsf/TcpSocket.h"

#include <cerrno>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
using SockLen = int;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
using SockLen = socklen_t;
static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace hsf {

namespace {
#if defined(_WIN32)
struct WinsockInitializer {
  WinsockInitializer() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
};
void EnsureWinsockInitialized() { static WinsockInitializer initializer; }

void CloseSocketHandle(SocketHandle s) { closesocket(s); }
void SetNonBlocking(SocketHandle s, bool enable) {
  u_long mode = enable ? 1 : 0;
  ioctlsocket(s, FIONBIO, &mode);
}
#else
void CloseSocketHandle(SocketHandle s) { close(s); }
void SetNonBlocking(SocketHandle s, bool enable) {
  int flags = fcntl(s, F_GETFL, 0);
  if (enable) {
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
  } else {
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
  }
}
#endif
}  // namespace

TcpSocket::TcpSocket() : socket_(static_cast<decltype(socket_)>(kInvalidSocket)) {
#if defined(_WIN32)
  EnsureWinsockInitialized();
#endif
}

TcpSocket::~TcpSocket() { Close(); }

bool TcpSocket::Connect(const std::string& ip, int port, int timeoutMs) {
  Close();

  SocketHandle sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == kInvalidSocket) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    CloseSocketHandle(sock);
    return false;
  }

  SetNonBlocking(sock, true);
  int result = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

  // A refused/failed connection also makes a non-blocking socket writable,
  // so select() returning >0 only means "connect() finished", not "it
  // succeeded" — the actual outcome has to be read back via SO_ERROR.
  auto socketErrored = [sock]() {
    int soError = 0;
    SockLen len = sizeof(soError);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len) != 0) return true;
    return soError != 0;
  };

  bool connected = false;
#if defined(_WIN32)
  if (result == 0) {
    connected = true;
  } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    connected = select(0, nullptr, &writeSet, nullptr, &tv) > 0 && !socketErrored();
  }
#else
  if (result == 0) {
    connected = true;
  } else if (errno == EINPROGRESS) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    connected = select(sock + 1, nullptr, &writeSet, nullptr, &tv) > 0 && !socketErrored();
  }
#endif

  SetNonBlocking(sock, false);

  if (!connected) {
    CloseSocketHandle(sock);
    return false;
  }

  socket_ = static_cast<decltype(socket_)>(sock);
  return true;
}

void TcpSocket::Close() {
  if (static_cast<SocketHandle>(socket_) != kInvalidSocket) {
    CloseSocketHandle(static_cast<SocketHandle>(socket_));
    socket_ = static_cast<decltype(socket_)>(kInvalidSocket);
  }
}

bool TcpSocket::IsOpen() const { return static_cast<SocketHandle>(socket_) != kInvalidSocket; }

bool TcpSocket::Send(const std::string& data) {
  if (!IsOpen()) return false;
  size_t totalSent = 0;
  while (totalSent < data.size()) {
    int sent = send(static_cast<SocketHandle>(socket_), data.data() + totalSent,
                     static_cast<int>(data.size() - totalSent), 0);
    if (sent <= 0) {
      // Mirrors Receive(): a failed send means the socket is unusable, so
      // close it and let IsOpen() say so. Leaving it open made callers keep
      // writing into a dead connection and never notice they should
      // reconnect.
      Close();
      return false;
    }
    totalSent += static_cast<size_t>(sent);
  }
  return true;
}

bool TcpSocket::Receive(std::string& data, size_t maxBytes, int timeoutMs) {
  if (!IsOpen()) return false;

  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(static_cast<SocketHandle>(socket_), &readSet);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};

#if defined(_WIN32)
  int ready = select(0, &readSet, nullptr, nullptr, &tv);
#else
  int ready = select(static_cast<SocketHandle>(socket_) + 1, &readSet, nullptr, nullptr, &tv);
#endif
  if (ready <= 0) return false;

  std::vector<char> buffer(maxBytes);
  int received = recv(static_cast<SocketHandle>(socket_), buffer.data(), static_cast<int>(maxBytes), 0);
  if (received <= 0) {
    // 0 means the peer closed the connection; <0 is a socket error. Either
    // way the socket is no longer usable, so close it so IsOpen() reflects
    // that and callers know to reconnect rather than keep polling it.
    Close();
    return false;
  }

  data.assign(buffer.data(), static_cast<size_t>(received));
  return true;
}


// --- non-blocking surface ----------------------------------------------------
//
// See the header for why this sits beside Send/Receive rather than replacing
// them. Nothing here changes the behaviour of the blocking calls.

namespace {

// True when the last socket operation failed only because it would have
// blocked. Separated out because the two platforms report it differently and
// getting it wrong turns a healthy idle link into a reported failure.
bool WouldBlockNow() {
#if defined(_WIN32)
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// An interrupted call is not a failure -- a signal arriving mid-recv must not
// be reported as a dead socket.
bool Interrupted() {
#if defined(_WIN32)
  return WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

}  // namespace

bool TcpSocket::SetBlocking(bool blocking) {
  if (!IsOpen()) return false;
  SetNonBlocking(static_cast<SocketHandle>(socket_), !blocking);
  return true;
}

intptr_t TcpSocket::NativeHandle() const {
  if (!IsOpen()) return -1;
  return static_cast<intptr_t>(socket_);
}

TcpSocket::IoStatus TcpSocket::TryReceive(void* buffer, size_t capacity, size_t* received) {
  if (received) *received = 0;
  if (!IsOpen()) return IoStatus::kError;
  if (!buffer || capacity == 0) return IoStatus::kError;

  for (;;) {
    const int n = recv(static_cast<SocketHandle>(socket_), static_cast<char*>(buffer),
                       static_cast<int>(capacity), 0);
    if (n > 0) {
      if (received) *received = static_cast<size_t>(n);
      return IoStatus::kOk;
    }
    if (n == 0) {
      // Orderly shutdown by the peer. Distinct from kWouldBlock: this one will
      // never produce data again, and a driver that confused the two would
      // either spin forever or abandon a live link.
      Close();
      return IoStatus::kClosed;
    }
    if (Interrupted()) continue;
    if (WouldBlockNow()) return IoStatus::kWouldBlock;
    Close();
    return IoStatus::kError;
  }
}

TcpSocket::IoStatus TcpSocket::TrySend(const void* buffer, size_t length, size_t* sent) {
  if (sent) *sent = 0;
  if (!IsOpen()) return IoStatus::kError;
  if (!buffer || length == 0) return IoStatus::kError;

  for (;;) {
    const int n = send(static_cast<SocketHandle>(socket_), static_cast<const char*>(buffer),
                       static_cast<int>(length), 0);
    if (n > 0) {
      // A PARTIAL write is success here, unlike in Send(), which loops until
      // everything is out. The caller is told how much went and retries the
      // rest -- that is the whole point of a non-blocking write, and pretending
      // it was all-or-nothing is what makes a truncated frame reach the device.
      if (sent) *sent = static_cast<size_t>(n);
      return IoStatus::kOk;
    }
    if (n == 0) return IoStatus::kWouldBlock;  // nothing accepted, nothing wrong
    if (Interrupted()) continue;
    if (WouldBlockNow()) return IoStatus::kWouldBlock;
    Close();
    return IoStatus::kError;
  }
}

bool TcpSocket::Wait(bool wantRead, bool wantWrite, int timeoutMs, bool* readable,
                      bool* writable) {
  if (readable) *readable = false;
  if (writable) *writable = false;
  if (!IsOpen()) return false;
  if (!wantRead && !wantWrite) return false;

#if defined(_WIN32)
  // fd_set on Winsock is an array of handles, not a bitmask indexed by fd, so
  // a large SOCKET value is fine here -- the FD_SETSIZE hazard that broke
  // libmodbus on Windows does not apply to holding one socket.
  fd_set readSet, writeSet;
  FD_ZERO(&readSet);
  FD_ZERO(&writeSet);
  if (wantRead) FD_SET(static_cast<SocketHandle>(socket_), &readSet);
  if (wantWrite) FD_SET(static_cast<SocketHandle>(socket_), &writeSet);

  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  const int ready = select(0, wantRead ? &readSet : nullptr, wantWrite ? &writeSet : nullptr,
                           nullptr, timeoutMs < 0 ? nullptr : &tv);
  if (ready <= 0) return false;
  if (readable) *readable = wantRead && FD_ISSET(static_cast<SocketHandle>(socket_), &readSet);
  if (writable) *writable = wantWrite && FD_ISSET(static_cast<SocketHandle>(socket_), &writeSet);
  return true;
#else
  // poll(), not select(): select indexes fd_set by descriptor and silently
  // corrupts the stack for any fd >= FD_SETSIZE (1024). A long-lived gateway
  // opening and closing serial ports, sockets and SQLite handles can absolutely
  // reach that, and the failure would be memory corruption rather than an
  // error. poll has no such limit.
  struct pollfd p{};
  p.fd = static_cast<SocketHandle>(socket_);
  p.events = static_cast<short>((wantRead ? POLLIN : 0) | (wantWrite ? POLLOUT : 0));

  for (;;) {
    const int ready = poll(&p, 1, timeoutMs);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (ready == 0) return false;  // timeout
    // POLLHUP means the peer hung up; report it as readable so the caller's
    // next read returns kClosed and learns the reason from the read, which is
    // the one place that distinction is documented.
    if (readable) *readable = (p.revents & (POLLIN | POLLHUP)) != 0;
    if (writable) *writable = (p.revents & POLLOUT) != 0;
    if (p.revents & (POLLERR | POLLNVAL)) return false;
    return true;
  }
#endif
}
}  // namespace hsf
