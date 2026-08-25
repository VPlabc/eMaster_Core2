#include "hsf/RuntimeVariables.h"

namespace hsf {

RuntimeVariables& RuntimeVariables::Instance() {
  static RuntimeVariables instance;
  return instance;
}

void RuntimeVariables::Set(const std::string& name, VariableValue value) {
  std::lock_guard<std::mutex> lock(mutex_);
  variables_[name] = std::move(value);
}

std::optional<VariableValue> RuntimeVariables::Get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = variables_.find(name);
  if (it == variables_.end()) return std::nullopt;
  return it->second;
}

bool RuntimeVariables::Has(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return variables_.count(name) > 0;
}

void RuntimeVariables::Remove(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  variables_.erase(name);
}

std::map<std::string, VariableValue> RuntimeVariables::All() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return variables_;
}

nlohmann::json RuntimeVariables::ToJson() const {
  nlohmann::json root = nlohmann::json::object();
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : variables_) {
    const std::string& name = entry.first;
    const VariableValue& value = entry.second;
    std::visit([&](auto&& v) { root[name] = v; }, value);
  }
  return root;
}

}  // namespace hsf
