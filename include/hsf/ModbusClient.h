#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "hsf/ConfigManager.h"
#include "hsf/TcpSocket.h"

namespace hsf {

// Modbus TCP client. The gateway acts as a Modbus TCP Client (master)
// talking to a PLC that exposes input coils, output coils and holding
// registers, per the spec's default 8 input / 4 output coil layout plus a
// configurable holding register range.
//
// Speaks the Modbus TCP wire protocol (MBAP header + PDU) directly over
// TcpSocket rather than linking libmodbus: libmodbus's Windows TCP connect
// path rejects real Windows SOCKET handles (it checks `ctx->s >= FD_SETSIZE`,
// a POSIX-fd assumption that doesn't hold for Windows' kernel-handle-valued
// sockets), which made every Modbus TCP connect fail on Windows regardless
// of whether the PLC was actually reachable.
class ModbusClient {
 public:
  ModbusClient();
  ~ModbusClient();

  void Configure(const ModbusConfig& config);
  ModbusConfig GetConfig() const;

  bool Connect();
  void Disconnect();
  bool IsConnected() const;

  bool ReadCoil(int address, bool& value);
  bool WriteCoil(int address, bool value);
  bool ReadCoils(int address, int count, std::vector<bool>& values);
  bool WriteCoils(int address, const std::vector<bool>& values);

  // Discrete inputs (function code 0x02) are a SEPARATE, read-only address
  // space from coils (0x01). Plenty of PLCs expose their physical input
  // terminals only here, and answer a coil read of those same addresses
  // with an exception or with unrelated data -- so reading inputs as coils
  // silently fails or reports nonsense on that hardware. There is no write
  // counterpart: discrete inputs are read-only by definition.
  bool ReadDiscreteInput(int address, bool& value);
  bool ReadDiscreteInputs(int address, int count, std::vector<bool>& values);

  bool ReadHoldingRegister(int address, uint16_t& value);
  bool WriteHoldingRegister(int address, uint16_t value);
  bool ReadHoldingRegisters(int address, int count, std::vector<uint16_t>& values);
  bool WriteHoldingRegisters(int address, const std::vector<uint16_t>& values);

  // Input registers (function code 0x04) are to holding registers what
  // discrete inputs are to coils: a separate, READ-ONLY address space. There
  // is deliberately no write counterpart -- Modbus defines none, because the
  // space models values the device produces (measurements, statuses) rather
  // than values you give it.
  bool ReadInputRegister(int address, uint16_t& value);
  bool ReadInputRegisters(int address, int count, std::vector<uint16_t>& values);

  // One-shot connect/disconnect against `ip`:`port`, independent of any
  // ModbusClient instance's live connection. Used by the Configuration
  // page's "Test Connect" button so a candidate PLC address can be checked
  // before saving it.
  static bool TestConnect(const std::string& ip, int port, std::string& error);

  // One-shot coil read/write against `ip`:`port`, independent of any
  // ModbusClient instance's live connection — same pattern as TestConnect.
  // Backs the Lua Modbus.ReadCoil/WriteCoil overloads that take an explicit
  // IP/port instead of using the gateway's configured PLC connection.
  static bool ReadCoilOnce(const std::string& ip, int port, int address, bool& value, std::string& error);
  static bool ReadDiscreteInputOnce(const std::string& ip, int port, int address, bool& value, std::string& error);
  static bool WriteCoilOnce(const std::string& ip, int port, int address, bool value, std::string& error);

  // Block one-shots, for the Test Tools page. `discreteInputs` picks FC02
  // over FC01 -- the same distinction that makes physical inputs readable at
  // all on PLCs that map them outside the coil space.
  static bool ReadBitsOnce(const std::string& ip, int port, int address, int count, bool discreteInputs,
                            std::vector<bool>& values, std::string& error);
  static bool ReadHoldingRegistersOnce(const std::string& ip, int port, int address, int count,
                                        std::vector<uint16_t>& values, std::string& error);
  static bool ReadInputRegistersOnce(const std::string& ip, int port, int address, int count,
                                      std::vector<uint16_t>& values, std::string& error);
  static bool WriteHoldingRegisterOnce(const std::string& ip, int port, int address, uint16_t value,
                                        std::string& error);

 private:
  // Call with mutex_ held, after every transaction. A clean peer close is
  // caught by TcpSocket (recv returns 0), but a PLC that loses power or has
  // its cable pulled never closes anything -- sends vanish and reads just
  // time out, leaving the socket "open" forever. Counting consecutive
  // failures is what turns that into a detected disconnect so the caller's
  // reconnect logic can run.
  void NoteTransactionResultUnlocked(bool ok);

  mutable std::mutex mutex_;
  ModbusConfig config_;
  TcpSocket socket_;
  uint16_t transactionId_ = 0;
  bool connected_ = false;
  int consecutiveFailures_ = 0;

  // Rate-limits the connect-failure log while auto-reconnect retries.
  int connectFailureCount_ = 0;
  std::chrono::steady_clock::time_point lastConnectFailureLog_{};
};

}  // namespace hsf
