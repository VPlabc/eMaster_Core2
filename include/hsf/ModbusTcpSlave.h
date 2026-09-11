#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "hsf/ModbusProtocol.h"

namespace hsf {

// Small single-listener Modbus TCP server. The protocol implementation is
// transport-neutral; this class owns only MBAP framing and socket lifetime.
class ModbusTcpSlave {
 public:
  explicit ModbusTcpSlave(ModbusRegisterStore& store);
  ~ModbusTcpSlave();

  bool Start(const std::string& bindAddress, int port, uint8_t unitId, std::string& error);
  void Stop();
  bool IsRunning() const { return running_.load(); }

 private:
  void Run();
  void ServeClient(intptr_t client);
  bool OpenListener(const std::string& bindAddress, int port, std::string& error);
  void CloseListener();

  ModbusProtocol protocol_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  intptr_t listener_ = -1;
  uint8_t unitId_ = 1;
};

}  // namespace hsf
