#include "hsf/ModbusRtu.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace hsf {

namespace {
void PutU16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value));
}

uint16_t GetU16(const std::string& data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) << 8) |
                               static_cast<uint8_t>(data[offset + 1]));
}
}

uint16_t ModbusRtuCrc16(const std::string& data) {
  uint16_t crc = 0xFFFF;
  for (unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : crc >> 1;
  }
  return crc;
}

std::string BuildModbusRtuFrame(uint8_t unitId, const std::string& pdu) {
  std::string frame(1, static_cast<char>(unitId));
  frame += pdu;
  const uint16_t crc = ModbusRtuCrc16(frame);
  frame.push_back(static_cast<char>(crc & 0xFF));
  frame.push_back(static_cast<char>(crc >> 8));
  return frame;
}

bool ParseModbusRtuFrame(const std::string& frame, uint8_t& unitId, std::string& pdu,
                         std::string& error) {
  if (frame.size() < 4) {
    error = "RTU frame is too short";
    return false;
  }
  const uint16_t expected = ModbusRtuCrc16(frame.substr(0, frame.size() - 2));
  const uint16_t actual = static_cast<uint16_t>(static_cast<uint8_t>(frame[frame.size() - 2]) |
                                                (static_cast<uint16_t>(static_cast<uint8_t>(frame.back())) << 8));
  if (expected != actual) {
    error = "RTU CRC16 mismatch";
    return false;
  }
  unitId = static_cast<uint8_t>(frame[0]);
  pdu.assign(frame.begin() + 1, frame.end() - 2);
  return true;
}

int ModbusRtuMaster::ExpectedFrameSize(const std::string& bytes, bool request) {
  if (bytes.size() < 2) return 0;
  const uint8_t function = static_cast<uint8_t>(bytes[1]);
  if (function & 0x80) return 5;
  if (function == 1 || function == 2 || function == 3 || function == 4) {
    if (request) return 8;
    if (bytes.size() < 3) return 0;
    return 5 + static_cast<uint8_t>(bytes[2]);
  }
  if (function == 5 || function == 6 || function == 15 || function == 16) return 8;
  return 0;
}

bool ModbusRtuMaster::Start(const SerialConfig& config, std::string& error) {
  if (config.port.empty()) {
    error = "RTU serial port is empty";
    return false;
  }
  if (!port_.Open(config)) {
    error = "could not open RTU serial port " + config.port;
    return false;
  }
  running_ = true;
  return true;
}

void ModbusRtuMaster::Stop() {
  running_ = false;
  port_.Close();
}

bool ModbusRtuMaster::Transact(uint8_t unitId, const std::string& requestPdu,
                               std::string& responsePdu, int timeoutMs, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_ || !port_.IsOpen()) {
    error = "RTU master is not running";
    return false;
  }
  if (requestPdu.empty()) {
    error = "RTU request PDU is empty";
    return false;
  }
  port_.ReadAvailable();
  if (!port_.Write(BuildModbusRtuFrame(unitId, requestPdu))) {
    error = "RTU frame could not be queued";
    return false;
  }

  std::string frame;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
  while (std::chrono::steady_clock::now() < deadline) {
    frame += port_.ReadAvailable();
    const int expected = ExpectedFrameSize(frame, false);
    if (expected > 0 && static_cast<int>(frame.size()) >= expected) {
      uint8_t responseUnit = 0;
      std::string response;
      if (!ParseModbusRtuFrame(frame.substr(0, expected), responseUnit, response, error)) return false;
      const uint8_t requestFunction = static_cast<uint8_t>(requestPdu[0]);
      if (responseUnit != unitId || response.empty() ||
          (static_cast<uint8_t>(response[0]) != requestFunction &&
           static_cast<uint8_t>(response[0]) != (requestFunction | 0x80))) {
        error = "unexpected RTU response";
        return false;
      }
      responsePdu = std::move(response);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  error = "RTU response timeout";
  return false;
}

bool ModbusRtuMaster::ReadBits(uint8_t unitId, int address, int count, bool discreteInputs,
                               std::vector<bool>& values, int timeoutMs, std::string& error) {
  if (address < 0 || address > 65535 || count <= 0 || count > 2000) {
    error = "RTU bit read must use address 0..65535 and count 1..2000";
    return false;
  }
  std::string request(1, static_cast<char>(discreteInputs ? 2 : 1));
  PutU16(request, static_cast<uint16_t>(address));
  PutU16(request, static_cast<uint16_t>(count));
  std::string response;
  if (!Transact(unitId, request, response, timeoutMs, error) || response.size() < 2) return false;
  const int bytes = static_cast<uint8_t>(response[1]);
  if (bytes != (count + 7) / 8 || response.size() != static_cast<size_t>(bytes + 2)) {
    error = "invalid RTU bit response length";
    return false;
  }
  values.assign(static_cast<size_t>(count), false);
  for (int i = 0; i < count; ++i)
    values[static_cast<size_t>(i)] = (static_cast<uint8_t>(response[2 + i / 8]) >> (i % 8)) & 1;
  return true;
}

bool ModbusRtuMaster::WriteCoil(uint8_t unitId, int address, bool value, int timeoutMs, std::string& error) {
  if (address < 0 || address > 65535) {
    error = "RTU coil address must be 0..65535";
    return false;
  }
  std::string request(1, char(5));
  PutU16(request, static_cast<uint16_t>(address));
  PutU16(request, value ? 0xFF00 : 0);
  std::string response;
  return Transact(unitId, request, response, timeoutMs, error);
}

bool ModbusRtuMaster::ReadRegisters(uint8_t unitId, int address, int count, bool inputRegisters,
                                    std::vector<uint16_t>& values, int timeoutMs, std::string& error) {
  if (address < 0 || address > 65535 || count <= 0 || count > 125) {
    error = "RTU register read must use address 0..65535 and count 1..125";
    return false;
  }
  std::string request(1, static_cast<char>(inputRegisters ? 4 : 3));
  PutU16(request, static_cast<uint16_t>(address));
  PutU16(request, static_cast<uint16_t>(count));
  std::string response;
  if (!Transact(unitId, request, response, timeoutMs, error) || response.size() < 2) return false;
  const int bytes = static_cast<uint8_t>(response[1]);
  if (bytes != count * 2 || response.size() != static_cast<size_t>(bytes + 2)) {
    error = "invalid RTU register response length";
    return false;
  }
  values.clear();
  values.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) values.push_back(GetU16(response, 2 + i * 2));
  return true;
}

bool ModbusRtuMaster::WriteHoldingRegister(uint8_t unitId, int address, uint16_t value,
                                           int timeoutMs, std::string& error) {
  if (address < 0 || address > 65535) {
    error = "RTU holding-register address must be 0..65535";
    return false;
  }
  std::string request(1, char(6));
  PutU16(request, static_cast<uint16_t>(address));
  PutU16(request, value);
  std::string response;
  return Transact(unitId, request, response, timeoutMs, error);
}

bool ModbusRtuMaster::WriteHoldingRegisters(uint8_t unitId, int address,
                                            const std::vector<uint16_t>& values, int timeoutMs,
                                            std::string& error) {
  if (address < 0 || address > 65535 || values.empty() || values.size() > 123 ||
      address > 65536 - static_cast<int>(values.size())) {
    error = "RTU register write must use address 0..65535 and count 1..123";
    return false;
  }
  std::string request(1, char(16));
  PutU16(request, static_cast<uint16_t>(address));
  PutU16(request, static_cast<uint16_t>(values.size()));
  request.push_back(static_cast<char>(values.size() * 2));
  for (uint16_t value : values) PutU16(request, value);
  std::string response;
  return Transact(unitId, request, response, timeoutMs, error);
}

ModbusRtuSlave::~ModbusRtuSlave() { Stop(); }

int ModbusRtuSlave::ExpectedFrameSize(const std::string& bytes) {
  if (bytes.size() < 2) return 0;
  const uint8_t function = static_cast<uint8_t>(bytes[1]);
  if (function == 1 || function == 2 || function == 3 || function == 4 || function == 5 || function == 6)
    return 8;
  if ((function == 15 || function == 16) && bytes.size() >= 7)
    return 9 + static_cast<uint8_t>(bytes[6]);
  return 0;
}

bool ModbusRtuSlave::Start(const SerialConfig& config, uint8_t unitId, std::string& error) {
  Stop();
  if (config.port.empty() || !port_.Open(config)) {
    error = "could not open RTU slave serial port " + config.port;
    return false;
  }
  unitId_ = unitId;
  running_.store(true);
  thread_ = std::thread(&ModbusRtuSlave::Run, this);
  return true;
}

void ModbusRtuSlave::Stop() {
  running_.store(false);
  port_.Close();
  if (thread_.joinable()) thread_.join();
}

void ModbusRtuSlave::Run() {
  std::string buffer;
  while (running_.load()) {
    buffer += port_.ReadAvailable();
    const int expected = ExpectedFrameSize(buffer);
    if (expected > 0 && static_cast<int>(buffer.size()) >= expected) {
      const std::string frame = buffer.substr(0, expected);
      buffer.erase(0, expected);
      uint8_t unit = 0;
      std::string requestPdu;
      std::string error;
      if (!ParseModbusRtuFrame(frame, unit, requestPdu, error) || unit != unitId_) continue;
      std::string responsePdu;
      if (protocol_.Handle(unit, requestPdu, responsePdu, error))
        port_.Write(BuildModbusRtuFrame(unit, responsePdu));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

}  // namespace hsf
