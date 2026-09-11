#pragma once

#include <cstdint>
#include <string>

#include "hsf/ModbusRegisterStore.h"

namespace hsf {

// Transport-neutral Modbus application-protocol core. TCP and RTU adapters
// feed it a unit id and PDU and receive a response PDU, keeping all address,
// access and exception semantics in one place.
class ModbusProtocol {
 public:
  explicit ModbusProtocol(ModbusRegisterStore& store) : store_(store) {}

  bool Handle(uint8_t unitId, const std::string& requestPdu, std::string& responsePdu,
              std::string& error) const;

 private:
  static void PutU16(std::string& out, uint16_t value);
  static uint16_t GetU16(const std::string& data, size_t offset);
  static bool Need(const std::string& data, size_t size, std::string& error);
  static bool Exception(uint8_t function, uint8_t code, std::string& responsePdu);

  ModbusRegisterStore& store_;
};

}  // namespace hsf
