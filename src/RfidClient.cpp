#include "hsf/RfidClient.h"

#include <chrono>

#include <nlohmann/json.hpp>

#include "hsf/Logger.h"
#include "hsf/NetPing.h"
#include "hsf/zk_controller/ZkController.h"

namespace hsf {

using nlohmann::json;

namespace {
// The ZK controller answers on the PullSDK port; keep a sane default if the
// configured port still holds a plain-TCP reader port.
constexpr int kDefaultZkPort = 4370;
constexpr int kZkConnectTimeoutMs = 2000;
}  // namespace

RfidClient::RfidClient() = default;

RfidClient::~RfidClient() { Stop(); }

void RfidClient::Configure(const RfidConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
}

RfidConfig RfidClient::GetConfig() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void RfidClient::SetZkController(ZkController* zk) { zk_ = zk; }

void RfidClient::Start() {
  if (running_.load()) return;
  running_.store(true);
  thread_ = std::thread(&RfidClient::RunLoop, this);
}

void RfidClient::Stop() {
  if (!running_.load()) return;
  running_.store(false);
  // Unblocks a Receive() sitting in tcp_json mode so the thread can notice
  // running_ went false instead of waiting out its timeout.
  socket_.Close();
  if (thread_.joinable()) thread_.join();
  connected_.store(false);
}

bool RfidClient::IsConnected() const { return connected_.load(); }

bool RfidClient::SendCommand(const std::string& command) {
  if (!connected_.load()) return false;
  return socket_.Send(command);
}

void RfidClient::SetCardCallback(CardCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  callback_ = std::move(callback);
}

void RfidClient::EmitCard(const std::string& uid) {
  if (uid.empty()) return;
  std::lock_guard<std::mutex> lock(callbackMutex_);
  if (callback_) callback_(uid);
}

/* -------------------------------------------------------------------- *
 * JSON payload parsing
 * -------------------------------------------------------------------- */

bool RfidClient::ExtractRawFromJson(const std::string& payload, std::string& uid) {
  json parsed;
  try {
    parsed = json::parse(payload);
  } catch (const std::exception&) {
    return false;
  }
  if (!parsed.is_object()) return false;

  // Documented shape is {"data": {"Raw": "..."}}. A flat {"Raw": "..."} is
  // accepted too -- readers vary, and rejecting the flat form would mean a
  // silent no-read rather than an obvious error.
  const json* holder = nullptr;
  if (parsed.contains("data") && parsed["data"].is_object()) {
    holder = &parsed["data"];
  } else if (parsed.contains("Raw")) {
    holder = &parsed;
  }
  if (!holder) return false;

  auto it = holder->find("Raw");
  if (it == holder->end()) return false;

  // Numeric card values are common; taking .dump() of a string would keep
  // the surrounding quotes, so the two cases are handled separately.
  if (it->is_string()) {
    uid = it->get<std::string>();
  } else if (it->is_number_integer()) {
    uid = std::to_string(it->get<int64_t>());
  } else if (it->is_number_unsigned()) {
    uid = std::to_string(it->get<uint64_t>());
  } else {
    return false;
  }

  return !uid.empty();
}

/* -------------------------------------------------------------------- *
 * Per-mode iterations
 * -------------------------------------------------------------------- */

void RfidClient::RunTcpJsonIteration(const RfidConfig& config) {
  if (!socket_.IsOpen()) {
    jsonBuffer_.clear();
    if (socket_.Connect(config.ip, config.port, config.timeout_ms)) {
      connected_.store(true);
      Logger::Instance().Info(LogCategory::Rfid, "Connected to RFID reader (TCP JSON) " + config.ip + ":" +
                                                       std::to_string(config.port));
    } else {
      connected_.store(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(config.reconnect_interval_ms));
      return;
    }
  }

  std::string chunk;
  if (socket_.Receive(chunk, 1024, config.timeout_ms)) {
    jsonBuffer_ += chunk;

    // Newline-framed: drain every complete payload the buffer holds, since
    // several reads can arrive in one segment.
    size_t newline;
    while ((newline = jsonBuffer_.find('\n')) != std::string::npos) {
      std::string line = jsonBuffer_.substr(0, newline);
      jsonBuffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;

      std::string uid;
      if (ExtractRawFromJson(line, uid)) {
        Logger::Instance().Info(LogCategory::Rfid, "Card read (data.Raw): " + uid);
        EmitCard(uid);
      } else {
        Logger::Instance().Warning(LogCategory::Rfid,
                                    "Ignored reader payload with no usable data.Raw value");
      }
    }

    // A peer that never sends a newline would otherwise grow this without
    // bound; a card payload is far smaller than this.
    if (jsonBuffer_.size() > 64 * 1024) {
      Logger::Instance().Warning(LogCategory::Rfid, "Reader payload buffer overflowed; discarding");
      jsonBuffer_.clear();
    }
  } else if (!socket_.IsOpen()) {
    connected_.store(false);
    Logger::Instance().Warning(LogCategory::Rfid, "RFID reader connection lost, will reconnect");
  }
  // A Receive() timeout with the socket still open is normal while idle.
}

void RfidClient::RunZkIteration(const RfidConfig& config) {
  if (!zk_) {
    connected_.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(config.reconnect_interval_ms));
    return;
  }

  // ZkController owns the heartbeat and the reconnect loop, so this only
  // kicks off the initial connect and then mirrors its state. Calling
  // Connect() again while it is already reconnecting would fight that.
  if (!zk_->IsConnected() && !zk_->IsReconnecting()) {
    int port = config.port > 0 ? config.port : kDefaultZkPort;
    zk_->SetAutoReconnect(true);
    if (zk_->Connect(config.ip, port, kZkConnectTimeoutMs, "")) {
      zk_->StartRTLog();
      Logger::Instance().Info(LogCategory::Rfid, "Connected to ZK reader " + config.ip + ":" +
                                                       std::to_string(port));
    }
  }

  connected_.store(zk_->IsConnected());
  std::this_thread::sleep_for(std::chrono::milliseconds(config.reconnect_interval_ms));
}

void RfidClient::RunCardApiIteration(const RfidConfig& config) {
  // Nothing to connect to: readers push to POST /api/card/input. Status is
  // therefore just whether the reader answers on the network. The
  // configured port is offered as an ICMP fallback, since many networks
  // drop pings outright.
  bool alive = PingHost(config.ip, config.timeout_ms, config.port);
  bool was = connected_.exchange(alive);
  if (was != alive) {
    Logger::Instance().Info(LogCategory::Rfid, std::string("Card API reader ") + config.ip +
                                                     (alive ? " is reachable" : " stopped responding"));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(config.reconnect_interval_ms));
}

void RfidClient::RunLoop() {
  std::string lastEndpoint;

  while (running_.load()) {
    // Re-read every iteration rather than using the snapshot Configure()
    // took at startup, so changing the method or address on the
    // Configuration page takes effect without restarting the gateway --
    // which is what changing it plainly implies.
    RfidConfig config = ConfigManager::Instance().GetRfid();

    // A live socket still points at the OLD endpoint, so it has to be
    // dropped when any part of the target changes; otherwise the status
    // keeps reporting on a reader nobody selected any more.
    std::string endpoint = config.mode + "|" + config.ip + "|" + std::to_string(config.port);
    if (endpoint != lastEndpoint) {
      if (!lastEndpoint.empty()) {
        Logger::Instance().Info(LogCategory::Rfid, "RFID reader settings changed -> " + endpoint);
        socket_.Close();
        connected_.store(false);
        jsonBuffer_.clear();
      }
      lastEndpoint = endpoint;
    }

    if (config.mode == "zk") {
      RunZkIteration(config);
    } else if (config.mode == "card_api") {
      RunCardApiIteration(config);
    } else {
      RunTcpJsonIteration(config);
    }
  }

  socket_.Close();
  connected_.store(false);
}

}  // namespace hsf
