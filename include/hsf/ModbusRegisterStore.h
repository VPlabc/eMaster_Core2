#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hsf {

enum class ModbusArea { kCoils, kDiscreteInputs, kInputRegisters, kHoldingRegisters };

struct ModbusRange {
  int start = 0;
  int count = 0;
};

// Thread-safe process-local storage shared by Modbus slave transports and
// application code. Addresses are protocol offsets (zero-based); presentation
// prefixes such as 40001 belong at the UI boundary.
class ModbusRegisterStore {
 public:
  ModbusRegisterStore() = default;

  bool Configure(ModbusArea area, ModbusRange range, std::string& error);
  ModbusRange Range(ModbusArea area) const;

  bool ReadBits(ModbusArea area, int address, int quantity, std::vector<bool>& values,
                std::string& error) const;
  bool WriteBits(ModbusArea area, int address, const std::vector<bool>& values, std::string& error);
  bool ReadRegisters(ModbusArea area, int address, int quantity, std::vector<uint16_t>& values,
                     std::string& error) const;
  bool WriteRegisters(ModbusArea area, int address, const std::vector<uint16_t>& values,
                      std::string& error);

 private:
  struct AreaStorage {
    ModbusRange range;
    std::vector<bool> bits;
    std::vector<uint16_t> registers;
  };

  AreaStorage& StorageUnlocked(ModbusArea area);
  const AreaStorage& StorageUnlocked(ModbusArea area) const;
  static bool ValidateRange(ModbusRange range, std::string& error);
  static bool ValidateAccess(ModbusArea area, bool write, std::string& error);
  static bool ValidateSpan(const AreaStorage& storage, int address, int quantity, std::string& error);

  mutable std::mutex mutex_;
  AreaStorage coils_{{0, 0}, {}, {}};
  AreaStorage discreteInputs_{{0, 0}, {}, {}};
  AreaStorage inputRegisters_{{0, 0}, {}, {}};
  AreaStorage holdingRegisters_{{0, 0}, {}, {}};
};

}  // namespace hsf
