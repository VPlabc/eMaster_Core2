#include <cassert>
#include <cstdint>
#include <string>

#include "hsf/ModbusRtu.h"

int main() {
  const std::string pdu{char(3), char(0), char(0), char(0), char(2)};
  const std::string frame = hsf::BuildModbusRtuFrame(7, pdu);
  uint8_t unit = 0;
  std::string decoded;
  std::string error;
  assert(hsf::ParseModbusRtuFrame(frame, unit, decoded, error));
  assert(unit == 7);
  assert(decoded == pdu);
  std::string bad = frame;
  bad.back() = static_cast<char>(static_cast<uint8_t>(bad.back()) ^ 1);
  assert(!hsf::ParseModbusRtuFrame(bad, unit, decoded, error));
  return 0;
}
