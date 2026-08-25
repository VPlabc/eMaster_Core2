#include "hsf/plugin_manager/PluginTransports.h"

#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <termios.h>
#  include <unistd.h>
#endif

#include "hsf/Logger.h"

namespace hsf {
namespace {

HSFStr Borrow(const std::string& s) {
  HSFStr r;
  r.ptr = s.c_str();
  r.len = s.size();
  return r;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

}  // namespace

// --- TcpTransport ------------------------------------------------------------

TcpTransport::TcpTransport(std::string host, int port, int connect_timeout_ms)
    : host_(std::move(host)),
      port_(port),
      connect_timeout_ms_(connect_timeout_ms > 0 ? connect_timeout_ms : 3000) {}

HSFTransportRef TcpTransport::Ref() {
  static const HSFTransportVTable vt = [] {
    HSFTransportVTable v{};
    v.struct_size = static_cast<uint32_t>(sizeof(HSFTransportVTable));

    v.kind = [](const HSFTransport*) { return HSF_TRANSPORT_TCP; };
    v.framing = [](const HSFTransport*) { return HSF_FRAMING_STREAM; };

    v.open = [](HSFTransport* t) -> HSFStatus {
      TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (me->socket_.IsOpen()) return HSF_ERR_ALREADY_OPEN;
      if (me->host_.empty()) {
        me->last_error_ = "no host configured";
        return HSF_ERR_CONFIG;
      }
      // Connect blocks up to the timeout. That is deliberate and is the one
      // blocking call in a non-blocking transport: a driver's start() is
      // allowed to take time, and an asynchronous connect would need a state
      // machine in every driver to no benefit.
      if (!me->socket_.Connect(me->host_, me->port_, me->connect_timeout_ms_)) {
        me->last_error_ = "cannot reach " + me->host_ + ":" + std::to_string(me->port_);
        return HSF_ERR_IO;
      }
      // Only now, and only on this socket. Existing blocking callers elsewhere
      // are unaffected because each owns its own TcpSocket.
      me->socket_.SetBlocking(false);
      me->last_error_.clear();
      return HSF_OK;
    };

    v.close = [](HSFTransport* t) {
      TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      me->socket_.Close();
    };

    v.is_open = [](const HSFTransport* t) -> int32_t {
      const TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return me->socket_.IsOpen() ? 1 : 0;
    };

    v.read = [](HSFTransport* t, void* buf, size_t cap, size_t* transferred) -> HSFStatus {
      TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (transferred) *transferred = 0;
      if (!me->socket_.IsOpen()) return HSF_ERR_NOT_OPEN;
      if (!buf || cap == 0) return HSF_ERR_INVALID_ARG;

      size_t got = 0;
      switch (me->socket_.TryReceive(buf, cap, &got)) {
        case TcpSocket::IoStatus::kOk:
          if (transferred) *transferred = got;
          return HSF_OK;
        case TcpSocket::IoStatus::kClosed:
          // HSF_OK with zero bytes IS the ABI's "peer closed" on a stream.
          // Reporting HSF_ERR_CLOSED here would be a second way to say it and
          // drivers would have to handle both.
          return HSF_OK;
        case TcpSocket::IoStatus::kWouldBlock:
          return HSF_AGAIN;
        case TcpSocket::IoStatus::kError:
        default:
          me->last_error_ = "read failed on " + me->host_;
          return HSF_ERR_IO;
      }
    };

    v.write = [](HSFTransport* t, const void* buf, size_t len, size_t* transferred) -> HSFStatus {
      TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (transferred) *transferred = 0;
      if (!me->socket_.IsOpen()) return HSF_ERR_NOT_OPEN;
      if (!buf || len == 0) return HSF_ERR_INVALID_ARG;

      size_t sent = 0;
      switch (me->socket_.TrySend(buf, len, &sent)) {
        case TcpSocket::IoStatus::kOk:
          if (transferred) *transferred = sent;
          return HSF_OK;
        case TcpSocket::IoStatus::kWouldBlock:
          return HSF_AGAIN;
        case TcpSocket::IoStatus::kClosed:
        case TcpSocket::IoStatus::kError:
        default:
          me->last_error_ = "write failed on " + me->host_;
          return HSF_ERR_IO;
      }
    };

    v.wait = [](HSFTransport* t, uint32_t events, int32_t timeout_ms,
                uint32_t* ready) -> HSFStatus {
      TcpTransport* me = Me(t);
      // NOT holding the mutex across the wait: a wait with a long timeout would
      // otherwise block every other call on this transport, including close(),
      // and a driver shutting down has to be able to interrupt a poll.
      bool readable = false, writable = false;
      const bool ok = me->socket_.Wait((events & HSF_IO_READ) != 0,
                                        (events & HSF_IO_WRITE) != 0, timeout_ms, &readable,
                                        &writable);
      uint32_t out = 0;
      if (readable) out |= HSF_IO_READ;
      if (writable) out |= HSF_IO_WRITE;
      if (ready) *ready = out;
      if (!ok || out == 0) return HSF_ERR_TIMEOUT;
      return HSF_OK;
    };

    v.flush_input = [](HSFTransport* t) -> HSFStatus {
      TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (!me->socket_.IsOpen()) return HSF_ERR_NOT_OPEN;
      // Drain whatever is buffered. Bounded so a peer flooding us cannot keep
      // this spinning.
      char scratch[512];
      for (int i = 0; i < 64; ++i) {
        size_t got = 0;
        const TcpSocket::IoStatus st = me->socket_.TryReceive(scratch, sizeof(scratch), &got);
        if (st != TcpSocket::IoStatus::kOk || got == 0) break;
      }
      return HSF_OK;
    };

    v.native_handle = [](const HSFTransport* t) -> intptr_t {
      const TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return me->socket_.NativeHandle();
    };

    v.last_error = [](const HSFTransport* t) -> HSFStr {
      const TcpTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return Borrow(me->last_error_);
    };
    return v;
  }();

  HSFTransportRef r;
  r.vt = &vt;
  r.self = reinterpret_cast<HSFTransport*>(this);
  return r;
}

// --- SerialTransport ---------------------------------------------------------

SerialTransport::SerialTransport(Settings settings) : settings_(std::move(settings)) {}

SerialTransport::~SerialTransport() { CloseDevice(); }

#if defined(_WIN32)

bool SerialTransport::OpenDevice() {
  if (handle_ != -1) return true;
  if (settings_.device.empty()) {
    last_error_ = "no device configured";
    return false;
  }
  // \\.\ prefix, or COM10 and above cannot be opened at all -- a classic
  // Windows serial trap that only shows up once a machine has ten ports.
  std::string path = settings_.device;
  if (path.rfind("\\\\.\\", 0) != 0) path = "\\\\.\\" + path;

  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    last_error_ = "cannot open " + settings_.device;
    return false;
  }

  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(h, &dcb)) {
    CloseHandle(h);
    last_error_ = "GetCommState failed on " + settings_.device;
    return false;
  }
  dcb.BaudRate = static_cast<DWORD>(settings_.baud);
  dcb.ByteSize = static_cast<BYTE>(settings_.data_bits);
  dcb.StopBits = settings_.stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
  switch (settings_.parity) {
    case 'E': case 'e': dcb.Parity = EVENPARITY; dcb.fParity = TRUE; break;
    case 'O': case 'o': dcb.Parity = ODDPARITY;  dcb.fParity = TRUE; break;
    default:            dcb.Parity = NOPARITY;   dcb.fParity = FALSE; break;
  }
  dcb.fBinary = TRUE;
  if (!SetCommState(h, &dcb)) {
    CloseHandle(h);
    last_error_ = "SetCommState failed on " + settings_.device;
    return false;
  }

  // All zeros except ReadIntervalTimeout = MAXDWORD, which is the documented
  // way to make ReadFile return immediately with whatever is buffered. That is
  // exactly the non-blocking read this transport promises; without it ReadFile
  // blocks and the whole contract breaks.
  COMMTIMEOUTS timeouts{};
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = 0;
  timeouts.WriteTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 0;
  SetCommTimeouts(h, &timeouts);

  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
  handle_ = reinterpret_cast<intptr_t>(h);
  last_error_.clear();
  return true;
}

void SerialTransport::CloseDevice() {
  if (handle_ == -1) return;
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
  handle_ = -1;
}

#else

bool SerialTransport::OpenDevice() {
  if (handle_ != -1) return true;
  if (settings_.device.empty()) {
    last_error_ = "no device configured";
    return false;
  }
  // O_NONBLOCK on open as well as on the fd: without it, opening a port with no
  // carrier can block indefinitely. O_NOCTTY so a serial console cannot become
  // this process's controlling terminal.
  const int fd = ::open(settings_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    last_error_ = "cannot open " + settings_.device + ": " + std::strerror(errno);
    return false;
  }

  termios tty{};
  if (tcgetattr(fd, &tty) != 0) {
    ::close(fd);
    last_error_ = "tcgetattr failed on " + settings_.device;
    return false;
  }

  cfmakeraw(&tty);   // no line discipline: this is a byte transport
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
  switch (settings_.data_bits) {
    case 7:  tty.c_cflag |= CS7; break;
    default: tty.c_cflag |= CS8; break;
  }
  if (settings_.stop_bits == 2) tty.c_cflag |= CSTOPB;
  else                          tty.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);

  switch (settings_.parity) {
    case 'E': case 'e': tty.c_cflag |= PARENB; tty.c_cflag &= ~static_cast<tcflag_t>(PARODD); break;
    case 'O': case 'o': tty.c_cflag |= PARENB; tty.c_cflag |= PARODD; break;
    default:            tty.c_cflag &= ~static_cast<tcflag_t>(PARENB); break;
  }

  // VMIN/VTIME both 0: read() returns immediately with whatever is available,
  // which is the non-blocking behaviour this transport promises.
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  speed_t speed;
  switch (settings_.baud) {
    case 1200:   speed = B1200;   break;
    case 2400:   speed = B2400;   break;
    case 4800:   speed = B4800;   break;
    case 9600:   speed = B9600;   break;
    case 19200:  speed = B19200;  break;
    case 38400:  speed = B38400;  break;
    case 57600:  speed = B57600;  break;
    case 115200: speed = B115200; break;
    case 230400: speed = B230400; break;
    default:
      ::close(fd);
      last_error_ = "unsupported baud rate " + std::to_string(settings_.baud);
      return false;
  }
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    ::close(fd);
    last_error_ = "tcsetattr failed on " + settings_.device;
    return false;
  }
  tcflush(fd, TCIOFLUSH);
  handle_ = fd;
  last_error_.clear();
  return true;
}

void SerialTransport::CloseDevice() {
  if (handle_ == -1) return;
  ::close(static_cast<int>(handle_));
  handle_ = -1;
}

#endif

HSFTransportRef SerialTransport::Ref() {
  static const HSFTransportVTable vt = [] {
    HSFTransportVTable v{};
    v.struct_size = static_cast<uint32_t>(sizeof(HSFTransportVTable));

    v.kind = [](const HSFTransport*) { return HSF_TRANSPORT_SERIAL; };
    v.framing = [](const HSFTransport*) { return HSF_FRAMING_STREAM; };

    v.open = [](HSFTransport* t) -> HSFStatus {
      SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (me->handle_ != -1) return HSF_ERR_ALREADY_OPEN;
      return me->OpenDevice() ? HSF_OK : HSF_ERR_IO;
    };

    v.close = [](HSFTransport* t) {
      SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      me->CloseDevice();
    };

    v.is_open = [](const HSFTransport* t) -> int32_t {
      const SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return me->handle_ != -1 ? 1 : 0;
    };

    v.read = [](HSFTransport* t, void* buf, size_t cap, size_t* transferred) -> HSFStatus {
      SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (transferred) *transferred = 0;
      if (me->handle_ == -1) return HSF_ERR_NOT_OPEN;
      if (!buf || cap == 0) return HSF_ERR_INVALID_ARG;
#if defined(_WIN32)
      DWORD got = 0;
      if (!ReadFile(reinterpret_cast<HANDLE>(me->handle_), buf, static_cast<DWORD>(cap), &got,
                    nullptr)) {
        me->last_error_ = "ReadFile failed on " + me->settings_.device;
        return HSF_ERR_IO;
      }
      // Zero bytes on a serial port means "nothing yet", NOT end of stream --
      // the opposite of a socket. A port with nothing on it is idle, not shut,
      // so this must be HSF_AGAIN or a driver would abandon a healthy line.
      if (got == 0) return HSF_AGAIN;
      if (transferred) *transferred = static_cast<size_t>(got);
      return HSF_OK;
#else
      for (;;) {
        const ssize_t n = ::read(static_cast<int>(me->handle_), buf, cap);
        if (n > 0) {
          if (transferred) *transferred = static_cast<size_t>(n);
          return HSF_OK;
        }
        if (n == 0) return HSF_AGAIN;   // idle, not closed; see above
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return HSF_AGAIN;
        me->last_error_ = std::string("read failed on ") + me->settings_.device + ": " +
                          std::strerror(errno);
        return HSF_ERR_IO;
      }
#endif
    };

    v.write = [](HSFTransport* t, const void* buf, size_t len, size_t* transferred) -> HSFStatus {
      SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (transferred) *transferred = 0;
      if (me->handle_ == -1) return HSF_ERR_NOT_OPEN;
      if (!buf || len == 0) return HSF_ERR_INVALID_ARG;
#if defined(_WIN32)
      DWORD sent = 0;
      if (!WriteFile(reinterpret_cast<HANDLE>(me->handle_), buf, static_cast<DWORD>(len), &sent,
                     nullptr)) {
        me->last_error_ = "WriteFile failed on " + me->settings_.device;
        return HSF_ERR_IO;
      }
      if (sent == 0) return HSF_AGAIN;
      if (transferred) *transferred = static_cast<size_t>(sent);
      return HSF_OK;
#else
      for (;;) {
        const ssize_t n = ::write(static_cast<int>(me->handle_), buf, len);
        if (n > 0) {
          if (transferred) *transferred = static_cast<size_t>(n);
          return HSF_OK;
        }
        if (n == 0) return HSF_AGAIN;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return HSF_AGAIN;
        me->last_error_ = std::string("write failed on ") + me->settings_.device + ": " +
                          std::strerror(errno);
        return HSF_ERR_IO;
      }
#endif
    };

    v.wait = [](HSFTransport* t, uint32_t events, int32_t timeout_ms,
                uint32_t* ready) -> HSFStatus {
      SerialTransport* me = Me(t);
      intptr_t h;
      {
        std::lock_guard<std::mutex> lock(me->mutex_);
        h = me->handle_;
      }
      if (h == -1) return HSF_ERR_NOT_OPEN;
      uint32_t out = 0;
#if defined(_WIN32)
      // No poll() for a COM handle without overlapped I/O. Since reads return
      // immediately (ReadIntervalTimeout = MAXDWORD), report ready and let the
      // caller's read say HSF_AGAIN if there is nothing. Honest, and it keeps
      // the driver's retry loop working -- the alternative, WaitCommEvent, needs
      // overlapped handles throughout and buys nothing here.
      (void)timeout_ms;
      out = events & (HSF_IO_READ | HSF_IO_WRITE);
#else
      struct pollfd p{};
      p.fd = static_cast<int>(h);
      p.events = static_cast<short>(((events & HSF_IO_READ) ? POLLIN : 0) |
                                     ((events & HSF_IO_WRITE) ? POLLOUT : 0));
      for (;;) {
        const int r = poll(&p, 1, timeout_ms);
        if (r < 0) {
          if (errno == EINTR) continue;
          return HSF_ERR_IO;
        }
        if (r == 0) {
          if (ready) *ready = 0;
          return HSF_ERR_TIMEOUT;
        }
        if (p.revents & POLLIN) out |= HSF_IO_READ;
        if (p.revents & POLLOUT) out |= HSF_IO_WRITE;
        // POLLHUP on a serial port means the USB adapter was unplugged.
        if (p.revents & POLLHUP) out |= HSF_IO_CLOSED;
        if (p.revents & (POLLERR | POLLNVAL)) out |= HSF_IO_ERROR;
        break;
      }
#endif
      if (ready) *ready = out;
      return out == 0 ? HSF_ERR_TIMEOUT : HSF_OK;
    };

    v.flush_input = [](HSFTransport* t) -> HSFStatus {
      SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      if (me->handle_ == -1) return HSF_ERR_NOT_OPEN;
#if defined(_WIN32)
      PurgeComm(reinterpret_cast<HANDLE>(me->handle_), PURGE_RXCLEAR);
#else
      // Necessary after a protocol error on a shared RS485 bus: the tail of
      // someone else's reply would otherwise be parsed as the head of ours.
      tcflush(static_cast<int>(me->handle_), TCIFLUSH);
#endif
      return HSF_OK;
    };

    v.native_handle = [](const HSFTransport* t) -> intptr_t {
      const SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return me->handle_;
    };

    v.last_error = [](const HSFTransport* t) -> HSFStr {
      const SerialTransport* me = Me(t);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return Borrow(me->last_error_);
    };
    return v;
  }();

  HSFTransportRef r;
  r.vt = &vt;
  r.self = reinterpret_cast<HSFTransport*>(this);
  return r;
}

// --- TransportFactory --------------------------------------------------------

// One owned transport. A variant would be tidier, but the pointer handed to the
// plugin has to stay valid for the transport's whole life, so each lives in its
// own allocation.
struct TransportFactory::Owned {
  std::unique_ptr<TcpTransport>    tcp;
  std::unique_ptr<SerialTransport> serial;
  HSFTransport*                    self = nullptr;   // identity, for Destroy
};

TransportFactory::TransportFactory() = default;
TransportFactory::~TransportFactory() = default;

bool TransportFactory::Supports(HSFTransportKind kind) {
  // Honest about what is actually implemented. A driver asking for CAN gets a
  // clear refusal at initialize() rather than a mysterious failure at the first
  // read.
  return kind == HSF_TRANSPORT_TCP || kind == HSF_TRANSPORT_SERIAL;
}

HSFTransportRef TransportFactory::Create(const nlohmann::json& spec, std::string& error) {
  if (!spec.is_object()) {
    error = "transport spec is not a JSON object";
    return HSFTransportRef{};
  }
  const std::string type =
      Lower(spec.value("type", std::string()));
  if (type.empty()) {
    error = "transport spec has no \"type\"";
    return HSFTransportRef{};
  }

  auto owned = std::make_unique<Owned>();
  HSFTransportRef ref{};

  if (type == "tcp") {
    const std::string host = spec.value("host", spec.value("ip", std::string()));
    const int port = spec.value("port", 0);
    if (host.empty() || port <= 0 || port > 65535) {
      error = "tcp transport needs a host and a port in 1..65535";
      return HSFTransportRef{};
    }
    owned->tcp = std::make_unique<TcpTransport>(host, port, spec.value("timeout_ms", 3000));
    ref = owned->tcp->Ref();
  } else if (type == "serial") {
    SerialTransport::Settings s;
    s.device = spec.value("device", spec.value("port", std::string()));
    if (s.device.empty()) {
      error = "serial transport needs a \"device\"";
      return HSFTransportRef{};
    }
    s.baud = spec.value("baudrate", spec.value("baud", 9600));
    s.data_bits = spec.value("data_bits", 8);
    s.stop_bits = spec.value("stop_bits", 1);
    const std::string parity = Lower(spec.value("parity", std::string("none")));
    s.parity = parity.empty() ? 'N' : static_cast<char>(parity[0] - 'a' + 'A');
    if (parity == "none") s.parity = 'N';
    owned->serial = std::make_unique<SerialTransport>(std::move(s));
    ref = owned->serial->Ref();
  } else {
    error = "unsupported transport type \"" + type +
            "\" (this gateway implements tcp and serial)";
    return HSFTransportRef{};
  }

  owned->self = ref.self;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    owned_.push_back(std::move(owned));
  }
  error.clear();
  return ref;
}

void TransportFactory::Destroy(HSFTransportRef ref) {
  if (!ref.self) return;
  std::unique_ptr<Owned> detached;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < owned_.size(); ++i) {
      if (owned_[i] && owned_[i]->self == ref.self) {
        detached = std::move(owned_[i]);
        owned_.erase(owned_.begin() + static_cast<long>(i));
        break;
      }
    }
  }
  // Destroyed outside the lock: closing a port can block briefly, and a
  // concurrent Create() must not wait for it.
  detached.reset();
}

HSFTransportFactoryRef TransportFactory::Ref() {
  static const HSFTransportFactoryVTable vt = [] {
    HSFTransportFactoryVTable v{};
    v.struct_size = static_cast<uint32_t>(sizeof(HSFTransportFactoryVTable));

    v.create = [](HSFTransportFactory* self, HSFStr spec_json,
                  HSFTransportRef* out) -> HSFStatus {
      auto* me = reinterpret_cast<TransportFactory*>(self);
      if (!out) return HSF_ERR_INVALID_ARG;
      *out = HSFTransportRef{};
      const std::string text = (spec_json.ptr && spec_json.len)
                                   ? std::string(spec_json.ptr, spec_json.len)
                                   : std::string();
      nlohmann::json spec;
      try {
        spec = nlohmann::json::parse(text);
      } catch (const std::exception&) {
        return HSF_ERR_INVALID_ARG;
      }
      std::string error;
      const HSFTransportRef ref = me->Create(spec, error);
      if (!ref.vt) {
        Logger::Instance().Warning(LogCategory::Lua, "Plugin transport request refused: " + error);
        return HSF_ERR_CONFIG;
      }
      *out = ref;
      return HSF_OK;
    };

    v.destroy = [](HSFTransportFactory* self, HSFTransportRef transport) {
      reinterpret_cast<TransportFactory*>(self)->Destroy(transport);
    };

    v.supports = [](const HSFTransportFactory*, HSFTransportKind kind) -> int32_t {
      return TransportFactory::Supports(kind) ? 1 : 0;
    };
    return v;
  }();

  HSFTransportFactoryRef r;
  r.vt = &vt;
  r.self = reinterpret_cast<HSFTransportFactory*>(this);
  return r;
}

}  // namespace hsf
