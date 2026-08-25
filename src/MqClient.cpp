#include "hsf/MqClient.h"

#include <amqpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#include "hsf/Logger.h"
#include "hsf/TcpSocket.h"

namespace hsf {

namespace {

// Bodies bigger than this are rejected rather than buffered. RabbitMQ.md draws
// the line at INT_MAX because Qt's containers are int-indexed; here it is plain
// memory safety -- a gateway with a few hundred MB of headroom must not try to
// hold a 2GB message just because a broker offered one.
constexpr uint64_t kMaxBodyBytes = 8ull * 1024 * 1024;

// Bytes per socket read. Frames are reassembled in inputBuffer either way, so
// this only trades syscalls against buffer size.
constexpr size_t kReadChunk = 16384;

// How long one service pass may block waiting for broker bytes. Also the
// worst-case latency between Publish() and the frame hitting the wire.
constexpr int kPollMs = 50;

// TcpSocket::Connect takes an IPv4 literal (it calls inet_pton), but a broker is
// far more likely to be configured by name -- "localhost" is RabbitMQ.md's own
// default. Resolving here rather than inside TcpSocket keeps a shared helper
// used by the RFID reader and the Lua Tcp.* bindings unchanged.
std::string ResolveHostToIpv4(const std::string& host) {
  if (host.empty()) return {};

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
    return {};
  }

  char text[INET_ADDRSTRLEN] = {};
  auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
  const char* converted = inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
  std::string ip = converted ? converted : "";
  freeaddrinfo(result);
  return ip;
}

AMQP::ExchangeType ExchangeTypeFromName(const std::string& name) {
  if (name == "fanout") return AMQP::fanout;
  if (name == "topic") return AMQP::topic;
  if (name == "headers") return AMQP::headers;
  return AMQP::direct;
}

std::string EndpointLabel(const MqConfig& config) {
  // Deliberately no password, here or anywhere else that logs: this string ends
  // up in the runtime log, which is readable from the web UI.
  return config.user + "@" + config.host + ":" + std::to_string(config.port) + config.vhost;
}

// One-shot handler for MqClient::TestConnect -- just enough of a
// ConnectionHandler to complete (or fail) the AMQP handshake once.
struct ProbeHandler : public AMQP::ConnectionHandler {
  TcpSocket* socket = nullptr;
  bool ready = false;
  bool errored = false;
  std::string error;

  void onData(AMQP::Connection*, const char* data, size_t size) override {
    if (!socket) return;
    if (!socket->Send(std::string(data, size))) {
      errored = true;
      if (error.empty()) error = "socket write failed";
    }
  }

  // No heartbeats for a probe that lives for three seconds.
  uint16_t onNegotiate(AMQP::Connection*, uint16_t) override { return 0; }

  void onReady(AMQP::Connection*) override { ready = true; }

  void onError(AMQP::Connection*, const char* message) override {
    errored = true;
    error = message ? message : "unknown AMQP error";
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct MqClient::Impl : public AMQP::ConnectionHandler {
  // --- configuration ------------------------------------------------------
  mutable std::mutex configMutex;
  MqConfig config;

  MqConfig Config() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config;
  }

  // --- IO thread ----------------------------------------------------------
  std::atomic<bool> running{false};
  std::thread thread;
  std::mutex wakeMutex;
  std::condition_variable wakeCv;
  bool wakeSignal = false;
  std::atomic<bool> reconfigured{false};

  // --- transport: touched ONLY on the IO thread ---------------------------
  //
  // AMQP-CPP's Connection and Channel are not thread-safe, so nothing below
  // this line may be reached from Publish(), Status() or any other public
  // entry point. That is why publishing goes through a queue.
  TcpSocket socket;
  std::string inputBuffer;
  std::unique_ptr<AMQP::Connection> connection;
  std::unique_ptr<AMQP::Channel> channel;
  bool ready = false;
  bool channelPrepared = false;
  bool consumerStarting = false;
  std::string consumerTag;
  bool failed = false;
  int negotiatedHeartbeat = 0;
  std::chrono::steady_clock::time_point lastInbound{};
  std::chrono::steady_clock::time_point lastHeartbeatSent{};
  std::string endpointLabel;

  // --- state readable from any thread -------------------------------------
  std::atomic<bool> connectedFlag{false};
  std::atomic<bool> consumingFlag{false};
  std::atomic<bool> pauseRequested{false};
  std::atomic<uint64_t> publishedCount{0};
  std::atomic<uint64_t> receivedCount{0};
  std::atomic<uint64_t> rejectedCount{0};
  std::atomic<uint64_t> droppedCount{0};
  std::atomic<uint64_t> overflowedCount{0};
  mutable std::mutex errorMutex;
  std::string lastError;

  // --- queues -------------------------------------------------------------
  struct Outgoing {
    std::string exchange;
    std::string routingKey;
    std::string body;
    std::string contentType;
  };
  mutable std::mutex outMutex;
  std::condition_variable outCv;
  std::deque<Outgoing> outgoing;

  mutable std::mutex inboxMutex;
  std::deque<MqMessage> inbox;

  std::mutex callbackMutex;
  MessageCallback callback;

  ~Impl() override = default;

  // --- AMQP::ConnectionHandler -------------------------------------------

  void onData(AMQP::Connection*, const char* data, size_t size) override {
    if (!socket.Send(std::string(data, size))) {
      Fail("socket write failed");
    }
  }

  // The broker proposes an interval; we answer with the one we will honour.
  // 0 disables heartbeats entirely, which is what a heartbeat_sec of 0 asks
  // for.
  uint16_t onNegotiate(AMQP::Connection*, uint16_t interval) override {
    const MqConfig cfg = Config();
    const int wanted = cfg.heartbeat_sec > 0 ? cfg.heartbeat_sec : 0;
    negotiatedHeartbeat = wanted;
    lastHeartbeatSent = std::chrono::steady_clock::now();
    (void)interval;
    return static_cast<uint16_t>(wanted);
  }

  void onReady(AMQP::Connection*) override {
    ready = true;
    connectedFlag.store(true);
    SetError("");
    Logger::Instance().Info(LogCategory::Mq, "Connected to broker " + endpointLabel);
  }

  void onError(AMQP::Connection*, const char* message) override {
    Fail(std::string("connection error: ") + (message ? message : "unknown"));
  }

  void onClosed(AMQP::Connection*) override {
    // Not an error in itself (this is also what a clean shutdown looks like),
    // but the connection object is finished either way and the loop has to
    // rebuild it.
    failed = true;
    connectedFlag.store(false);
    ready = false;
  }

  void onHeartbeat(AMQP::Connection*) override {
    // Nothing to do -- receiving anything at all is what keeps the liveness
    // check in Service() happy, and that is already stamped there.
  }

  // --- error bookkeeping --------------------------------------------------

  void SetError(const std::string& message) {
    std::lock_guard<std::mutex> lock(errorMutex);
    lastError = message;
  }

  std::string Error() const {
    std::lock_guard<std::mutex> lock(errorMutex);
    return lastError;
  }

  // Marks the connection unusable. The IO loop tears down and reconnects on
  // the next pass; callers of this never touch the connection themselves,
  // because Fail() can be reached from inside AMQP-CPP's own call stack.
  void Fail(const std::string& message) {
    if (!failed) {
      Logger::Instance().Error(LogCategory::Mq, "Broker " + endpointLabel + ": " + message);
    }
    SetError(message);
    failed = true;
    connectedFlag.store(false);
    consumingFlag.store(false);
    ready = false;
  }

  void Wake() {
    {
      std::lock_guard<std::mutex> lock(wakeMutex);
      wakeSignal = true;
    }
    wakeCv.notify_all();
  }

  void SleepFor(int ms) {
    std::unique_lock<std::mutex> lock(wakeMutex);
    wakeCv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return !running.load() || wakeSignal; });
    wakeSignal = false;
  }

  // --- connection lifecycle ----------------------------------------------

  bool Establish(const MqConfig& cfg) {
    endpointLabel = EndpointLabel(cfg);

    const std::string ip = ResolveHostToIpv4(cfg.host);
    if (ip.empty()) {
      SetError("cannot resolve host \"" + cfg.host + "\"");
      return false;
    }

    if (!socket.Connect(ip, cfg.port, 3000)) {
      SetError("TCP connect to " + ip + ":" + std::to_string(cfg.port) + " failed");
      return false;
    }

    inputBuffer.clear();
    ready = false;
    channelPrepared = false;
    consumerStarting = false;
    consumerTag.clear();
    failed = false;
    negotiatedHeartbeat = 0;
    lastInbound = std::chrono::steady_clock::now();
    lastHeartbeatSent = lastInbound;

    // Constructed only now that the socket is up, not before: the constructor
    // immediately emits the AMQP protocol header through onData, which needs
    // somewhere to write it (RabbitMQ.md makes the same point about
    // constructing in the socket's connected handler).
    connection = std::make_unique<AMQP::Connection>(this, AMQP::Login(cfg.user, cfg.password), cfg.vhost);

    if (failed) return false;
    return true;
  }

  void Teardown(const std::string& reason) {
    if (!reason.empty() && (connection || socket.IsOpen())) {
      Logger::Instance().Warning(LogCategory::Mq, "Broker " + endpointLabel + " disconnected: " + reason);
    }

    // Channel first: its destructor may want to send channel.close, which
    // needs both the connection and the socket to still exist.
    channel.reset();
    if (connection) {
      if (socket.IsOpen()) connection->close();
      connection.reset();
    }
    socket.Close();

    inputBuffer.clear();
    ready = false;
    channelPrepared = false;
    consumerStarting = false;
    consumerTag.clear();
    failed = false;
    connectedFlag.store(false);
    consumingFlag.store(false);
  }

  void DeclareTopology(const MqConfig& cfg) {
    // Fired back-to-back rather than nested in each other's onSuccess: AMQP
    // executes commands on a channel in order, so this is the same sequence
    // with none of the callback pyramid. Any failure trips the channel's
    // onError, which fails the whole connection -- correct, because a gateway
    // that cannot declare its topology has nowhere to publish.
    if (!cfg.exchange.empty()) {
      channel->declareExchange(cfg.exchange, ExchangeTypeFromName(cfg.exchange_type), AMQP::durable)
          .onError([this](const char* message) {
            Fail(std::string("exchange declare failed: ") + (message ? message : "unknown"));
          });
    }

    if (!cfg.queue.empty()) {
      channel->declareQueue(cfg.queue, AMQP::durable).onError([this](const char* message) {
        Fail(std::string("queue declare failed: ") + (message ? message : "unknown"));
      });

      // An empty exchange name is the default exchange, which cannot be bound
      // to (and needs no binding -- it routes by queue name).
      if (!cfg.exchange.empty()) {
        channel->bindQueue(cfg.exchange, cfg.queue, cfg.routing_key).onError([this](const char* message) {
          Fail(std::string("queue bind failed: ") + (message ? message : "unknown"));
        });
      }
    }
  }

  void StartConsuming(const MqConfig& cfg) {
    if (cfg.queue.empty()) return;

    Logger::Instance().Debug(LogCategory::Mq, "Requesting consumer on " + cfg.queue);
    consumerStarting = true;

    // Registered as separate statements rather than one chain on purpose:
    // onError() is inherited from Deferred and returns Deferred&, which has no
    // onReceived(), so a chain that puts onError in the middle does not compile.
    AMQP::DeferredConsumer& consumer = channel->consume(cfg.queue);

    consumer.onReceived([this](const AMQP::Message& message, uint64_t tag, bool redelivered) {
      OnDelivery(message, tag, redelivered);
    });

    consumer.onSuccess([this](const std::string& tag) {
      consumerTag = tag;
      consumerStarting = false;
      consumingFlag.store(true);
      Logger::Instance().Info(LogCategory::Mq, "Consuming, tag " + tag);
      // Pause may have been requested while this was still starting; honour it
      // now rather than leaving a consumer nobody asked for.
      if (pauseRequested.load()) StopConsuming();
    });

    consumer.onError([this](const char* message) {
      consumerStarting = false;
      consumingFlag.store(false);
      Fail(std::string("consume failed: ") + (message ? message : "unknown"));
    });
  }

  void StopConsuming() {
    if (consumerTag.empty()) return;
    const std::string tag = consumerTag;
    channel->cancel(tag).onSuccess([this](const std::string&) {
      consumingFlag.store(false);
      consumerTag.clear();
      Logger::Instance().Info(LogCategory::Mq, "Consumer cancelled");
      // Resumed while the cancel was in flight -- Service() will start a new
      // consumer on its next pass, which is why this only clears state.
    });
  }

  void OnDelivery(const AMQP::Message& message, uint64_t tag, bool redelivered) {
    receivedCount.fetch_add(1);
    const MqConfig cfg = Config();

    if (message.bodySize() > kMaxBodyBytes) {
      // Rejected WITHOUT requeue, here and below: a message that is too large
      // or undecodable fails identically on redelivery, so requeueing it would
      // spin forever while holding one of the prefetch slots (RabbitMQ.md
      // spells this out). Until a dead-letter exchange exists, it is dropped.
      channel->reject(tag);
      rejectedCount.fetch_add(1);
      Logger::Instance().Warning(LogCategory::Mq, "Rejected message: body of " +
                                                       std::to_string(message.bodySize()) + " bytes is too large");
      return;
    }

    MqMessage received;
    received.raw.assign(message.body(), static_cast<size_t>(message.bodySize()));
    received.exchange = message.exchange();
    received.routing_key = message.routingkey();
    received.redelivered = redelivered;
    received.delivery_tag = tag;

    if (cfg.envelope) {
      std::string error;
      if (!DecodeEnvelope(received.raw, received, error)) {
        channel->reject(tag);
        rejectedCount.fetch_add(1);
        Logger::Instance().Warning(LogCategory::Mq, "Rejected message: invalid envelope (" + error + ")");
        return;
      }
    } else {
      received.body = received.raw;
    }

    const size_t limit = cfg.inbox_limit > 0 ? static_cast<size_t>(cfg.inbox_limit) : 1;
    {
      std::lock_guard<std::mutex> lock(inboxMutex);
      while (inbox.size() >= limit) {
        inbox.pop_front();
        overflowedCount.fetch_add(1);
      }
      inbox.push_back(received);
    }

    // Acked once it is in the inbox -- the same point the Qt reference acks
    // (right after the message is committed to its model), not when a script
    // eventually reads it. Waiting for the read would stall the consumer at
    // `prefetch` unacked messages for any script that only listens for
    // OnMqMessage and never calls Mq.Get().
    channel->ack(tag);

    MessageCallback handler;
    {
      std::lock_guard<std::mutex> lock(callbackMutex);
      handler = callback;
    }
    if (handler) handler(received);
  }

  void DrainOutgoing() {
    if (!ready || !channel) return;

    // Bounded per pass so a large backlog can't starve socket reads (and
    // therefore heartbeats) on this same thread.
    constexpr int kMaxPerPass = 64;
    for (int i = 0; i < kMaxPerPass; ++i) {
      Outgoing item;
      {
        std::lock_guard<std::mutex> lock(outMutex);
        if (outgoing.empty()) break;
        item = std::move(outgoing.front());
        outgoing.pop_front();
      }

      // Envelope does NOT copy the body -- it holds the pointer -- so `item`
      // has to outlive the publish call. It does: same scope.
      AMQP::Envelope envelope(item.body.data(), item.body.size());
      envelope.setDeliveryMode(2);  // persistent, per RabbitMQ.md
      if (!item.contentType.empty()) envelope.setContentType(item.contentType);

      if (!channel->publish(item.exchange, item.routingKey, envelope)) {
        {
          std::lock_guard<std::mutex> lock(outMutex);
          outgoing.push_front(std::move(item));
        }
        Fail("publish failed");
        return;
      }

      publishedCount.fetch_add(1);
      if (failed) return;
    }

    outCv.notify_all();
  }

  void Service(const MqConfig& cfg) {
    // 1. Inbound bytes.
    std::string chunk;
    if (socket.Receive(chunk, kReadChunk, kPollMs)) {
      lastInbound = std::chrono::steady_clock::now();
      inputBuffer.append(chunk);

      // parse() returning 0 means "this frame is incomplete, keep the bytes" --
      // the one thing that makes AMQP frames split across TCP reads work
      // (RabbitMQ.md's note about MqBuffer).
      while (!inputBuffer.empty() && connection) {
        const uint64_t consumed = connection->parse(inputBuffer.data(), inputBuffer.size());
        if (consumed == 0) break;
        inputBuffer.erase(0, static_cast<size_t>(consumed));
        if (failed) return;
      }
    } else if (!socket.IsOpen()) {
      // Receive() closes the socket on EOF or error, which is what separates a
      // dropped connection from an idle one -- a plain false is just the
      // poll timeout expiring.
      Fail("connection closed by broker");
      return;
    }

    if (failed) return;

    // 2. Channel and topology, once the handshake is through.
    //
    // The Debug lines through here are not scaffolding: broker-side setup is
    // where this fails in practice (wrong vhost permissions, a queue that
    // already exists with different arguments), and those failures arrive
    // asynchronously, so knowing which step was last attempted is the
    // difference between a diagnosis and a guess.
    if (ready && !channelPrepared) {
      Logger::Instance().Debug(LogCategory::Mq, "Opening channel");
      channel = std::make_unique<AMQP::Channel>(connection.get());
      channel->onError([this](const char* message) {
        Fail(std::string("channel error: ") + (message ? message : "unknown"));
      });

      if (cfg.declare_topology) {
        Logger::Instance().Debug(LogCategory::Mq, "Declaring topology");
        DeclareTopology(cfg);
      }
      Logger::Instance().Debug(LogCategory::Mq, "Setting prefetch");
      channel->setQos(static_cast<uint16_t>(std::max(1, cfg.prefetch)));
      channelPrepared = true;
      Logger::Instance().Debug(LogCategory::Mq, "Channel ready");
      if (failed) return;
    }

    // 3. Consumer state, reconciled against Pause().
    if (ready && channelPrepared && cfg.consume) {
      const bool wantConsume = !pauseRequested.load();
      if (wantConsume && !consumingFlag.load() && !consumerStarting) {
        StartConsuming(cfg);
      } else if (!wantConsume && consumingFlag.load()) {
        StopConsuming();
      }
      if (failed) return;
    }

    // 4. Outbound queue.
    DrainOutgoing();
    if (failed) return;

    // 5. Heartbeats, both directions. Half the negotiated interval outbound
    // (the usual margin, and what the Qt reference does with interval * 500ms);
    // twice it inbound before declaring the peer dead, which is the rule the
    // AMQP spec states.
    if (negotiatedHeartbeat > 0 && connection) {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastHeartbeatSent >= std::chrono::milliseconds(negotiatedHeartbeat * 500)) {
        lastHeartbeatSent = now;
        connection->heartbeat();
      }
      if (now - lastInbound >= std::chrono::seconds(negotiatedHeartbeat * 2)) {
        Fail("no traffic or heartbeat from broker for " + std::to_string(negotiatedHeartbeat * 2) + "s");
      }
    }
  }

  void Run() {
    while (running.load()) {
      MqConfig cfg = Config();

      if (!cfg.enabled) {
        if (connection || socket.IsOpen()) Teardown("disabled");
        SleepFor(500);
        continue;
      }

      if (reconfigured.exchange(false) && (connection || socket.IsOpen())) {
        Teardown("configuration changed");
        continue;  // re-read the config before dialling again
      }

      if (!connection) {
        if (!Establish(cfg)) {
          Teardown("");
          Logger::Instance().Warning(LogCategory::Mq,
                                      "Broker " + endpointLabel + " unreachable: " + Error() + "; retrying in " +
                                          std::to_string(cfg.reconnect_interval_ms) + "ms");
          SleepFor(std::max(500, cfg.reconnect_interval_ms));
          continue;
        }
      }

      Service(cfg);

      if (failed) {
        Teardown(Error().empty() ? "connection lost" : Error());
        SleepFor(std::max(500, cfg.reconnect_interval_ms));
      }
    }

    Teardown("gateway shutting down");
  }
};

// ---------------------------------------------------------------------------
// MqClient
// ---------------------------------------------------------------------------

MqClient::MqClient() : impl_(std::make_unique<Impl>()) {}

MqClient::~MqClient() { Stop(); }

void MqClient::Configure(const MqConfig& config) {
  std::lock_guard<std::mutex> lock(impl_->configMutex);

  // Only settings that shape the connection itself force a reconnect. Changing
  // e.g. the envelope flag or the queue limits takes effect on the next
  // message with no interruption.
  const MqConfig& old = impl_->config;
  const bool endpointChanged = old.host != config.host || old.port != config.port || old.vhost != config.vhost ||
                                old.user != config.user || old.password != config.password ||
                                old.exchange != config.exchange || old.exchange_type != config.exchange_type ||
                                old.queue != config.queue || old.routing_key != config.routing_key ||
                                old.declare_topology != config.declare_topology || old.prefetch != config.prefetch ||
                                old.heartbeat_sec != config.heartbeat_sec || old.enabled != config.enabled;

  impl_->config = config;
  if (endpointChanged) impl_->reconfigured.store(true);
  impl_->Wake();
}

MqConfig MqClient::GetConfig() const { return impl_->Config(); }

void MqClient::SetMessageCallback(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->callbackMutex);
  impl_->callback = std::move(callback);
}

void MqClient::Start() {
  if (impl_->running.exchange(true)) return;
  impl_->thread = std::thread([this] { impl_->Run(); });
}

void MqClient::Stop() {
  if (!impl_->running.exchange(false)) return;
  impl_->Wake();
  if (impl_->thread.joinable()) impl_->thread.join();
}

bool MqClient::IsConnected() const { return impl_->connectedFlag.load(); }

MqStatus MqClient::Status() const {
  MqStatus status;
  const MqConfig cfg = impl_->Config();
  status.enabled = cfg.enabled;
  status.connected = impl_->connectedFlag.load();
  status.consuming = impl_->consumingFlag.load();
  status.paused = impl_->pauseRequested.load();
  status.error = impl_->Error();
  status.published = impl_->publishedCount.load();
  status.received = impl_->receivedCount.load();
  status.rejected = impl_->rejectedCount.load();
  status.dropped = impl_->droppedCount.load();
  status.overflowed = impl_->overflowedCount.load();
  {
    std::lock_guard<std::mutex> lock(impl_->outMutex);
    status.pending = impl_->outgoing.size();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->inboxMutex);
    status.available = impl_->inbox.size();
  }
  return status;
}

bool MqClient::Publish(const std::string& body, const std::string& routingKey, const std::string& exchange,
                       bool wrapEnvelope, std::string& error) {
  const MqConfig cfg = impl_->Config();

  if (!cfg.enabled) {
    error = "RabbitMQ is disabled in the configuration";
    return false;
  }

  Impl::Outgoing item;
  item.exchange = exchange.empty() ? cfg.exchange : exchange;
  item.routingKey = routingKey.empty() ? cfg.routing_key : routingKey;
  if (wrapEnvelope) {
    item.body = EncodeEnvelope(body);
    item.contentType = "application/json";
  } else {
    item.body = body;
  }

  const size_t limit = cfg.publish_queue_limit > 0 ? static_cast<size_t>(cfg.publish_queue_limit) : 1;
  {
    std::lock_guard<std::mutex> lock(impl_->outMutex);
    if (impl_->outgoing.size() >= limit) {
      // Refuse the new message rather than evict an older one: the older ones
      // are the ones already promised to the caller, and a script that gets
      // `false` back can decide for itself (retry, log, drop).
      impl_->droppedCount.fetch_add(1);
      error = "outbound queue is full (" + std::to_string(limit) + " messages); broker unreachable?";
      return false;
    }
    impl_->outgoing.push_back(std::move(item));
  }

  impl_->Wake();
  return true;
}

bool MqClient::Flush(int timeoutMs) {
  std::unique_lock<std::mutex> lock(impl_->outMutex);
  return impl_->outCv.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
                                [this] { return impl_->outgoing.empty(); });
}

void MqClient::Pause(bool paused) {
  impl_->pauseRequested.store(paused);
  impl_->Wake();
}

bool MqClient::IsPaused() const { return impl_->pauseRequested.load(); }

bool MqClient::Available() const {
  std::lock_guard<std::mutex> lock(impl_->inboxMutex);
  return !impl_->inbox.empty();
}

bool MqClient::Get(MqMessage& message) {
  std::lock_guard<std::mutex> lock(impl_->inboxMutex);
  if (impl_->inbox.empty()) return false;
  message = std::move(impl_->inbox.front());
  impl_->inbox.pop_front();
  return true;
}

void MqClient::Clear() {
  std::lock_guard<std::mutex> lock(impl_->inboxMutex);
  impl_->inbox.clear();
}

// The v1 envelope codec lives in MqCodec.cpp -- it needs no AMQP library, and
// keeping it out of here is what lets the stub build still format payloads.

// --- connectivity probe ----------------------------------------------------

bool MqClient::TestConnect(const MqConfig& config, std::string& error) {
  // Declared before the lookup on purpose: on Windows getaddrinfo() fails with
  // WSANOTINITIALISED until WSAStartup has run, and TcpSocket's constructor is
  // what runs it.
  TcpSocket socket;

  const std::string ip = ResolveHostToIpv4(config.host);
  if (ip.empty()) {
    error = "cannot resolve host \"" + config.host + "\"";
    return false;
  }

  if (!socket.Connect(ip, config.port, 3000)) {
    error = "TCP connect to " + ip + ":" + std::to_string(config.port) + " failed";
    return false;
  }

  ProbeHandler handler;
  handler.socket = &socket;

  AMQP::Connection connection(&handler, AMQP::Login(config.user, config.password), config.vhost);

  // Pumped for up to three seconds: reaching the port proves nothing about the
  // credentials or the vhost, and it is connection.open-ok that does.
  std::string buffer;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!handler.ready && !handler.errored && std::chrono::steady_clock::now() < deadline) {
    std::string chunk;
    if (socket.Receive(chunk, kReadChunk, 100)) {
      buffer.append(chunk);
      while (!buffer.empty()) {
        const uint64_t consumed = connection.parse(buffer.data(), buffer.size());
        if (consumed == 0) break;
        buffer.erase(0, static_cast<size_t>(consumed));
      }
    } else if (!socket.IsOpen()) {
      error = handler.error.empty() ? "broker closed the connection" : handler.error;
      return false;
    }
  }

  if (handler.ready) {
    connection.close();
    socket.Close();
    return true;
  }

  error = handler.errored ? handler.error : "timed out waiting for the AMQP handshake";
  socket.Close();
  return false;
}

// ---------------------------------------------------------------------------
// Ad-hoc operations (Test Tool plan sections 50-54)
// ---------------------------------------------------------------------------

namespace {

// TestConnect's ProbeHandler with somewhere to put a channel-level failure.
// Kept separate from MqClient::Impl on purpose: Impl is the gateway's own
// long-lived session, with reconnect logic, an inbox and a publish queue, and
// none of that belongs in an operation that lives for three seconds.
struct AdHocHandler : public AMQP::ConnectionHandler {
  TcpSocket* socket = nullptr;
  bool ready = false;
  bool errored = false;
  std::string error;

  void onData(AMQP::Connection*, const char* data, size_t size) override {
    if (!socket) return;
    if (!socket->Send(std::string(data, size))) {
      errored = true;
      if (error.empty()) error = "socket write failed";
    }
  }

  uint16_t onNegotiate(AMQP::Connection*, uint16_t) override { return 0; }
  void onReady(AMQP::Connection*) override { ready = true; }

  void onError(AMQP::Connection*, const char* message) override {
    errored = true;
    error = message ? message : "unknown AMQP error";
  }
};

// Opens a connection and a channel, hands the channel to `setup`, then pumps
// the socket until `setup` reports done (through the callback it is given) or
// the deadline passes.
//
// `setup` runs ONCE, when the channel exists. Everything it starts is a
// deferred whose handlers fire from inside the pump loop below.
bool RunAdHoc(const MqConfig& config, int timeoutMs,
               const std::function<void(AMQP::Channel&, const std::function<void(bool, std::string)>&)>& setup,
               std::string& error) {
  TcpSocket socket;

  const std::string ip = ResolveHostToIpv4(config.host);
  if (ip.empty()) {
    error = "cannot resolve host \"" + config.host + "\"";
    return false;
  }

  if (!socket.Connect(ip, config.port, 3000)) {
    error = "TCP connect to " + ip + ":" + std::to_string(config.port) + " failed";
    return false;
  }

  AdHocHandler handler;
  handler.socket = &socket;

  AMQP::Connection connection(&handler, AMQP::Login(config.user, config.password), config.vhost);

  bool done = false;
  bool ok = false;
  std::string opError;

  auto finish = [&done, &ok, &opError](bool success, std::string message) {
    if (done) return;   // first answer wins; a late error must not rewrite it
    done = true;
    ok = success;
    opError = std::move(message);
  };

  std::unique_ptr<AMQP::Channel> channel;
  std::string buffer;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(500, timeoutMs));

  while (!done && !handler.errored && std::chrono::steady_clock::now() < deadline) {
    // The channel is created the moment the handshake completes, and `setup`
    // immediately after it -- not before, because a channel on an unopened
    // connection is a protocol error rather than a queued command.
    if (handler.ready && !channel) {
      channel = std::make_unique<AMQP::Channel>(&connection);
      channel->onError([&finish](const char* message) {
        finish(false, std::string("channel error: ") + (message ? message : "unknown"));
      });
      setup(*channel, finish);
    }

    std::string chunk;
    if (socket.Receive(chunk, kReadChunk, 100)) {
      buffer.append(chunk);
      while (!buffer.empty()) {
        const uint64_t consumed = connection.parse(buffer.data(), buffer.size());
        if (consumed == 0) break;
        buffer.erase(0, static_cast<size_t>(consumed));
      }
    } else if (!socket.IsOpen()) {
      error = handler.error.empty() ? "broker closed the connection" : handler.error;
      return false;
    }
  }

  // Closed before reporting: a connection left open would sit on the broker
  // until its heartbeat lapsed, and these run one per button press.
  if (channel) channel.reset();
  connection.close();
  socket.Close();

  if (handler.errored && !done) {
    error = handler.error;
    return false;
  }

  if (!done) {
    error = handler.ready ? "timed out waiting for the broker to answer"
                           : "timed out waiting for the AMQP handshake";
    return false;
  }

  error = opError;
  return ok;
}

int ExchangeFlagsFor(const MqClient::AdHoc& options) {
  int flags = 0;
  if (options.durable) flags |= AMQP::durable;
  if (options.auto_delete) flags |= AMQP::autodelete;
  return flags;
}

int QueueFlagsFor(const MqClient::AdHoc& options) {
  int flags = 0;
  if (options.durable) flags |= AMQP::durable;
  if (options.auto_delete) flags |= AMQP::autodelete;
  if (options.exclusive) flags |= AMQP::exclusive;
  return flags;
}

}  // namespace

bool MqClient::DeclareExchange(const MqConfig& config, const AdHoc& options, std::string& error) {
  if (options.exchange.empty()) {
    error = "exchange name is required";
    return false;
  }

  return RunAdHoc(config, options.timeout_ms,
                   [&options](AMQP::Channel& channel, const std::function<void(bool, std::string)>& finish) {
                     channel
                         .declareExchange(options.exchange,
                                           ExchangeTypeFromName(options.exchange_type),
                                           ExchangeFlagsFor(options))
                         .onSuccess([finish]() { finish(true, ""); })
                         .onError([finish](const char* message) {
                           finish(false, message ? message : "declare failed");
                         });
                   },
                   error);
}

bool MqClient::DeclareQueue(const MqConfig& config, const AdHoc& options, uint32_t& messages,
                             uint32_t& consumers, std::string& error) {
  if (options.queue.empty()) {
    error = "queue name is required";
    return false;
  }

  messages = 0;
  consumers = 0;

  return RunAdHoc(config, options.timeout_ms,
                   [&](AMQP::Channel& channel, const std::function<void(bool, std::string)>& finish) {
                     channel.declareQueue(options.queue, QueueFlagsFor(options))
                         .onSuccess([&messages, &consumers, finish](const std::string&, uint32_t messageCount,
                                                                     uint32_t consumerCount) {
                           messages = messageCount;
                           consumers = consumerCount;
                           finish(true, "");
                         })
                         .onError([finish](const char* message) {
                           finish(false, message ? message : "declare failed");
                         });
                   },
                   error);
}

bool MqClient::BindQueue(const MqConfig& config, const AdHoc& options, std::string& error) {
  if (options.queue.empty() || options.exchange.empty()) {
    // The default exchange routes by queue name and cannot be bound to, so an
    // empty exchange here is a mistake rather than a shorthand.
    error = "both an exchange and a queue are required to bind";
    return false;
  }

  return RunAdHoc(config, options.timeout_ms,
                   [&options](AMQP::Channel& channel, const std::function<void(bool, std::string)>& finish) {
                     channel.bindQueue(options.exchange, options.queue, options.routing_key)
                         .onSuccess([finish]() { finish(true, ""); })
                         .onError([finish](const char* message) {
                           finish(false, message ? message : "bind failed");
                         });
                   },
                   error);
}

bool MqClient::PublishOnce(const MqConfig& config, const AdHoc& options, std::string& error) {
  return RunAdHoc(config, options.timeout_ms,
                   [&options](AMQP::Channel& channel, const std::function<void(bool, std::string)>& finish) {
                     // Wrapped in a transaction so the answer means the broker
                     // took it. A bare publish is fire-and-forget: it would
                     // report success for a broker that closed the channel a
                     // millisecond later.
                     channel.startTransaction();
                     channel.publish(options.exchange, options.routing_key, options.body);
                     channel.commitTransaction()
                         .onSuccess([finish]() { finish(true, ""); })
                         .onError([finish](const char* message) {
                           finish(false, message ? message : "publish failed");
                         });
                   },
                   error);
}

bool MqClient::ConsumeOnce(const MqConfig& config, const AdHoc& options,
                            std::vector<AdHocMessage>& received, std::string& error) {
  if (options.queue.empty()) {
    error = "queue name is required";
    return false;
  }

  received.clear();

  const int wanted = std::max(1, options.max_messages);

  bool ok = RunAdHoc(
      config, options.timeout_ms,
      [&](AMQP::Channel& channel, const std::function<void(bool, std::string)>& finish) {
        AMQP::DeferredConsumer& consumer = channel.consume(options.queue);

        // Separate statements, not a chain: onError() comes from Deferred and
        // returns Deferred&, which has no onReceived().
        consumer.onReceived([&, finish](const AMQP::Message& message, uint64_t tag, bool redelivered) {
          AdHocMessage row;
          row.body.assign(message.body(), message.bodySize());
          row.exchange = message.exchange();
          row.routing_key = message.routingkey();
          row.message_id = message.hasMessageID() ? message.messageID() : "";
          row.delivery_tag = tag;
          row.redelivered = redelivered;
          received.push_back(std::move(row));

          if (options.ack) {
            channel.ack(tag);
          } else {
            // Requeued, because this is a peek: the message must still be
            // there for whoever it was actually meant for.
            channel.reject(tag, AMQP::requeue);
          }

          if (static_cast<int>(received.size()) >= wanted) finish(true, "");
        });

        consumer.onError([finish](const char* message) {
          finish(false, message ? message : "consume failed");
        });
      },
      error);

  // A timeout with messages already collected is a successful drain that simply
  // ran out of messages before it ran out of patience -- and an empty queue is
  // the normal state of a healthy one, so that is not a failure either.
  if (!ok && error.find("timed out waiting for the broker") != std::string::npos) {
    error.clear();
    return true;
  }

  return ok;
}

}  // namespace hsf
