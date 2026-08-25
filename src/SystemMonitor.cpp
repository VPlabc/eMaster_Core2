#include "hsf/SystemMonitor.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <ifaddrs.h>
#include <net/if.h>
#else
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <dirent.h>
#endif

namespace hsf {

namespace {

bool NetworkUp() {
#if defined(_WIN32)
  ULONG size = 0;
  if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                            nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
    return false;
  }
  std::vector<unsigned char> buffer(size);
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &size) != NO_ERROR) return false;
  for (auto* aa = addresses; aa != nullptr; aa = aa->Next) {
    if (aa->OperStatus == IfOperStatusUp && aa->IfType != IF_TYPE_SOFTWARE_LOOPBACK) return true;
  }
  return false;
#else
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return false;
  bool up = false;
  for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_flags) continue;
    bool isUp = ifa->ifa_flags & IFF_UP;
    bool isLoopback = ifa->ifa_flags & IFF_LOOPBACK;
    if (isUp && !isLoopback) {
      up = true;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return up;
#endif
}

unsigned CurrentProcessThreadCount() {
#if defined(_WIN32)
  unsigned count = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  DWORD pid = GetCurrentProcessId();
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == pid) ++count;
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return count;
#elif defined(__APPLE__)
  thread_array_t threadList;
  mach_msg_type_number_t threadCount = 0;
  if (task_threads(mach_task_self(), &threadList, &threadCount) != KERN_SUCCESS) return 0;
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threadList), threadCount * sizeof(thread_t));
  return static_cast<unsigned>(threadCount);
#else
  unsigned count = 0;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return 0;
  while (readdir(dir) != nullptr) ++count;
  closedir(dir);
  return count > 2 ? count - 2 : 0;  // exclude "." and ".."
#endif
}

uint64_t SystemUptimeSeconds() {
#if defined(_WIN32)
  return GetTickCount64() / 1000;
#elif defined(__APPLE__)
  struct timeval boottime{};
  size_t size = sizeof(boottime);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &boottime, &size, nullptr, 0) != 0) return 0;
  time_t now = time(nullptr);
  return static_cast<uint64_t>(now - boottime.tv_sec);
#else
  std::ifstream in("/proc/uptime");
  double uptime = 0;
  if (in.good()) in >> uptime;
  return static_cast<uint64_t>(uptime);
#endif
}

void SampleMemory(double& usedMb, double& totalMb) {
#if defined(_WIN32)
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    totalMb = static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0);
    usedMb = totalMb - static_cast<double>(mem.ullAvailPhys) / (1024.0 * 1024.0);
  }
#elif defined(__APPLE__)
  int64_t physMem = 0;
  size_t size = sizeof(physMem);
  sysctlbyname("hw.memsize", &physMem, &size, nullptr, 0);
  totalMb = static_cast<double>(physMem) / (1024.0 * 1024.0);

  vm_size_t pageSize = 0;
  host_page_size(mach_host_self(), &pageSize);
  vm_statistics64_data_t vmStats{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vmStats), &count) ==
      KERN_SUCCESS) {
    uint64_t usedPages = vmStats.active_count + vmStats.inactive_count + vmStats.wire_count;
    usedMb = static_cast<double>(usedPages) * pageSize / (1024.0 * 1024.0);
  }
#else
  std::ifstream in("/proc/meminfo");
  std::string key;
  long value = 0;
  std::string unit;
  long memTotalKb = 0, memAvailableKb = 0;
  while (in >> key >> value >> unit) {
    if (key == "MemTotal:") memTotalKb = value;
    if (key == "MemAvailable:") memAvailableKb = value;
  }
  totalMb = memTotalKb / 1024.0;
  usedMb = (memTotalKb - memAvailableKb) / 1024.0;
#endif
}

void SampleDisk(double& usedGb, double& totalGb) {
  std::error_code ec;
  auto space = std::filesystem::space(std::filesystem::path("/"), ec);
  if (ec) return;
  totalGb = static_cast<double>(space.capacity) / (1024.0 * 1024.0 * 1024.0);
  usedGb = static_cast<double>(space.capacity - space.free) / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace

SystemMonitor::SystemMonitor() = default;

SystemMonitor::CpuTicks SystemMonitor::ReadCpuTicks() {
  CpuTicks ticks;
#if defined(_WIN32)
  FILETIME idleTime, kernelTime, userTime;
  if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
    auto toU64 = [](const FILETIME& ft) {
      return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    uint64_t idle = toU64(idleTime);
    uint64_t total = toU64(kernelTime) + toU64(userTime);
    ticks.idle = idle;
    ticks.total = total;
  }
#elif defined(__APPLE__)
  host_cpu_load_info_data_t cpuInfo{};
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpuInfo), &count) ==
      KERN_SUCCESS) {
    uint64_t total = 0;
    for (int i = 0; i < CPU_STATE_MAX; ++i) total += cpuInfo.cpu_ticks[i];
    ticks.idle = cpuInfo.cpu_ticks[CPU_STATE_IDLE];
    ticks.total = total;
  }
#else
  std::ifstream in("/proc/stat");
  std::string label;
  in >> label;  // "cpu"
  uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
  in >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  ticks.idle = idle + iowait;
  ticks.total = user + nice + system + idle + iowait + irq + softirq + steal;
#endif
  return ticks;
}

SystemStats SystemMonitor::Sample() {
  SystemStats stats;

  CpuTicks current = ReadCpuTicks();
  if (haveLastTicks_ && current.total > lastTicks_.total) {
    uint64_t totalDelta = current.total - lastTicks_.total;
    uint64_t idleDelta = current.idle - lastTicks_.idle;
    if (totalDelta > 0) {
      stats.cpu_percent = 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
    }
  }
  lastTicks_ = current;
  haveLastTicks_ = true;

  SampleMemory(stats.ram_used_mb, stats.ram_total_mb);
  SampleDisk(stats.disk_used_gb, stats.disk_total_gb);
  stats.network_up = NetworkUp();
  stats.uptime_seconds = SystemUptimeSeconds();
  stats.thread_count = CurrentProcessThreadCount();

  return stats;
}

nlohmann::json SystemMonitor::ToJson(const SystemStats& stats) {
  return nlohmann::json{
      {"cpu_percent", stats.cpu_percent},
      {"ram_used_mb", stats.ram_used_mb},
      {"ram_total_mb", stats.ram_total_mb},
      {"disk_used_gb", stats.disk_used_gb},
      {"disk_total_gb", stats.disk_total_gb},
      {"network_up", stats.network_up},
      {"uptime_seconds", stats.uptime_seconds},
      {"thread_count", stats.thread_count},
  };
}

}  // namespace hsf
