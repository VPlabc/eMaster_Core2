#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/plugin.h"

namespace hsf {

// The host side of the ABI: the services a plugin is handed at creation.
//
// Each class owns a static vtable and hands out a {vtable, self} fat pointer.
// A plugin cannot reach the gateway's singletons directly -- Logger::Instance()
// would not even link across a shared-object boundary -- so everything arrives
// through these.
//
// Every one of them is scoped or bounded on purpose. A plugin is code the
// gateway did not compile; it gets its own configuration section, its own log
// prefix, and a queue that cannot grow without limit.

// Bridges to Logger and LogStore, prefixing every line with the plugin's id so
// two instances of the same driver are distinguishable in one log.
class PluginLogger {
 public:
  PluginLogger(const std::string& plugin_id, const std::string& instance_id);

  HSFLoggerRef Ref();

 private:
  std::string id_;
  std::string prefix_;
};

// Read-only view of one plugin's configuration section.
//
// Read-only for operator-owned settings, with one narrow exception (set_str)
// for values the plugin itself learned and wants back after a restart. The
// asymmetry is deliberate: operator settings flow one way, from the
// Configuration page into the plugin, or the page and the file disagree about
// who owns a value.
class PluginConfig {
 public:
  // `section` is the plugin's own JSON object. Copied, not referenced: the
  // caller's document may be reparsed when an operator saves the page, and a
  // plugin holding a pointer into it would read freed memory.
  PluginConfig(std::string plugin_id, nlohmann::json section);

  HSFConfigRef Ref();

  // Replaces the section wholesale. Used by reconfigure.
  void Replace(nlohmann::json section);

  // Values the plugin persisted via set_str, for the host to write back.
  nlohmann::json Persisted() const;
  bool           Dirty() const;

 private:
  mutable std::mutex mutex_;
  std::string        plugin_id_;
  nlohmann::json     section_;
  nlohmann::json     persisted_;
  bool               dirty_ = false;
  // set_str returns a borrowed HSFStr, and get_str must too, so the strings
  // handed out have to outlive the call. They live here.
  mutable std::deque<std::string> borrowed_;
};

// A bounded fan-out event bus (plan §3.1, and the thing the gateway does not
// have today -- events currently propagate as direct callbacks that name
// concrete modules).
//
// BOUNDED, not growable: §27 requires bounded queues, and a driver that can
// outrun its subscribers must be told rather than allowed to consume memory
// until the device falls over. Publish returns HSF_ERR_BUSY when full.
class PluginEventBus {
 public:
  using Sink = std::function<void(const std::string& topic, const std::string& source,
                                  const std::string& payload_json)>;

  explicit PluginEventBus(size_t capacity = 256);

  // Handed to each plugin, tagged with which plugin is publishing so `source`
  // cannot be forged.
  HSFEventBusRef RefFor(const std::string& plugin_id);

  // Where events go once out of the queue: Lua, the WebSocket, the log. Set by
  // the host at wiring time.
  void SetSink(Sink sink);

  // Drains the queue on the caller's thread, invoking subscribers and the sink.
  // Called by the host's loop, NOT from publish: a publisher must never run a
  // subscriber inline, or a slow subscriber stalls the driver that produced the
  // event and re-entrancy becomes possible.
  size_t Dispatch(size_t max_events = 64);

  size_t Dropped() const { return dropped_.load(); }
  size_t QueueDepth() const;

 private:
  struct Queued {
    std::string topic;
    std::string source;
    std::string payload_json;
    HSFValue    value;
    int64_t     timestamp_ms;
  };
  struct Subscriber {
    HSFSubscription id;
    std::string     filter;   // may end in '*'
    HSFEventHandler handler;
    void*           user;
  };
  // Per-plugin binding, so the bus knows who is calling without the plugin
  // saying so.
  struct Binding {
    PluginEventBus* bus;
    std::string     plugin_id;
  };

  static bool Matches(const std::string& filter, const std::string& topic);

  HSFStatus Publish(const std::string& source, const std::string& topic,
                    const std::string& payload_json, const HSFValue* value);
  HSFSubscription Subscribe(const std::string& filter, HSFEventHandler h, void* user);
  void Unsubscribe(HSFSubscription id);

  mutable std::mutex      mutex_;
  std::deque<Queued>      queue_;
  std::vector<Subscriber> subscribers_;
  std::deque<Binding>     bindings_;   // stable addresses; never reallocated
  Sink                    sink_;
  size_t                  capacity_;
  HSFSubscription         next_sub_ = 1;
  std::atomic<size_t>     dropped_{0};
};

}  // namespace hsf
