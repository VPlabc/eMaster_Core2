#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsf/ConfigManager.h"
#include "hsf/ModbusProtocol.h"
#include "hsf/SerialPort.h"

namespace hsf {

// Modbus RTU framing shared by RTU master and slave transports. The PDU is
// identical to TCP; RTU adds unit id and little-endian CRC16 on the wire.
uint16_t ModbusRtuCrc16(const std::string& data);
std::string BuildModbusRtuFrame(uint8_t unitId, const std::string& pdu);
bool ParseModbusRtuFrame(const std::string& frame, uint8_t& unitId, std::string& pdu,
                         std::string& error);

class ModbusRtuMaster {
 public:
  explicit ModbusRtuMaster(SerialPort& port) : port_(port) {}
  bool Start(const SerialConfig& config, std::string& error);
  void Stop();
  bool IsRunning() const { return running_; }
  bool Transact(uint8_t unitId, const std::string& requestPdu, std::string& responsePdu,
                int timeoutMs, std::string& error);
  bool ReadBits(uint8_t unitId, int address, int count, bool discreteInputs,
                std::vector<bool>& values, int timeoutMs, std::string& error);
  bool WriteCoil(uint8_t unitId, int address, bool value, int timeoutMs, std::string& error);
  bool ReadRegisters(uint8_t unitId, int address, int count, bool inputRegisters,
                     std::vector<uint16_t>& values, int timeoutMs, std::string& error);
  bool WriteHoldingRegister(uint8_t unitId, int address, uint16_t value,
                            int timeoutMs, std::string& error);
  bool WriteHoldingRegisters(uint8_t unitId, int address, const std::vector<uint16_t>& values,
                             int timeoutMs, std::string& error);

 private:
  static int ExpectedFrameSize(const std::string& bytes, bool request);
  SerialPort& port_;
  mutable std::mutex mutex_;
  bool running_ = false;
};

class ModbusRtuSlave {
 public:
  explicit ModbusRtuSlave(ModbusRegisterStore& store) : protocol_(store) {}
  ~ModbusRtuSlave();
  bool Start(const SerialConfig& config, uint8_t unitId, std::string& error);
  void Stop();
  bool IsRunning() const { return running_; }

 private:
  void Run();
  static int ExpectedFrameSize(const std::string& bytes);
  ModbusProtocol protocol_;
  SerialPort port_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  uint8_t unitId_ = 1;
};

}  // namespace hsf
