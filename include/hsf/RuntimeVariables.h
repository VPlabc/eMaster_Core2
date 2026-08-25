#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace hsf {

using VariableValue = std::variant<bool, int64_t, double, std::string>;

// Thread-safe store for runtime variables produced by Lua scripts
// (DoorState, ReaderConnected, PLCConnected, LastCitizenID, APIStatus, ...).
// Exposed to Lua via SetVariable/GetVariable and to the dashboard via
// GET /api/variables.
class RuntimeVariables {
 public:
  static RuntimeVariables& Instance();

  void Set(const std::string& name, VariableValue value);
  std::optional<VariableValue> Get(const std::string& name) const;
  bool Has(const std::string& name) const;
  void Remove(const std::string& name);

  std::map<std::string, VariableValue> All() const;
  nlohmann::json ToJson() const;

 private:
  RuntimeVariables() = default;

  mutable std::mutex mutex_;
  std::map<std::string, VariableValue> variables_;
};

}  // namespace hsf
