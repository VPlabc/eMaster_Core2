#include "hsf/ModbusTcpSlave.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using ModbusSocket = SOCKET;
static constexpr ModbusSocket kInvalidModbusSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using ModbusSocket = int;
static constexpr ModbusSocket kInvalidModbusSocket = -1;
#endif

namespace hsf {

namespace {
#if defined(_WIN32)
void EnsureWinsock() {
  static bool initialized = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)initialized;
}
void CloseSocket(ModbusSocket socket) { closesocket(socket); }
void ShutdownSocket(ModbusSocket socket) { shutdown(socket, SD_BOTH); }
#else
void EnsureWinsock() {}
void CloseSocket(ModbusSocket socket) { close(socket); }
void ShutdownSocket(ModbusSocket socket) { shutdown(socket, SHUT_RDWR); }
#endif

bool ReceiveExact(ModbusSocket socket, char* data, size_t size) {
  size_t received = 0;
  while (received < size) {
    const int n = recv(socket, data + received, static_cast<int>(size - received), 0);
    if (n <= 0) return false;
    received += static_cast<size_t>(n);
  }
  return true;
}

bool SendExact(ModbusSocket socket, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

uint16_t ReadU16(const char* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(data[0])) << 8) |
                               static_cast<uint8_t>(data[1]));
}

void PutU16(std::string& data, uint16_t value) {
  data.push_back(static_cast<char>(value >> 8));
  data.push_back(static_cast<char>(value));
}
}  // namespace

ModbusTcpSlave::ModbusTcpSlave(ModbusRegisterStore& store) : protocol_(store) {}

ModbusTcpSlave::~ModbusTcpSlave() { Stop(); }

bool ModbusTcpSlave::OpenListener(const std::string& bindAddress, int port, std::string& error) {
  EnsureWinsock();
  ModbusSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidModbusSocket) {
    error = "could not create Modbus TCP listener";
    return false;
  }

  int reuse = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  const std::string host = bindAddress.empty() ? "0.0.0.0" : bindAddress;
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
      bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(socket, 8) != 0) {
    error = "could not bind/listen on " + host + ":" + std::to_string(port);
    CloseSocket(socket);
    return false;
  }
  listener_ = static_cast<intptr_t>(socket);
  return true;
}

void ModbusTcpSlave::CloseListener() {
  const ModbusSocket socket = static_cast<ModbusSocket>(listener_);
  if (socket != kInvalidModbusSocket) {
    ShutdownSocket(socket);
    CloseSocket(socket);
    listener_ = -1;
  }
}

bool ModbusTcpSlave::Start(const std::string& bindAddress, int port, uint8_t unitId, std::string& error) {
  if (port <= 0 || port > 65535) {
    error = "invalid Modbus TCP port";
    return false;
  }
  Stop();
  if (!OpenListener(bindAddress, port, error)) return false;
  unitId_ = unitId;
  running_.store(true);
  thread_ = std::thread(&ModbusTcpSlave::Run, this);
  return true;
}

void ModbusTcpSlave::Stop() {
  running_.store(false);
  CloseListener();
  if (thread_.joinable()) thread_.join();
}

void ModbusTcpSlave::Run() {
  while (running_.load()) {
    const ModbusSocket listener = static_cast<ModbusSocket>(listener_);
    if (listener == kInvalidModbusSocket) break;
    sockaddr_in clientAddress{};
#if defined(_WIN32)
    int clientLength = sizeof(clientAddress);
#else
    socklen_t clientLength = sizeof(clientAddress);
#endif
    const ModbusSocket client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
    if (client == kInvalidModbusSocket) {
      if (running_.load()) continue;
      break;
    }
    ServeClient(static_cast<intptr_t>(client));
  }
}

void ModbusTcpSlave::ServeClient(intptr_t clientValue) {
  const ModbusSocket client = static_cast<ModbusSocket>(clientValue);
  while (running_.load()) {
    char header[7]{};
    if (!ReceiveExact(client, header, sizeof(header))) break;
    const uint16_t transaction = ReadU16(header);
    const uint16_t protocol = ReadU16(header + 2);
    const uint16_t length = ReadU16(header + 4);
    if (protocol != 0 || length < 2 || length > 253) break;

    std::string body(length - 1, '\0');
    if (!ReceiveExact(client, body.data(), body.size())) break;
    if (static_cast<uint8_t>(body[0]) != unitId_) continue;

    std::string responsePdu;
    std::string error;
    if (!protocol_.Handle(static_cast<uint8_t>(body[0]), body.substr(1), responsePdu, error)) break;
    std::string response;
    PutU16(response, transaction);
    PutU16(response, 0);
    // MBAP length counts the unit identifier plus PDU, but not the six-byte
    // transaction/protocol/length prefix itself.
    PutU16(response, static_cast<uint16_t>(responsePdu.size() + 1));
    response.push_back(static_cast<char>(unitId_));
    response += responsePdu;
    if (!SendExact(client, response)) break;
  }
  ShutdownSocket(client);
  CloseSocket(client);
}

}  // namespace hsf
