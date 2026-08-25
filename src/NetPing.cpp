#include "hsf/NetPing.h"

#include "hsf/TcpSocket.h"

#if defined(_WIN32)
// winsock2 must precede windows.h, and icmpapi needs iphlpapi's types --
// this include order is load-bearing.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <vector>
#endif

namespace hsf {
namespace {

#if defined(_WIN32)
// Returns true only on a genuine echo reply. A resolvable-but-silent host,
// or one behind a firewall dropping ICMP, comes back false -- which is why
// callers pass a fallback port.
bool IcmpPing(const std::string& host, int timeoutMs) {
  // IPv4 literal only: IcmpSendEcho is IPv4, and resolving names here would
  // drag in a DNS wait that defeats the point of a quick liveness check.
  IN_ADDR addr{};
  if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) return false;

  HANDLE icmp = ::IcmpCreateFile();
  if (icmp == INVALID_HANDLE_VALUE) return false;

  // Payload plus room for ICMP_ECHO_REPLY and the reply data, per the API's
  // documented sizing requirement.
  const char payload[] = "hsf-gateway-ping";
  std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8);

  DWORD replies = ::IcmpSendEcho(icmp, addr.S_un.S_addr, const_cast<char*>(payload),
                                  static_cast<WORD>(sizeof(payload)), nullptr, reply.data(),
                                  static_cast<DWORD>(reply.size()),
                                  static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 1000));
  bool ok = false;
  if (replies > 0) {
    auto* echo = reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data());
    ok = echo->Status == IP_SUCCESS;
  }
  ::IcmpCloseHandle(icmp);
  return ok;
}
#endif

}  // namespace

bool PingHost(const std::string& host, int timeoutMs, int fallbackPort) {
  if (host.empty()) return false;

#if defined(_WIN32)
  if (IcmpPing(host, timeoutMs)) return true;
#endif

  if (fallbackPort > 0) {
    TcpSocket socket;
    if (socket.Connect(host, fallbackPort, timeoutMs)) {
      socket.Close();
      return true;
    }
  }
  return false;
}

}  // namespace hsf
