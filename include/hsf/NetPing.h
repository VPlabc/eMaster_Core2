#pragma once

#include <string>

namespace hsf {

// Reachability check for a host the gateway does not hold a connection to.
//
// Windows uses real ICMP echo (IcmpSendEcho from iphlpapi, already linked);
// elsewhere, and whenever ICMP is unavailable or blocked, it falls back to a
// TCP connect on `fallbackPort`. The fallback matters in practice: plenty of
// networks drop ICMP entirely, and a reader that answers on its port is
// plainly reachable regardless of what the firewall does to pings.
//
// `fallbackPort` <= 0 skips the TCP fallback (ICMP result only).
bool PingHost(const std::string& host, int timeoutMs, int fallbackPort = 0);

}  // namespace hsf
