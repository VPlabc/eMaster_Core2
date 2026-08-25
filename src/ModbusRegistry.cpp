#include "hsf/ModbusRegistry.h"

#include <algorithm>

namespace hsf {

using nlohmann::json;

ModbusRegistry& ModbusRegistry::Instance() {
  static ModbusRegistry instance;
  return instance;
}

const char* ToString(ModbusSource source) {
  switch (source) {
    case ModbusSource::kDiscreteInput: return "discrete";
    case ModbusSource::kCoil: return "coil";
    default: return "auto";
  }
}

ModbusSource ParseModbusSource(const std::string& text, ModbusSource fallback) {
  // Accepts the names an operator would reasonably reach for, including the
  // raw function codes, rather than one blessed spelling.
  if (text == "coil" || text == "coils" || text == "fc01" || text == "FC01" || text == "1") {
    return ModbusSource::kCoil;
  }
  if (text == "discrete" || text == "discrete_input" || text == "disc" || text == "input" ||
      text == "fc02" || text == "FC02" || text == "2") {
    return ModbusSource::kDiscreteInput;
  }
  if (text == "auto") return ModbusSource::kAuto;
  return fallback;
}

void ModbusRegistry::UpsertUnlocked(std::vector<ModbusPoint>& points, const std::string& name, int address,
                                     ModbusSource source, int64_t owner) {
  for (auto& point : points) {
    if (point.name == name) {
      if (point.address != address || point.source != source) {
        point.address = address;
        point.source = source;
        // The name now means a different coil (or a different address space),
        // so the cached reading no longer describes it.
        point.valid = false;
        point.value = false;
        point.function_code = 0;
      }
      // Last registrant owns it. Two scripts registering the same NAME is a
      // naming collision they have to resolve between themselves -- the
      // registry keeps one row per name because that is what the dashboard
      // shows.
      point.owner = owner;
      return;
    }
  }
  ModbusPoint point;
  point.name = name;
  point.address = address;
  point.source = source;
  point.owner = owner;
  points.push_back(point);
}

void ModbusRegistry::UpdateUnlocked(std::vector<ModbusPoint>& points, const std::string& name, bool value,
                                     int functionCode) {
  for (auto& point : points) {
    if (point.name == name) {
      point.value = value;
      point.valid = true;
      point.function_code = functionCode;
      return;  // names are unique (UpsertUnlocked enforces it)
    }
  }
}

void ModbusRegistry::RegisterInput(const std::string& name, int address, ModbusSource source, int64_t owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpsertUnlocked(inputs_, name, address, source, owner);
}

void ModbusRegistry::RegisterOutput(const std::string& name, int address, int64_t owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Outputs are written as coils; there is no writable discrete-input space.
  UpsertUnlocked(outputs_, name, address, ModbusSource::kCoil, owner);
}

void ModbusRegistry::RegisterRegister(const ModbusRegisterPoint& point) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& existing : registers_) {
    if (existing.name == point.name) {
      // Keep the cached reading only if nothing about the layout moved --
      // otherwise the stored value describes a decode that no longer applies.
      const bool sameShape = existing.address == point.address &&
                              existing.input_register == point.input_register &&
                              existing.format.type == point.format.type &&
                              existing.format.byte_swap == point.format.byte_swap &&
                              existing.format.word_swap == point.format.word_swap &&
                              existing.format.length == point.format.length;
      ModbusRegisterPoint updated = point;
      if (sameShape) {
        updated.raw = existing.raw;
        updated.value = existing.value;
        updated.valid = existing.valid;
        updated.error = existing.error;
      }
      existing = updated;
      return;
    }
  }
  registers_.push_back(point);
}

std::vector<ModbusRegisterPoint> ModbusRegistry::Registers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return registers_;
}

void ModbusRegistry::UpdateRegister(const std::string& name, const std::vector<uint16_t>& raw,
                                     const nlohmann::json& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& point : registers_) {
    if (point.name == name) {
      point.raw = raw;
      point.value = value;
      point.valid = true;
      point.error.clear();
      return;
    }
  }
}

void ModbusRegistry::UpdateRegisterError(const std::string& name, const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& point : registers_) {
    if (point.name == name) {
      point.error = error;
      // `valid` deliberately survives: a transient read failure should grey
      // the row out, not erase the last value the operator saw.
      return;
    }
  }
}

bool ModbusRegistry::FindRegister(const std::string& name, ModbusRegisterPoint& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& point : registers_) {
    if (point.name == name) {
      out = point;
      return true;
    }
  }
  return false;
}

void ModbusRegistry::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  inputs_.clear();
  outputs_.clear();
  registers_.clear();
}

void ModbusRegistry::ClearOwner(int64_t owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto drop = [owner](auto& points) {
    points.erase(std::remove_if(points.begin(), points.end(),
                                 [owner](const auto& point) { return point.owner == owner; }),
                 points.end());
  };
  drop(inputs_);
  drop(outputs_);
  drop(registers_);
}

std::vector<ModbusPoint> ModbusRegistry::Inputs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputs_;
}

std::vector<ModbusPoint> ModbusRegistry::Outputs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outputs_;
}

bool ModbusRegistry::Empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputs_.empty() && outputs_.empty() && registers_.empty();
}

void ModbusRegistry::UpdateInput(const std::string& name, bool value, int functionCode) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpdateUnlocked(inputs_, name, value, functionCode);
}

void ModbusRegistry::UpdateOutput(const std::string& name, bool value) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpdateUnlocked(outputs_, name, value, 1);
}

void ModbusRegistry::SetResolvedSource(const std::string& name, ModbusSource source) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& point : inputs_) {
    if (point.name == name && point.source == ModbusSource::kAuto) {
      point.source = source;
      return;
    }
  }
}

bool ModbusRegistry::FindOutputAddress(const std::string& name, int& address) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& point : outputs_) {
    if (point.name == name) {
      address = point.address;
      return true;
    }
  }
  return false;
}

json ModbusRegistry::PointsToJson(const std::vector<ModbusPoint>& points) {
  json arr = json::array();
  for (const auto& point : points) {
    arr.push_back({{"name", point.name},
                    {"address", point.address},
                    {"value", point.value},
                    {"valid", point.valid},
                    {"source", ToString(point.source)},
                    {"function_code", point.function_code}});
  }
  return arr;
}

json ModbusRegistry::ToJson() const {
  std::lock_guard<std::mutex> lock(mutex_);

  json regs = json::array();
  for (const auto& point : registers_) {
    json raw = json::array();
    for (uint16_t word : point.raw) raw.push_back(word);
    regs.push_back({{"name", point.name},
                     {"address", point.address},
                     {"type", ToString(point.format.type)},
                     {"endian", EndianName(point.format.byte_swap, point.format.word_swap)},
                     {"length", point.format.length},
                     {"count", RegisterCount(point.format)},
                     {"function_code", point.input_register ? 4 : 3},
                     {"writable", point.writable},
                     {"unit", point.unit},
                     {"raw", raw},
                     {"value", point.valid ? point.value : json(nullptr)},
                     {"valid", point.valid},
                     {"error", point.error}});
  }

  return json{{"inputs", PointsToJson(inputs_)},
               {"outputs", PointsToJson(outputs_)},
               {"registers", regs}};
}

}  // namespace hsf
