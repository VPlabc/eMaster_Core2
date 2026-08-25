#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "hsf/ConfigManager.h"
#include "hsf/TcpSocket.h"

namespace hsf {

class ZkController;

// Card reader front-end. One background thread serves whichever source
// RfidConfig::mode selects, and IsConnected() is what the dashboard's RFID
// Reader status reflects in every case:
//
//   "tcp_json"  Connects to a TCP server that pushes a JSON object per read
//               and takes the card value from data.Raw. Auto-reconnects.
//   "zk"        Drives a ZKTeco controller through ZkController, which owns
//               its own heartbeat and auto-reconnect; this thread just
//               mirrors that connection state.
//   "card_api"  Readers POST to /api/card/input instead, so there is no
//               outbound connection to observe -- status comes from pinging
//               the reader's IP on the reconnect interval.
class RfidClient {
 public:
  using CardCallback = std::function<void(const std::string& uid)>;

  RfidClient();
  ~RfidClient();

  void Configure(const RfidConfig& config);
  RfidConfig GetConfig() const;

  // Non-owning; must outlive this object. Only used in "zk" mode, and the
  // mode is inert without it.
  void SetZkController(ZkController* zk);

  // Pulls data.Raw out of one JSON payload. Exposed for testing -- the
  // parsing is the fiddly part and shouldn't need a live reader to check.
  // Returns false if the payload isn't JSON or has no usable value.
  static bool ExtractRawFromJson(const std::string& payload, std::string& uid);

  void Start();
  void Stop();
  bool IsConnected() const;

  bool SendCommand(const std::string& command);

  void SetCardCallback(CardCallback callback);

 private:
  void RunLoop();
  void RunTcpJsonIteration(const RfidConfig& config);
  void RunZkIteration(const RfidConfig& config);
  void RunCardApiIteration(const RfidConfig& config);
  void EmitCard(const std::string& uid);

  RfidConfig config_;
  mutable std::mutex mutex_;
  TcpSocket socket_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex callbackMutex_;
  CardCallback callback_;
  ZkController* zk_ = nullptr;

  // Accumulates bytes across Receive() calls in tcp_json mode: a JSON object
  // can arrive split across TCP segments, and two can arrive coalesced in
  // one, so payloads are framed by newline rather than by read boundary.
  std::string jsonBuffer_;
};

}  // namespace hsf
