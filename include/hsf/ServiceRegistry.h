#pragma once

#include <map>
#include <mutex>
#include <string>
#include <typeindex>
#include <vector>

namespace hsf {

namespace ServiceNames {
inline constexpr const char* kRest = "rest";
inline constexpr const char* kSerial = "serial";
inline constexpr const char* kSerial2 = "serial2";
inline constexpr const char* kModbus = "modbus";
inline constexpr const char* kModbusStore = "modbus.store";
inline constexpr const char* kModbusRtuMaster = "modbus.rtu.master";
inline constexpr const char* kModbusRtuSlave = "modbus.rtu.slave";
inline constexpr const char* kRfid = "rfid";
inline constexpr const char* kZk = "zk";
inline constexpr const char* kMq = "mq";
inline constexpr const char* kLua = "lua";
inline constexpr const char* kWeb = "web";
inline constexpr const char* kUpdate = "update";
inline constexpr const char* kPackages = "packages";
inline constexpr const char* kPlugins = "plugins";
}  // namespace ServiceNames

namespace ServiceCapabilities {
inline constexpr const char* kHttpClient = "http.client";
inline constexpr const char* kSerialPort = "serial.port";
inline constexpr const char* kFieldbus = "fieldbus.modbus";
inline constexpr const char* kRegisterStore = "fieldbus.modbus.register_store";
inline constexpr const char* kCardReader = "card.reader";
inline constexpr const char* kAccessController = "access.controller";
inline constexpr const char* kMessageBroker = "message.broker";
inline constexpr const char* kRuntime = "runtime.lua";
inline constexpr const char* kHttpServer = "http.server";
inline constexpr const char* kUpdateManager = "update.manager";
inline constexpr const char* kPackageManager = "package.manager";
inline constexpr const char* kPluginManager = "plugin.manager";
}  // namespace ServiceCapabilities

enum class ServiceLifecycleState {
  kRegistered,
  kConfigured,
  kRunning,
  kStopping,
  kStopped
};

enum class ServiceHealthState {
  kUnknown,
  kHealthy,
  kDegraded,
  kUnavailable
};

struct ServiceRecord {
  std::string name;
  std::string capability;
  std::type_index type = typeid(void);
  void* instance = nullptr;
  ServiceLifecycleState lifecycle = ServiceLifecycleState::kRegistered;
  ServiceHealthState health = ServiceHealthState::kUnknown;
};

class ServiceRegistry {
 public:
  template <typename T>
  void Register(const std::string& name, T* instance, const std::string& capability,
                ServiceLifecycleState lifecycle = ServiceLifecycleState::kRegistered,
                ServiceHealthState health = ServiceHealthState::kUnknown) {
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceRecord record;
    record.name = name;
    record.capability = capability;
    record.type = std::type_index(typeid(T));
    record.instance = instance;
    record.lifecycle = lifecycle;
    record.health = health;
    services_[name] = record;
  }

  template <typename T>
  T* Get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) return nullptr;
    if (it->second.type != std::type_index(typeid(T))) return nullptr;
    return static_cast<T*>(it->second.instance);
  }

  template <typename T>
  T* FindByCapability(const std::string& capability) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : services_) {
      if (entry.second.capability != capability) continue;
      if (entry.second.type != std::type_index(typeid(T))) continue;
      return static_cast<T*>(entry.second.instance);
    }
    return nullptr;
  }

  bool SetLifecycle(const std::string& name, ServiceLifecycleState lifecycle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) return false;
    it->second.lifecycle = lifecycle;
    return true;
  }

  bool SetHealth(const std::string& name, ServiceHealthState health) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) return false;
    it->second.health = health;
    return true;
  }

  std::vector<ServiceRecord> List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceRecord> out;
    out.reserve(services_.size());
    for (const auto& entry : services_) out.push_back(entry.second);
    return out;
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ServiceRecord> services_;
};

}  // namespace hsf
