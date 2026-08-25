#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "hsf/Logger.h"
#include "hsf/SerialPort.h"

namespace hsf {

namespace {
speed_t BaudToSpeed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#if defined(B230400)
    case 230400: return B230400;
#endif
    default: return B115200;
  }
}
}  // namespace

bool SerialPort::PlatformOpen(const SerialConfig& config) {
  fd_ = ::open(config.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) return false;

  termios tty{};
  if (tcgetattr(fd_, &tty) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  cfsetospeed(&tty, BaudToSpeed(config.baudrate));
  cfsetispeed(&tty, BaudToSpeed(config.baudrate));

  tty.c_cflag &= ~CSIZE;
  switch (config.data_bits) {
    case 5: tty.c_cflag |= CS5; break;
    case 6: tty.c_cflag |= CS6; break;
    case 7: tty.c_cflag |= CS7; break;
    default: tty.c_cflag |= CS8; break;
  }

  if (config.stop_bits == 2) {
    tty.c_cflag |= CSTOPB;
  } else {
    tty.c_cflag &= ~CSTOPB;
  }

  switch (config.parity) {
    case 'E':
      tty.c_cflag |= PARENB;
      tty.c_cflag &= ~PARODD;
      break;
    case 'O':
      tty.c_cflag |= PARENB;
      tty.c_cflag |= PARODD;
      break;
    default:
      tty.c_cflag &= ~PARENB;
      break;
  }

  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
  tty.c_oflag &= ~OPOST;

  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 5;  // 500ms read timeout

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  // Reads block (with the VTIME timeout above) rather than busy-polling.
  int flags = fcntl(fd_, F_GETFL, 0);
  fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);

  return true;
}

void SerialPort::PlatformClose() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// Nothing to cancel: PlatformRead() gates every read with its own 200ms
// select() timeout (see the comment there), so the read thread already returns
// on its own promptly enough for Close() to join it. The Windows backend needs
// CancelIoEx because its ReadFile has no such gate.
void SerialPort::PlatformCancelIo() {}

int SerialPort::PlatformRead(char* buffer, size_t bufferSize) {
  if (fd_ < 0) return -1;

  // Gate the blocking read with our own select() timeout rather than
  // relying solely on the termios VTIME set in PlatformOpen: VTIME is
  // honored by real UART hardware, but USB-CDC/ACM virtual serial ports
  // (and pseudo-terminals) don't reliably respect it, so a read() with no
  // incoming data can block indefinitely. That in turn hangs Close() (which
  // joins this thread) forever — and since Close() is called from inside a
  // running Lua script, it takes the whole engine down with it. select()'s
  // timeout is enforced at the file-descriptor-readiness level, independent
  // of whatever the device's VTIME behavior actually is.
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd_, &readSet);
  timeval tv{0, 200000};  // 200ms
  int selectResult = select(fd_ + 1, &readSet, nullptr, nullptr, &tv);
  if (selectResult < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  if (selectResult == 0) return 0;  // timeout, no data ready yet

  ssize_t n = ::read(fd_, buffer, bufferSize);
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return 0;
    return -1;
  }
  return static_cast<int>(n);
}

bool SerialPort::PlatformWrite(const std::string& text) {
  if (fd_ < 0) return false;
  ssize_t written = ::write(fd_, text.data(), text.size());
  return written == static_cast<ssize_t>(text.size());
}

}  // namespace hsf
