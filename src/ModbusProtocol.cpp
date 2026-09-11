#include "hsf/ModbusProtocol.h"

#include <algorithm>

namespace hsf {

namespace {
constexpr uint8_t kIllegalFunction = 0x01;
constexpr uint8_t kIllegalAddress = 0x02;
constexpr uint8_t kIllegalValue = 0x03;
constexpr uint8_t kDeviceFailure = 0x04;

bool ParseQuantity(uint16_t quantity, uint16_t max, std::string& error) {
  if (quantity == 0 || quantity > max) {
    error = "invalid Modbus quantity";
    return false;
  }
  return true;
}
}  // namespace

void ModbusProtocol::PutU16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

uint16_t ModbusProtocol::GetU16(const std::string& data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) << 8) |
                               static_cast<uint8_t>(data[offset + 1]));
}

bool ModbusProtocol::Need(const std::string& data, size_t size, std::string& error) {
  if (data.size() < size) {
    error = "truncated Modbus PDU";
    return false;
  }
  return true;
}

bool ModbusProtocol::Exception(uint8_t function, uint8_t code, std::string& responsePdu) {
  responsePdu.clear();
  responsePdu.push_back(static_cast<char>(function | 0x80));
  responsePdu.push_back(static_cast<char>(code));
  return true;
}

bool ModbusProtocol::Handle(uint8_t /*unitId*/, const std::string& requestPdu,
                            std::string& responsePdu, std::string& error) const {
  responsePdu.clear();
  error.clear();
  if (!Need(requestPdu, 1, error)) return false;
  const uint8_t function = static_cast<uint8_t>(requestPdu[0]);

  if (function == 1 || function == 2) {
    if (!Need(requestPdu, 5, error)) return false;
    const uint16_t address = GetU16(requestPdu, 1);
    const uint16_t quantity = GetU16(requestPdu, 3);
    if (!ParseQuantity(quantity, 2000, error)) return Exception(function, kIllegalValue, responsePdu);
    std::vector<bool> values;
    if (!store_.ReadBits(function == 1 ? ModbusArea::kCoils : ModbusArea::kDiscreteInputs,
                         address, quantity, values, error)) {
      return Exception(function, kIllegalAddress, responsePdu);
    }
    responsePdu.push_back(static_cast<char>((quantity + 7) / 8));
    responsePdu.resize(responsePdu.size() + (quantity + 7) / 8, '\0');
    for (size_t i = 0; i < values.size(); ++i) {
      if (values[i]) responsePdu[1 + i / 8] |= static_cast<char>(1u << (i % 8));
    }
    return true;
  }

  if (function == 3 || function == 4) {
    if (!Need(requestPdu, 5, error)) return false;
    const uint16_t address = GetU16(requestPdu, 1);
    const uint16_t quantity = GetU16(requestPdu, 3);
    if (!ParseQuantity(quantity, 125, error)) return Exception(function, kIllegalValue, responsePdu);
    std::vector<uint16_t> values;
    if (!store_.ReadRegisters(function == 3 ? ModbusArea::kHoldingRegisters : ModbusArea::kInputRegisters,
                               address, quantity, values, error)) {
      return Exception(function, kIllegalAddress, responsePdu);
    }
    responsePdu.push_back(static_cast<char>(quantity * 2));
    for (uint16_t value : values) PutU16(responsePdu, value);
    return true;
  }

  if (function == 5) {
    if (!Need(requestPdu, 5, error)) return false;
    const uint16_t address = GetU16(requestPdu, 1);
    const uint16_t raw = GetU16(requestPdu, 3);
    if (raw != 0 && raw != 0xFF00) return Exception(function, kIllegalValue, responsePdu);
    if (!store_.WriteBits(ModbusArea::kCoils, address, {raw == 0xFF00}, error))
      return Exception(function, kIllegalAddress, responsePdu);
    responsePdu = requestPdu;
    return true;
  }

  if (function == 6) {
    if (!Need(requestPdu, 5, error)) return false;
    const uint16_t address = GetU16(requestPdu, 1);
    if (!store_.WriteRegisters(ModbusArea::kHoldingRegisters, address,
                               {GetU16(requestPdu, 3)}, error))
      return Exception(function, kIllegalAddress, responsePdu);
    responsePdu = requestPdu;
    return true;
  }

  if (function == 15) {
    if (!Need(requestPdu, 6, error)) return false;
    const uint16_t address = GetU16(requestPdu, 1);
    const uint16_t quantity = GetU16(requestPdu, 3);
    const uint8_t byteCount = static_cast<uint8_t>(requestPdu[5]);
    if (!ParseQuantity(quantity, 1968, error) || byteCount != (quantity + 7) / 8 ||
        requestPdu.size() != 6 + byteCount)
      return Exception(function, kIllegalValue, responsePdu);
    std::vector<bool> values(quantity, false);
    for (size_t i = 0; i < values.size(); ++i)
      values[i] = (static_cast<uint8_t>(requestPdu[6 + i / 8]) >> (i % 8)) & 1;
    if (!store_.WriteBits(ModbusArea::kCoils, address, values, error))
      return Exception(function, kIllegalAddress, responsePdu);
    responsePdu.assign(requestPdu.begin(), requestPdu.begin() + 5);
    return true;
  }

  if (function == 16) {
    if (!Need(requestPdu, 6, error)) return false;
    const uint16_t address = GetU16(requestPdu, 1);
    const uint16_t quantity = GetU16(requestPdu, 3);
    const uint8_t byteCount = static_cast<uint8_t>(requestPdu[5]);
    if (!ParseQuantity(quantity, 123, error) || byteCount != quantity * 2 ||
        requestPdu.size() != 6 + byteCount)
      return Exception(function, kIllegalValue, responsePdu);
    std::vector<uint16_t> values;
    values.reserve(quantity);
    for (size_t i = 0; i < quantity; ++i) values.push_back(GetU16(requestPdu, 6 + i * 2));
    if (!store_.WriteRegisters(ModbusArea::kHoldingRegisters, address, values, error))
      return Exception(function, kIllegalAddress, responsePdu);
    responsePdu.assign(requestPdu.begin(), requestPdu.begin() + 5);
    return true;
  }

  return Exception(function, kIllegalFunction, responsePdu);
}

}  // namespace hsf
