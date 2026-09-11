#include "hsf/ModbusRegisterStore.h"

#include <algorithm>

namespace hsf {

namespace {
constexpr int kMaxPoints = 65536;
}

ModbusRegisterStore::AreaStorage& ModbusRegisterStore::StorageUnlocked(ModbusArea area) {
  switch (area) {
    case ModbusArea::kCoils: return coils_;
    case ModbusArea::kDiscreteInputs: return discreteInputs_;
    case ModbusArea::kInputRegisters: return inputRegisters_;
    case ModbusArea::kHoldingRegisters: return holdingRegisters_;
  }
  return coils_;
}

const ModbusRegisterStore::AreaStorage& ModbusRegisterStore::StorageUnlocked(ModbusArea area) const {
  switch (area) {
    case ModbusArea::kCoils: return coils_;
    case ModbusArea::kDiscreteInputs: return discreteInputs_;
    case ModbusArea::kInputRegisters: return inputRegisters_;
    case ModbusArea::kHoldingRegisters: return holdingRegisters_;
  }
  return coils_;
}

bool ModbusRegisterStore::ValidateRange(ModbusRange range, std::string& error) {
  if (range.start < 0 || range.count <= 0 || range.start > 65535 || range.count > kMaxPoints ||
      range.start > 65536 - range.count) {
    error = "range must fit within Modbus addresses 0..65535";
    return false;
  }
  return true;
}

bool ModbusRegisterStore::ValidateAccess(ModbusArea area, bool write, std::string& error) {
  if (write && (area == ModbusArea::kDiscreteInputs || area == ModbusArea::kInputRegisters)) {
    error = "address area is read-only";
    return false;
  }
  return true;
}

bool ModbusRegisterStore::ValidateSpan(const AreaStorage& storage, int address, int quantity,
                                       std::string& error) {
  if (quantity <= 0 || address < storage.range.start || quantity > storage.range.count ||
      address > storage.range.start + storage.range.count - quantity) {
    error = "address is outside the configured register range";
    return false;
  }
  return true;
}

bool ModbusRegisterStore::Configure(ModbusArea area, ModbusRange range, std::string& error) {
  if (!ValidateRange(range, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  AreaStorage& storage = StorageUnlocked(area);
  storage.range = range;
  if (area == ModbusArea::kCoils || area == ModbusArea::kDiscreteInputs) {
    storage.bits.assign(static_cast<size_t>(range.count), false);
    storage.registers.clear();
  } else {
    storage.registers.assign(static_cast<size_t>(range.count), 0);
    storage.bits.clear();
  }
  return true;
}

ModbusRange ModbusRegisterStore::Range(ModbusArea area) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return StorageUnlocked(area).range;
}

bool ModbusRegisterStore::ReadBits(ModbusArea area, int address, int quantity,
                                   std::vector<bool>& values, std::string& error) const {
  if (area != ModbusArea::kCoils && area != ModbusArea::kDiscreteInputs) {
    error = "area is not bit storage";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const AreaStorage& storage = StorageUnlocked(area);
  if (!ValidateSpan(storage, address, quantity, error)) return false;
  const size_t offset = static_cast<size_t>(address - storage.range.start);
  values.assign(storage.bits.begin() + offset, storage.bits.begin() + offset + quantity);
  return true;
}

bool ModbusRegisterStore::WriteBits(ModbusArea area, int address, const std::vector<bool>& values,
                                    std::string& error) {
  if (!ValidateAccess(area, true, error)) return false;
  if (area != ModbusArea::kCoils) {
    error = "area is not writable coil storage";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  AreaStorage& storage = StorageUnlocked(area);
  if (!ValidateSpan(storage, address, static_cast<int>(values.size()), error)) return false;
  const size_t offset = static_cast<size_t>(address - storage.range.start);
  std::copy(values.begin(), values.end(), storage.bits.begin() + offset);
  return true;
}

bool ModbusRegisterStore::ReadRegisters(ModbusArea area, int address, int quantity,
                                        std::vector<uint16_t>& values, std::string& error) const {
  if (area != ModbusArea::kInputRegisters && area != ModbusArea::kHoldingRegisters) {
    error = "area is not register storage";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const AreaStorage& storage = StorageUnlocked(area);
  if (!ValidateSpan(storage, address, quantity, error)) return false;
  const size_t offset = static_cast<size_t>(address - storage.range.start);
  values.assign(storage.registers.begin() + offset, storage.registers.begin() + offset + quantity);
  return true;
}

bool ModbusRegisterStore::WriteRegisters(ModbusArea area, int address,
                                         const std::vector<uint16_t>& values, std::string& error) {
  if (!ValidateAccess(area, true, error)) return false;
  if (area != ModbusArea::kHoldingRegisters) {
    error = "area is not writable holding-register storage";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  AreaStorage& storage = StorageUnlocked(area);
  if (!ValidateSpan(storage, address, static_cast<int>(values.size()), error)) return false;
  const size_t offset = static_cast<size_t>(address - storage.range.start);
  std::copy(values.begin(), values.end(), storage.registers.begin() + offset);
  return true;
}

}  // namespace hsf
