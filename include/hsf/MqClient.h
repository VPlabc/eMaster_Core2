#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "hsf/ConfigManager.h"

namespace hsf {

// One message taken off the broker, as handed to Lua by Mq.Get().
struct MqMessage {
  // Payload the script cares about. With MqConfig::envelope on this is the
  // envelope's "body" field; with it off, the raw AMQP body.
  std::string body;
  // Whole AMQP body as it arrived, envelope and all. Kept so a script can deal
  // with a payload this gateway doesn't model.
  std::string raw;
  // Envelope fields (empty / 0 when envelope handling is off or the payload
  // carried none).
  std::string id;
  std::string timestamp;
  int version = 0;
  std::string exchange;
  std::string routing_key;
  bool redelivered = false;
  uint64_t delivery_tag = 0;
};

// Counters and connection state for Mq.Status() and the dashboard.
struct MqStatus {
  bool enabled = false;
  bool connected = false;
  bool consuming = false;
  bool paused = false;
  std::string error;  // last AMQP / transport error, "" when healthy
  uint64_t published = 0;
  uint64_t received = 0;
  uint64_t rejected = 0;  // undecodable or oversized, dropped without requeue
  uint64_t dropped = 0;   // outbound messages refused because the queue was full
  uint64_t overflowed = 0;  // inbound messages evicted from a full inbox
  size_t pending = 0;       // outbound messages not yet handed to the broker
  size_t available = 0;     // inbound messages waiting for Mq.Get()
};

// RabbitMQ (AMQP 0-9-1) client. Protocol handling is AMQP-CPP's; the transport,
// the thread and the reconnect loop are ours -- AMQP-CPP deliberately ships no
// I/O of its own, which is exactly why it fits a codebase that already owns a
// TcpSocket wrapper (see qt-mq-lab's RabbitMQ.md, "AMQP-CPP has no I/O of its
// own").
//
// Threading: AMQP-CPP's Connection and Channel are NOT thread-safe, so every
// call into them happens on this object's own IO thread. Publish() and Pause()
// are therefore queues, not direct calls -- they hand work to that thread and
// return. Everything public here is safe to call from any thread, which matters
// because the callers are Lua scripts on their own runtime threads plus the web
// server's workers.
//
// Differences from the qt-mq-lab reference this is modelled on, both
// deliberate:
//   - it reconnects. That app connects once from main() and stays down after a
//     drop; an unattended gateway cannot.
//   - a delivery is acked once it is in the inbox (the same point the Qt app
//     acks: right after the message is committed to its model), not when a
//     script gets around to reading it. Ack-on-read would stall the consumer
//     at `prefetch` messages for any script that only listens for
//     OnMqMessage, and the inbox is a convenience buffer, not the system of
//     record.
class MqClient {
 public:
  // Fired for every accepted message, from the IO thread. Used by main.cpp to
  // queue OnMqMessage into every live Lua runtime; the message is already in
  // the inbox by then, so a handler that prefers polling can ignore it.
  using MessageCallback = std::function<void(const MqMessage& message)>;

  MqClient();
  ~MqClient();

  MqClient(const MqClient&) = delete;
  MqClient& operator=(const MqClient&) = delete;

  // Takes effect on the next connection attempt; an address or credential
  // change tears the current connection down so the new one is used.
  void Configure(const MqConfig& config);
  MqConfig GetConfig() const;

  void SetMessageCallback(MessageCallback callback);

  // Starts the IO thread. Does nothing but idle while MqConfig::enabled is
  // false, so this is safe to call unconditionally at startup.
  void Start();
  void Stop();

  bool IsConnected() const;
  MqStatus Status() const;

  // Queues one message. `routingKey` empty means MqConfig::routing_key,
  // `exchange` empty means MqConfig::exchange (and "" is also a legitimate
  // exchange -- the default one -- so pass MqConfig::exchange explicitly to
  // target it).
  //
  // Returns false with `error` set when the client is disabled or the outbound
  // queue is full. True means queued, NOT delivered: publishing is
  // asynchronous, and delivery is visible through Status().published /
  // Status().pending. Persistent (delivery mode 2), per RabbitMQ.md.
  bool Publish(const std::string& body, const std::string& routingKey, const std::string& exchange,
               bool wrapEnvelope, std::string& error);

  // Blocks until the outbound queue is empty or `timeoutMs` elapses. For a
  // script that wants to know its message actually left the process before it
  // moves on. False on timeout.
  bool Flush(int timeoutMs);

  // Cancels the consumer (true) or restarts it (false). Acks for messages
  // already in flight are still sent.
  void Pause(bool paused);
  bool IsPaused() const;

  // Inbox, polled from Lua exactly like Card.Available()/Card.Get().
  bool Available() const;
  bool Get(MqMessage& message);
  void Clear();

  // --- envelope v1 (RabbitMQ.md "Message envelope") ------------------------
  //
  // {"id":"<uuid, no braces>","ts":"<ISO-8601 with ms, UTC>","v":1,"body":"<text>"}
  //
  // Encode() stamps a fresh id and timestamp. Decode() is strict on purpose:
  // id non-empty string, ts parseable and UTC, v == 1, body a string. Anything
  // else is not a v1 envelope and the caller must not guess.
  static std::string EncodeEnvelope(const std::string& body);
  static bool DecodeEnvelope(const std::string& payload, MqMessage& out, std::string& error);

  // One-shot connectivity probe for the Configuration page's Test button.
  // Opens its own connection, waits for connection.open-ok, closes it again --
  // so it proves credentials and vhost, not just that the port answers, and
  // never disturbs the gateway's own session.
  static bool TestConnect(const MqConfig& config, std::string& error);

  // --- ad-hoc broker operations (Test Tool plan sections 50-54) ------------
  //
  // EACH OF THESE OPENS ITS OWN SHORT-LIVED CONNECTION and closes it again --
  // the same shape as TestConnect above, and for a stronger reason. Declaring
  // test topology on the gateway's live channel would be merely untidy; running
  // an ad-hoc consumer on it would be worse than that, because every message it
  // took is a message the SmartLocker script never receives. A separate
  // connection tests the same broker over the same codec without competing with
  // the running system for its own inbox.
  //
  // They are synchronous: each pumps its socket until the broker answers or
  // `timeout_ms` passes, so a caller is a web thread holding a request open.
  struct AdHoc {
    std::string exchange;
    std::string exchange_type = "topic";  // direct | topic | fanout | headers
    std::string queue;
    std::string routing_key;
    std::string body;

    bool durable = true;
    bool auto_delete = false;
    bool exclusive = false;

    // Consume only: acknowledge what arrives (false leaves it for the next
    // consumer, which is what a peek should do).
    bool ack = true;
    int max_messages = 10;

    int timeout_ms = 3000;
  };

  struct AdHocMessage {
    std::string body;
    std::string exchange;
    std::string routing_key;
    std::string message_id;
    uint64_t delivery_tag = 0;
    bool redelivered = false;
  };

  static bool DeclareExchange(const MqConfig& config, const AdHoc& options, std::string& error);

  // `messages`/`consumers` are what the broker reports for the queue, which is
  // also how a passive declare doubles as "does this queue exist and how deep
  // is it".
  static bool DeclareQueue(const MqConfig& config, const AdHoc& options, uint32_t& messages,
                            uint32_t& consumers, std::string& error);

  static bool BindQueue(const MqConfig& config, const AdHoc& options, std::string& error);

  // Published inside a transaction, so "published" means the broker committed
  // it rather than only that the bytes left this process. Note that a commit
  // still succeeds for a message no queue was bound to receive -- AMQP drops
  // unroutable messages silently unless they are published mandatory.
  static bool PublishOnce(const MqConfig& config, const AdHoc& options, std::string& error);

  // Drains up to `max_messages` from `queue`, or waits out `timeout_ms`,
  // whichever comes first. Returning nothing after the timeout is a result, not
  // a failure: an empty queue is the normal state of a healthy one.
  static bool ConsumeOnce(const MqConfig& config, const AdHoc& options,
                           std::vector<AdHocMessage>& received, std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hsf
