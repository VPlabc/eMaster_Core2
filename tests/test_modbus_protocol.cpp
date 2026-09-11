#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "hsf/ModbusProtocol.h"

namespace {
void PutU16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value));
}

uint16_t U16(const std::string& data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) << 8) |
                               static_cast<uint8_t>(data[offset + 1]));
}
}

int main() {
  hsf::ModbusRegisterStore store;
  std::string error;
  assert(store.Configure(hsf::ModbusArea::kCoils, {0, 16}, error));
  assert(store.Configure(hsf::ModbusArea::kDiscreteInputs, {0, 16}, error));
  assert(store.Configure(hsf::ModbusArea::kInputRegisters, {0, 8}, error));
  assert(store.Configure(hsf::ModbusArea::kHoldingRegisters, {0, 8}, error));

  assert(store.WriteBits(hsf::ModbusArea::kCoils, 3, {true, false, true}, error));
  assert(store.WriteRegisters(hsf::ModbusArea::kHoldingRegisters, 2, {0x1234, 0x5678}, error));

  hsf::ModbusProtocol protocol(store);
  std::string response;
  std::string request{char(1)};
  PutU16(request, 3);
  PutU16(request, 3);
  assert(protocol.Handle(1, request, response, error));
  assert(static_cast<uint8_t>(response[0]) == 1);
  assert(static_cast<uint8_t>(response[1]) == 1);
  assert((static_cast<uint8_t>(response[2]) & 0x05) == 0x05);

  request.assign(1, char(3));
  PutU16(request, 2);
  PutU16(request, 2);
  assert(protocol.Handle(1, request, response, error));
  assert(static_cast<uint8_t>(response[0]) == 4);
  assert(U16(response, 1) == 0x1234);
  assert(U16(response, 3) == 0x5678);

  request.assign(1, char(6));
  PutU16(request, 8);
  PutU16(request, 1);
  assert(protocol.Handle(1, request, response, error));
  assert(static_cast<uint8_t>(response[0]) == 0x86);
  assert(static_cast<uint8_t>(response[1]) == 2);  // outside configured range

  request.assign(1, char(4));
  PutU16(request, 0);
  PutU16(request, 1);
  assert(protocol.Handle(1, request, response, error));
  request.assign(1, char(16));
  PutU16(request, 0);
  PutU16(request, 1);
  request.push_back(char(2));
  PutU16(request, 9);
  assert(protocol.Handle(1, request, response, error));
  assert(static_cast<uint8_t>(response[0]) == 0x90);
  assert(static_cast<uint8_t>(response[1]) == 2);  // input registers are read-only
  return 0;
}
