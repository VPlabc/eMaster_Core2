#include "hsf/ModbusClient.h"

#include <chrono>

#include "hsf/Logger.h"

namespace hsf {

namespace {

constexpr int kConnectTimeoutMs = 3000;
constexpr int kResponseTimeoutMs = 2000;
// Modbus TCP has no real "slave" concept when talking directly over TCP
// (as opposed to a serial gateway); 0xFF is the conventional placeholder
// unit ID for that case, matching libmodbus's own default.
constexpr uint8_t kUnitId = 0xFF;

constexpr uint8_t kReadCoils = 0x01;
// Discrete inputs are a separate, read-only address space from coils. Many
// PLCs expose physical input terminals ONLY here and answer FC01 for those
// addresses with an exception (or with unrelated coil data), which is why
// reading inputs as coils fails on hardware that maps them this way.
constexpr uint8_t kReadDiscreteInputs = 0x02;
constexpr uint8_t kReadHoldingRegisters = 0x03;
constexpr uint8_t kReadInputRegisters = 0x04;
constexpr uint8_t kWriteSingleCoil = 0x05;
constexpr uint8_t kWriteSingleRegister = 0x06;
constexpr uint8_t kWriteMultipleCoils = 0x0F;
constexpr uint8_t kWriteMultipleRegisters = 0x10;

void PushU16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value & 0xFF));
}

uint16_t ReadU16(const std::string& buf, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint8_t>(buf[offset]) << 8) | static_cast<uint8_t>(buf[offset + 1]));
}

// Reads exactly `count` bytes, looping over TcpSocket::Receive (which only
// returns whatever a single recv() call happened to yield) until the whole
// MBAP header/PDU has arrived or the overall timeout budget runs out.
bool ReadExact(TcpSocket& socket, size_t count, int timeoutMs, std::string& out) {
  out.clear();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (out.size() < count) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return false;
    std::string chunk;
    if (!socket.Receive(chunk, count - out.size(), static_cast<int>(remaining.count()))) return false;
    out += chunk;
  }
  return true;
}

// Sends a Modbus TCP request (MBAP header + function code + `data`) and
// returns the response PDU's data (i.e. with the function code stripped),
// after validating the transaction ID, protocol ID and function code, and
// checking for an exception response.
// `peerResponded` (optional) reports whether the PLC answered at all, as
// opposed to the transport failing. It matters because a Modbus *exception*
// -- illegal address, illegal function -- is a perfectly healthy device
// saying "no": the link is fine and must not be torn down. Only genuine
// transport failures (send failed, timeout, malformed frame) count toward
// the disconnect detector.
bool Transact(TcpSocket& socket, uint16_t& transactionId, uint8_t functionCode, const std::string& data,
              std::string& responseData, std::string& error, bool* peerResponded = nullptr) {
  if (peerResponded) *peerResponded = false;
  if (!socket.IsOpen()) {
    error = "not connected";
    return false;
  }

  uint16_t txId = ++transactionId;

  std::string request;
  PushU16(request, txId);
  PushU16(request, 0);  // protocol ID, always 0 for Modbus
  PushU16(request, static_cast<uint16_t>(1 + 1 + data.size()));  // unit id + function code + data
  request.push_back(static_cast<char>(kUnitId));
  request.push_back(static_cast<char>(functionCode));
  request += data;

  if (!socket.Send(request)) {
    error = "send failed";
    return false;
  }

  std::string header;
  if (!ReadExact(socket, 7, kResponseTimeoutMs, header)) {
    error = "no response (timeout)";
    return false;
  }

  uint16_t respTxId = ReadU16(header, 0);
  uint16_t protocolId = ReadU16(header, 2);
  uint16_t length = ReadU16(header, 4);

  if (respTxId != txId) {
    error = "transaction ID mismatch";
    return false;
  }
  if (protocolId != 0) {
    error = "invalid protocol ID in response";
    return false;
  }
  if (length < 2 || length > 253) {
    error = "invalid length in response";
    return false;
  }

  std::string body;
  if (!ReadExact(socket, length - 1, kResponseTimeoutMs, body)) {
    error = "incomplete response";
    return false;
  }

  // A complete, well-formed frame came back, so the device is talking to us
  // regardless of what it says next.
  if (peerResponded) *peerResponded = true;

  uint8_t respFunctionCode = static_cast<uint8_t>(body[0]);
  if (respFunctionCode == (functionCode | 0x80)) {
    uint8_t exceptionCode = body.size() > 1 ? static_cast<uint8_t>(body[1]) : 0;
    // Spelled out: "exception 0x2" tells an operator nothing, and address
    // errors are by far the most common thing to hit here.
    const char* meaning = exceptionCode == 0x01   ? " (illegal function - the PLC does not support this "
                                                    "function code)"
                          : exceptionCode == 0x02 ? " (illegal data address - no such address on this PLC)"
                          : exceptionCode == 0x03 ? " (illegal data value)"
                          : exceptionCode == 0x04 ? " (device failure)"
                                                  : "";
    error = "Modbus exception 0x" + std::to_string(exceptionCode) + meaning;
    return false;
  }
  if (respFunctionCode != functionCode) {
    error = "unexpected function code in response";
    return false;
  }

  responseData = body.substr(1);
  return true;
}

// FC01 (read coils) and FC02 (read discrete inputs) have identical request
// and response layouts -- address, quantity, then a packed bit array -- so
// they share one implementation and differ only by function code.
bool DoReadBits(TcpSocket& socket, uint16_t& transactionId, uint8_t functionCode, int address, int count,
                 std::vector<bool>& values, std::string& error, bool* peerResponded = nullptr) {
  if (count <= 0) return false;
  std::string data;
  PushU16(data, static_cast<uint16_t>(address));
  PushU16(data, static_cast<uint16_t>(count));

  std::string response;
  if (!Transact(socket, transactionId, functionCode, data, response, error, peerResponded)) return false;
  if (response.empty()) {
    error = "empty response";
    return false;
  }

  values.clear();
  values.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    size_t byteIndex = 1 + static_cast<size_t>(i / 8);
    if (byteIndex >= response.size()) break;
    uint8_t byte = static_cast<uint8_t>(response[byteIndex]);
    values.push_back((byte >> (i % 8)) & 0x01);
  }
  return true;
}

bool DoWriteCoil(TcpSocket& socket, uint16_t& transactionId, int address, bool value, std::string& error, bool* peerResponded = nullptr) {
  std::string data;
  PushU16(data, static_cast<uint16_t>(address));
  PushU16(data, value ? 0xFF00 : 0x0000);

  std::string response;
  return Transact(socket, transactionId, kWriteSingleCoil, data, response, error, peerResponded);
}

bool DoWriteCoils(TcpSocket& socket, uint16_t& transactionId, int address, const std::vector<bool>& values,
                   std::string& error, bool* peerResponded = nullptr) {
  if (values.empty()) return false;
  int count = static_cast<int>(values.size());
  int byteCount = (count + 7) / 8;

  std::string data;
  PushU16(data, static_cast<uint16_t>(address));
  PushU16(data, static_cast<uint16_t>(count));
  data.push_back(static_cast<char>(byteCount));

  std::vector<uint8_t> packed(static_cast<size_t>(byteCount), 0);
  for (int i = 0; i < count; ++i) {
    if (values[static_cast<size_t>(i)]) packed[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1 << (i % 8));
  }
  for (uint8_t b : packed) data.push_back(static_cast<char>(b));

  std::string response;
  return Transact(socket, transactionId, kWriteMultipleCoils, data, response, error, peerResponded);
}

// FC03 (holding registers) and FC04 (input registers) have identical request
// and response layouts -- address, quantity, then a byte count and that many
// data bytes -- so they share one implementation, exactly like FC01/FC02.
bool DoReadRegisters(TcpSocket& socket, uint16_t& transactionId, uint8_t functionCode, int address, int count,
                      std::vector<uint16_t>& values, std::string& error, bool* peerResponded = nullptr) {
  if (count <= 0) return false;
  std::string data;
  PushU16(data, static_cast<uint16_t>(address));
  PushU16(data, static_cast<uint16_t>(count));

  std::string response;
  if (!Transact(socket, transactionId, functionCode, data, response, error, peerResponded)) return false;
  if (response.empty()) {
    error = "empty response";
    return false;
  }

  uint8_t byteCount = static_cast<uint8_t>(response[0]);
  int registerCount = byteCount / 2;
  values.clear();
  values.reserve(static_cast<size_t>(registerCount));
  for (int i = 0; i < registerCount; ++i) {
    size_t offset = 1 + static_cast<size_t>(i) * 2;
    if (offset + 1 >= response.size()) break;
    values.push_back(ReadU16(response, offset));
  }
  return true;
}

bool DoWriteHoldingRegister(TcpSocket& socket, uint16_t& transactionId, int address, uint16_t value,
                             std::string& error, bool* peerResponded = nullptr) {
  std::string data;
  PushU16(data, static_cast<uint16_t>(address));
  PushU16(data, value);

  std::string response;
  return Transact(socket, transactionId, kWriteSingleRegister, data, response, error, peerResponded);
}

bool DoWriteHoldingRegisters(TcpSocket& socket, uint16_t& transactionId, int address,
                              const std::vector<uint16_t>& values, std::string& error,
                              bool* peerResponded = nullptr) {
  if (values.empty()) return false;
  int count = static_cast<int>(values.size());

  std::string data;
  PushU16(data, static_cast<uint16_t>(address));
  PushU16(data, static_cast<uint16_t>(count));
  data.push_back(static_cast<char>(count * 2));
  for (uint16_t v : values) PushU16(data, v);

  std::string response;
  return Transact(socket, transactionId, kWriteMultipleRegisters, data, response, error, peerResponded);
}

}  // namespace

ModbusClient::ModbusClient() = default;

ModbusClient::~ModbusClient() { Disconnect(); }

bool ModbusClient::TestConnect(const std::string& ip, int port, std::string& error) {
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  socket.Close();
  return true;
}

namespace {
// Shared by the one-shot coil and discrete-input reads below, which differ
// only by function code.
bool ReadBitOnce(uint8_t functionCode, const std::string& ip, int port, int address, bool& value,
                  std::string& error) {
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  uint16_t transactionId = 0;
  std::vector<bool> values;
  if (!DoReadBits(socket, transactionId, functionCode, address, 1, values, error) || values.empty()) {
    socket.Close();
    return false;
  }
  value = values[0];
  socket.Close();
  return true;
}
}  // namespace

bool ModbusClient::ReadCoilOnce(const std::string& ip, int port, int address, bool& value, std::string& error) {
  return ReadBitOnce(kReadCoils, ip, port, address, value, error);
}

bool ModbusClient::ReadDiscreteInputOnce(const std::string& ip, int port, int address, bool& value,
                                          std::string& error) {
  return ReadBitOnce(kReadDiscreteInputs, ip, port, address, value, error);
}

bool ModbusClient::WriteCoilOnce(const std::string& ip, int port, int address, bool value, std::string& error) {
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  uint16_t transactionId = 0;
  bool ok = DoWriteCoil(socket, transactionId, address, value, error);
  socket.Close();
  return ok;
}

bool ModbusClient::ReadBitsOnce(const std::string& ip, int port, int address, int count, bool discreteInputs,
                                 std::vector<bool>& values, std::string& error) {
  if (count <= 0 || count > 2000) {
    error = "count must be 1..2000";
    return false;
  }
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  uint16_t transactionId = 0;
  uint8_t fc = discreteInputs ? kReadDiscreteInputs : kReadCoils;
  bool ok = DoReadBits(socket, transactionId, fc, address, count, values, error);
  socket.Close();
  return ok;
}

bool ModbusClient::ReadHoldingRegistersOnce(const std::string& ip, int port, int address, int count,
                                             std::vector<uint16_t>& values, std::string& error) {
  // 125 is the Modbus ceiling for FC03: the response byte count is a single
  // byte, so 125 registers (250 bytes) is all that fits.
  if (count <= 0 || count > 125) {
    error = "count must be 1..125";
    return false;
  }
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  uint16_t transactionId = 0;
  bool ok = DoReadRegisters(socket, transactionId, kReadHoldingRegisters, address, count, values, error);
  socket.Close();
  return ok;
}

bool ModbusClient::ReadInputRegistersOnce(const std::string& ip, int port, int address, int count,
                                           std::vector<uint16_t>& values, std::string& error) {
  if (count <= 0 || count > 125) {
    error = "count must be 1..125";
    return false;
  }
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  uint16_t transactionId = 0;
  bool ok = DoReadRegisters(socket, transactionId, kReadInputRegisters, address, count, values, error);
  socket.Close();
  return ok;
}

bool ModbusClient::WriteHoldingRegisterOnce(const std::string& ip, int port, int address, uint16_t value,
                                             std::string& error) {
  TcpSocket socket;
  if (!socket.Connect(ip, port, kConnectTimeoutMs)) {
    error = "connect failed";
    return false;
  }
  uint16_t transactionId = 0;
  bool ok = DoWriteHoldingRegister(socket, transactionId, address, value, error);
  socket.Close();
  return ok;
}

void ModbusClient::Configure(const ModbusConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) {
    socket_.Close();
    connected_ = false;
  }
  config_ = config;
}

ModbusConfig ModbusClient::GetConfig() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void ModbusClient::NoteTransactionResultUnlocked(bool ok) {
  if (ok) {
    consecutiveFailures_ = 0;
    return;
  }

  // Three in a row rather than one: a single timeout is normal on a busy or
  // slow PLC, and dropping the connection for that would cause needless
  // reconnect churn. Three consecutive failures means the link is gone.
  constexpr int kFailureThreshold = 3;
  if (++consecutiveFailures_ < kFailureThreshold) return;

  if (connected_) {
    Logger::Instance().Warning(LogCategory::Modbus,
                                "PLC unresponsive after " + std::to_string(consecutiveFailures_) +
                                    " consecutive failures; marking disconnected so it can reconnect");
    socket_.Close();
    connected_ = false;
  }
  consecutiveFailures_ = 0;
}

bool ModbusClient::Connect() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!config_.enabled) return false;

  // Must match IsConnected()'s definition, not just the flag. The socket can
  // close underneath us -- peer reset, or a failed send closing it -- which
  // leaves connected_ stale: IsConnected() correctly says false, but a
  // flag-only check here returns true and makes every reconnect attempt a
  // no-op that reports success. The connection then never actually comes
  // back while the log cheerfully announces "reconnected" each time.
  if (connected_ && socket_.IsOpen()) return true;

  // Stale state: drop whatever is left before dialling again.
  socket_.Close();
  connected_ = false;

  if (!socket_.Connect(config_.ip, config_.port, kConnectTimeoutMs)) {
    // Throttled: with auto-reconnect retrying every 5s, logging every
    // failure buries every other message in the viewer within minutes and
    // grows the log file all night for a PLC that is simply switched off.
    // The first failure is always reported; after that, one line a minute.
    ++connectFailureCount_;
    auto now = std::chrono::steady_clock::now();
    if (connectFailureCount_ == 1 || now - lastConnectFailureLog_ >= std::chrono::seconds(60)) {
      lastConnectFailureLog_ = now;
      std::string suffix = connectFailureCount_ > 1
                                ? " (" + std::to_string(connectFailureCount_) + " attempts so far)"
                                : "";
      Logger::Instance().Error(LogCategory::Modbus, "Modbus connect failed: could not reach " + config_.ip +
                                                         ":" + std::to_string(config_.port) + suffix);
    }
    return false;
  }
  connectFailureCount_ = 0;

  connected_ = true;
  transactionId_ = 0;
  consecutiveFailures_ = 0;
  Logger::Instance().Info(LogCategory::Modbus, "Connected to PLC at " + config_.ip + ":" + std::to_string(config_.port));
  return true;
}

void ModbusClient::Disconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  socket_.Close();
  connected_ = false;
}

bool ModbusClient::IsConnected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connected_ && socket_.IsOpen();
}

bool ModbusClient::ReadCoil(int address, bool& value) {
  std::vector<bool> values;
  if (!ReadCoils(address, 1, values) || values.empty()) return false;
  value = values[0];
  return true;
}

bool ModbusClient::ReadDiscreteInput(int address, bool& value) {
  std::vector<bool> values;
  if (!ReadDiscreteInputs(address, 1, values) || values.empty()) return false;
  value = values[0];
  return true;
}

bool ModbusClient::ReadDiscreteInputs(int address, int count, std::vector<bool>& values) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || count <= 0) return false;
  std::string error;
  bool responded = false;
  if (!DoReadBits(socket_, transactionId_, kReadDiscreteInputs, address, count, values, error, &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "ReadDiscreteInputs failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::WriteCoil(int address, bool value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) return false;
  std::string error;
  bool responded = false;
  if (!DoWriteCoil(socket_, transactionId_, address, value, error, &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "WriteCoil failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::ReadCoils(int address, int count, std::vector<bool>& values) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || count <= 0) return false;
  std::string error;
  bool responded = false;
  if (!DoReadBits(socket_, transactionId_, kReadCoils, address, count, values, error, &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "ReadCoils failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::WriteCoils(int address, const std::vector<bool>& values) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || values.empty()) return false;
  std::string error;
  bool responded = false;
  if (!DoWriteCoils(socket_, transactionId_, address, values, error, &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "WriteCoils failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::ReadHoldingRegister(int address, uint16_t& value) {
  std::vector<uint16_t> values;
  if (!ReadHoldingRegisters(address, 1, values) || values.empty()) return false;
  value = values[0];
  return true;
}

bool ModbusClient::WriteHoldingRegister(int address, uint16_t value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) return false;
  std::string error;
  bool responded = false;
  if (!DoWriteHoldingRegister(socket_, transactionId_, address, value, error, &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "WriteHoldingRegister failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::ReadHoldingRegisters(int address, int count, std::vector<uint16_t>& values) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || count <= 0) return false;
  std::string error;
  bool responded = false;
  if (!DoReadRegisters(socket_, transactionId_, kReadHoldingRegisters, address, count, values, error,
                        &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "ReadHoldingRegisters failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::ReadInputRegister(int address, uint16_t& value) {
  std::vector<uint16_t> values;
  if (!ReadInputRegisters(address, 1, values) || values.empty()) return false;
  value = values[0];
  return true;
}

bool ModbusClient::ReadInputRegisters(int address, int count, std::vector<uint16_t>& values) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || count <= 0) return false;
  std::string error;
  bool responded = false;
  if (!DoReadRegisters(socket_, transactionId_, kReadInputRegisters, address, count, values, error,
                        &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "ReadInputRegisters failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

bool ModbusClient::WriteHoldingRegisters(int address, const std::vector<uint16_t>& values) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || values.empty()) return false;
  std::string error;
  bool responded = false;
  if (!DoWriteHoldingRegisters(socket_, transactionId_, address, values, error, &responded)) {
    Logger::Instance().Warning(LogCategory::Modbus, "WriteHoldingRegisters failed: " + error);
    NoteTransactionResultUnlocked(responded);
    return false;
  }
  NoteTransactionResultUnlocked(true);
  return true;
}

}  // namespace hsf
