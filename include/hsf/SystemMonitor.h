#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace hsf {

struct SystemStats {
  double cpu_percent = 0.0;
  double ram_used_mb = 0.0;
  double ram_total_mb = 0.0;
  double disk_used_gb = 0.0;
  double disk_total_gb = 0.0;
  bool network_up = false;
  uint64_t uptime_seconds = 0;
  unsigned thread_count = 0;
};

// Samples host CPU/RAM/disk/network/uptime. Stateful because CPU percentage
// is computed as a delta between two samples, so instances should be reused
// (e.g. one long-lived SystemMonitor polled every second) rather than
// constructed per-call.
class SystemMonitor {
 public:
  SystemMonitor();

  SystemStats Sample();

  static nlohmann::json ToJson(const SystemStats& stats);

 private:
  struct CpuTicks {
    uint64_t idle = 0;
    uint64_t total = 0;
  };

  CpuTicks ReadCpuTicks();

  CpuTicks lastTicks_;
  bool haveLastTicks_ = false;
};

}  // namespace hsf
