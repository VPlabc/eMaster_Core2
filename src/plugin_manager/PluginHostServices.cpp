#include "hsf/plugin_manager/PluginHostServices.h"

#include <chrono>
#include <map>

#include "hsf/LogStore.h"
#include "hsf/Logger.h"

namespace hsf {
namespace {

std::string ToStd(HSFStr s) {
  return (s.ptr && s.len) ? std::string(s.ptr, s.len) : std::string();
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// --- PluginLogger ------------------------------------------------------------

PluginLogger::PluginLogger(const std::string& plugin_id, const std::string& instance_id)
    : id_(plugin_id) {
  prefix_ = "[" + plugin_id;
  if (!instance_id.empty()) prefix_ += ":" + instance_id;
  prefix_ += "] ";
}

HSFLoggerRef PluginLogger::Ref() {
  static const HSFLoggerVTable vt = [] {
    HSFLoggerVTable v{};
    v.struct_size = static_cast<uint32_t>(sizeof(HSFLoggerVTable));

    v.write = [](HSFLogger* self, HSFLogLevel level, HSFStr msg) {
      auto* me = reinterpret_cast<PluginLogger*>(self);
      const std::string line = me->prefix_ + ToStd(msg);
      // Everything a plugin logs lands in the Lua category, which is the
      // gateway's existing channel for "code the gateway did not compile".
      // Adding a category per plugin would resize the Logger's per-category
      // ring-buffer array at runtime, and that array is fixed at build time.
      switch (level) {
        case HSF_LOG_TRACE:
        case HSF_LOG_DEBUG: Logger::Instance().Debug(LogCategory::Lua, line); break;
        case HSF_LOG_INFO:  Logger::Instance().Info(LogCategory::Lua, line); break;
        case HSF_LOG_WARN:  Logger::Instance().Warning(LogCategory::Lua, line); break;
        case HSF_LOG_ERROR: Logger::Instance().Error(LogCategory::Lua, line); break;
      }
    };

    // The gateway's Logger has no level filter to consult, so everything is
    // enabled. Answering honestly matters: a plugin that trusted a false here
    // would skip building a message the host would in fact have recorded.
    v.enabled = [](const HSFLogger*, HSFLogLevel) { return (int32_t)1; };

    v.record = [](HSFLogger* self, HSFStr type, HSFStr json_fields) -> HSFStatus {
      auto* me = reinterpret_cast<PluginLogger*>(self);
      const std::string t = ToStd(type);
      const std::string f = ToStd(json_fields);
      if (t.empty()) return HSF_ERR_INVALID_ARG;
      try {
        const nlohmann::json data =
            f.empty() ? nlohmann::json::object() : nlohmann::json::parse(f);
        if (!data.is_object()) return HSF_ERR_INVALID_ARG;

        // LogStore stores field values as strings and validates them against
        // the declared type, so numbers and booleans are flattened here rather
        // than in the plugin -- otherwise every plugin author would have to
        // know that a log field is textual on the wire.
        std::map<std::string, std::string> fields;
        for (auto it = data.begin(); it != data.end(); ++it) {
          if (it.value().is_string()) {
            fields[it.key()] = it.value().get<std::string>();
          } else if (it.value().is_null()) {
            fields[it.key()] = std::string();
          } else {
            fields[it.key()] = it.value().dump();
          }
        }

        std::string error;
        // The plugin id goes in as the "script name", which is the column the
        // Structured Logs page already filters on -- a plugin's rows are then
        // attributable exactly like a Lua application's, with no schema change.
        // LogStore refuses an undeclared type, which is correct: that is a bug
        // in the plugin, not a new schema to invent on its behalf.
        if (!LogStore::Instance().Write(t, me->id_, "INFO", fields, error)) {
          return HSF_ERR_NOT_FOUND;
        }
        return HSF_OK;
      } catch (const std::exception&) {
        return HSF_ERR_INVALID_ARG;
      }
    };
    return v;
  }();

  HSFLoggerRef r;
  r.vt = &vt;
  r.self = reinterpret_cast<HSFLogger*>(this);
  return r;
}

// --- PluginConfig ------------------------------------------------------------

PluginConfig::PluginConfig(std::string plugin_id, nlohmann::json section)
    : plugin_id_(std::move(plugin_id)),
      section_(section.is_object() ? std::move(section) : nlohmann::json::object()),
      persisted_(nlohmann::json::object()) {}

void PluginConfig::Replace(nlohmann::json section) {
  std::lock_guard<std::mutex> lock(mutex_);
  section_ = section.is_object() ? std::move(section) : nlohmann::json::object();
}

nlohmann::json PluginConfig::Persisted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return persisted_;
}

bool PluginConfig::Dirty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dirty_;
}

HSFConfigRef PluginConfig::Ref() {
  static const HSFConfigVTable vt = [] {
    HSFConfigVTable v{};
    v.struct_size = static_cast<uint32_t>(sizeof(HSFConfigVTable));

    v.has = [](const HSFConfig* self, HSFStr key) -> int32_t {
      auto* me = reinterpret_cast<const PluginConfig*>(self);
      std::lock_guard<std::mutex> lock(me->mutex_);
      return me->section_.contains(ToStd(key)) ? 1 : 0;
    };

    v.get_str = [](const HSFConfig* self, HSFStr key, HSFStr fallback) -> HSFStr {
      auto* me = reinterpret_cast<const PluginConfig*>(self);
      std::lock_guard<std::mutex> lock(me->mutex_);
      const std::string k = ToStd(key);
      if (!me->section_.contains(k) || !me->section_[k].is_string()) return fallback;
      // The ABI says the result is borrowed, so it must outlive the call. The
      // value lives in a json document that may be replaced under us, so a
      // pointer into it is not safe -- stash a copy.
      //
      // Bounded, because a driver polling a string setting in a loop would
      // otherwise grow this forever. 64 is far more than any plugin's distinct
      // string keys, and the oldest entry can only be reclaimed once no
      // caller can still be holding it, which one call later is true.
      me->borrowed_.push_back(me->section_[k].get<std::string>());
      if (me->borrowed_.size() > 64) me->borrowed_.pop_front();
      const std::string& kept = me->borrowed_.back();
      HSFStr out;
      out.ptr = kept.c_str();
      out.len = kept.size();
      return out;
    };

    v.get_i64 = [](const HSFConfig* self, HSFStr key, int64_t fallback) -> int64_t {
      auto* me = reinterpret_cast<const PluginConfig*>(self);
      std::lock_guard<std::mutex> lock(me->mutex_);
      const std::string k = ToStd(key);
      if (!me->section_.contains(k)) return fallback;
      const nlohmann::json& v2 = me->section_[k];
      if (v2.is_number_integer()) return v2.get<int64_t>();
      if (v2.is_number_float()) return static_cast<int64_t>(v2.get<double>());
      if (v2.is_boolean()) return v2.get<bool>() ? 1 : 0;
      // A wrong-typed value yields the fallback rather than an error: a driver
      // must not refuse to start because one optional setting is a string.
      return fallback;
    };

    v.get_f64 = [](const HSFConfig* self, HSFStr key, double fallback) -> double {
      auto* me = reinterpret_cast<const PluginConfig*>(self);
      std::lock_guard<std::mutex> lock(me->mutex_);
      const std::string k = ToStd(key);
      if (!me->section_.contains(k) || !me->section_[k].is_number()) return fallback;
      return me->section_[k].get<double>();
    };

    v.get_bool = [](const HSFConfig* self, HSFStr key, int32_t fallback) -> int32_t {
      auto* me = reinterpret_cast<const PluginConfig*>(self);
      std::lock_guard<std::mutex> lock(me->mutex_);
      const std::string k = ToStd(key);
      if (!me->section_.contains(k)) return fallback;
      const nlohmann::json& v2 = me->section_[k];
      if (v2.is_boolean()) return v2.get<bool>() ? 1 : 0;
      if (v2.is_number_integer()) return v2.get<int64_t>() != 0 ? 1 : 0;
      return fallback;
    };

    v.get_json = [](const HSFConfig* self) -> HSFStr {
      auto* me = reinterpret_cast<const PluginConfig*>(self);
      std::lock_guard<std::mutex> lock(me->mutex_);
      me->borrowed_.push_back(me->section_.dump());
      if (me->borrowed_.size() > 64) me->borrowed_.pop_front();
      const std::string& kept = me->borrowed_.back();
      HSFStr out;
      out.ptr = kept.c_str();
      out.len = kept.size();
      return out;
    };

    v.set_str = [](HSFConfig* self, HSFStr key, HSFStr value) -> HSFStatus {
      auto* me = reinterpret_cast<PluginConfig*>(self);
      const std::string k = ToStd(key);
      if (k.empty()) return HSF_ERR_INVALID_ARG;
      std::lock_guard<std::mutex> lock(me->mutex_);
      me->persisted_[k] = ToStd(value);
      me->section_[k] = ToStd(value);
      me->dirty_ = true;
      return HSF_OK;
    };
    return v;
  }();

  HSFConfigRef r;
  r.vt = &vt;
  r.self = reinterpret_cast<HSFConfig*>(this);
  return r;
}

// --- PluginEventBus ----------------------------------------------------------

PluginEventBus::PluginEventBus(size_t capacity) : capacity_(capacity ? capacity : 1) {}

void PluginEventBus::SetSink(Sink sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = std::move(sink);
}

size_t PluginEventBus::QueueDepth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

// "driver.modbus.*" matches "driver.modbus.value". A bare "*" matches all.
bool PluginEventBus::Matches(const std::string& filter, const std::string& topic) {
  if (filter.empty()) return false;
  if (filter == "*") return true;
  if (filter.back() == '*') {
    const size_t n = filter.size() - 1;
    return topic.size() >= n && topic.compare(0, n, filter, 0, n) == 0;
  }
  return filter == topic;
}

HSFStatus PluginEventBus::Publish(const std::string& source, const std::string& topic,
                                  const std::string& payload_json, const HSFValue* value) {
  if (topic.empty()) return HSF_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= capacity_) {
    // Refuse rather than drop the oldest. The publisher is a driver that can
    // slow down; silently discarding events would make a missed card read
    // indistinguishable from one that never happened.
    dropped_.fetch_add(1);
    return HSF_ERR_BUSY;
  }
  Queued q;
  q.topic = topic;
  q.source = source;
  q.payload_json = payload_json;
  q.value = value ? *value : hsf_value_null();
  q.timestamp_ms = NowMs();
  queue_.push_back(std::move(q));
  return HSF_OK;
}

HSFSubscription PluginEventBus::Subscribe(const std::string& filter, HSFEventHandler h,
                                          void* user) {
  if (filter.empty() || !h) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  Subscriber s;
  s.id = next_sub_++;
  s.filter = filter;
  s.handler = h;
  s.user = user;
  subscribers_.push_back(s);
  return s.id;
}

void PluginEventBus::Unsubscribe(HSFSubscription id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < subscribers_.size(); ++i) {
    if (subscribers_[i].id == id) {
      subscribers_.erase(subscribers_.begin() + static_cast<long>(i));
      return;
    }
  }
}

size_t PluginEventBus::Dispatch(size_t max_events) {
  size_t handled = 0;
  for (; handled < max_events; ++handled) {
    Queued q;
    std::vector<Subscriber> targets;
    Sink sink;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty()) break;
      q = std::move(queue_.front());
      queue_.pop_front();
      // Copy the matching subscribers and release the lock before calling any
      // of them. A handler that publishes -- which is normal, one event
      // causing another -- would otherwise deadlock on a non-recursive mutex.
      for (const Subscriber& s : subscribers_) {
        if (Matches(s.filter, q.topic)) targets.push_back(s);
      }
      sink = sink_;
    }

    HSFEvent ev{};
    ev.struct_size = static_cast<uint32_t>(sizeof(HSFEvent));
    ev.topic = hsf_cstr(q.topic.c_str());
    ev.source_plugin = hsf_cstr(q.source.c_str());
    ev.payload_json = hsf_cstr(q.payload_json.c_str());
    ev.value = q.value;
    ev.timestamp_ms = q.timestamp_ms;

    for (const Subscriber& s : targets) {
      // A subscriber is plugin code. An exception escaping it would unwind
      // through the host's dispatch loop and take the gateway down, so it is
      // contained here as well as inside the plugin.
      try {
        s.handler(&ev, s.user);
      } catch (...) {
        Logger::Instance().Error(LogCategory::Lua,
                                 "A plugin event handler threw while handling " + q.topic);
      }
    }
    if (sink) {
      try {
        sink(q.topic, q.source, q.payload_json);
      } catch (...) {
        Logger::Instance().Error(LogCategory::Lua, "The plugin event sink threw on " + q.topic);
      }
    }
  }
  return handled;
}

HSFEventBusRef PluginEventBus::RefFor(const std::string& plugin_id) {
  static const HSFEventBusVTable vt = [] {
    HSFEventBusVTable v{};
    v.struct_size = static_cast<uint32_t>(sizeof(HSFEventBusVTable));

    v.publish = [](HSFEventBus* self, HSFStr topic, HSFStr payload) -> HSFStatus {
      auto* b = reinterpret_cast<Binding*>(self);
      return b->bus->Publish(b->plugin_id, ToStd(topic), ToStd(payload), nullptr);
    };
    v.publish_value = [](HSFEventBus* self, HSFStr topic, HSFValue value) -> HSFStatus {
      auto* b = reinterpret_cast<Binding*>(self);
      return b->bus->Publish(b->plugin_id, ToStd(topic), std::string(), &value);
    };
    v.subscribe = [](HSFEventBus* self, HSFStr filter, HSFEventHandler h,
                     void* user) -> HSFSubscription {
      auto* b = reinterpret_cast<Binding*>(self);
      return b->bus->Subscribe(ToStd(filter), h, user);
    };
    v.unsubscribe = [](HSFEventBus* self, HSFSubscription sub) {
      auto* b = reinterpret_cast<Binding*>(self);
      b->bus->Unsubscribe(sub);
    };
    return v;
  }();

  // One Binding per plugin, kept in a deque so its address is stable: the
  // plugin holds this pointer for its whole life, and a vector reallocating
  // would leave it dangling.
  Binding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Binding& b : bindings_) {
      if (b.plugin_id == plugin_id) { binding = &b; break; }
    }
    if (!binding) {
      bindings_.push_back(Binding{this, plugin_id});
      binding = &bindings_.back();
    }
  }

  HSFEventBusRef r;
  r.vt = &vt;
  r.self = reinterpret_cast<HSFEventBus*>(binding);
  return r;
}

}  // namespace hsf
