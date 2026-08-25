/* hello-plugin — the smallest thing that is genuinely a driver.
 *
 * It speaks a deliberately trivial line protocol so the example is about the
 * SDK rather than about a protocol:
 *
 *     ->  "GET <point>\n"        ->  "VAL <number>\n"
 *     ->  "SET <point> <num>\n"  ->  "OK\n"
 *     ->  "PING\n"               ->  "PONG\n"
 *
 * Everything a real driver has to get right is here in miniature: it is
 * transport-agnostic, it reassembles replies that arrive in pieces, it
 * distinguishes "nothing yet" from "connection closed", it reports failures
 * with a reason, and it never lets an exception reach the host.
 *
 * Copy this file, replace the three lines of protocol with yours.
 */
#include "hsf/support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* --- the driver ---------------------------------------------------------- */

class HelloDriver : public hsf::DriverBase<HelloDriver> {
 public:
  explicit HelloDriver(HSFLoggerRef log) : log_(log) {}

  HSFStatus Initialize(HSFConfigRef config, HSFTransportRef transport) override {
    /* No hardware here. initialize() runs while the operator may still be
     * editing settings, so opening a port now would make a typo unrecoverable
     * without a restart. Read configuration, validate it, claim nothing. */
    timeout_ms_ = static_cast<int>(hsf_cfg_i64(config, "timeout_ms", 1000));
    if (timeout_ms_ < 1) timeout_ms_ = 1;   /* 0 would busy-spin forever */

    if (!hsf_transport_valid(transport)) {
      SetError("no transport was supplied");
      SetState(HSF_DRIVER_FAILED);
      return HSF_ERR_CONFIG;
    }
    transport_ = transport;
    io_ = hsf::BlockingIo(transport);
    SetState(HSF_DRIVER_READY);
    HSF_LOG_I(log_, "hello driver initialised");
    return HSF_OK;
  }

  HSFStatus Start() override {
    /* Must tolerate a restart: Stop/Start is what the Enable and Disable
     * buttons do, so this may run on an instance that has already been used. */
    if (state_ == HSF_DRIVER_RUNNING) return HSF_OK;
    if (!hsf_transport_valid(transport_)) return HSF_ERR_STATE;

    HSFStatus st = transport_.vt->open(transport_.self);
    if (st == HSF_ERR_ALREADY_OPEN) st = HSF_OK;   /* benign */
    if (st < 0) {
      SetError(std::string("cannot open the transport: ") + hsf_status_name(st));
      SetState(HSF_DRIVER_FAILED);
      return st;
    }
    SetState(HSF_DRIVER_RUNNING);
    SetConnected(true);
    SetError("");
    return HSF_OK;
  }

  HSFStatus Stop() override {
    /* Idempotent, and prompt. The host calls this during shutdown; a stop that
     * blocks is indistinguishable from a hang. */
    if (hsf_transport_valid(transport_)) transport_.vt->close(transport_.self);
    SetConnected(false);
    SetState(HSF_DRIVER_READY);
    return HSF_OK;
  }

  HSFStatus Read(const HSFDeviceAddress* addr, HSFValue* out) override {
    if (!addr || !out) return HSF_ERR_INVALID_ARG;
    if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;

    const std::string point = hsf::ToString(addr->point_name);
    std::string reply;
    HSFStatus st = Exchange("GET " + point + "\n", &reply);
    if (st < 0) return st;

    if (reply.compare(0, 4, "VAL ") != 0) {
      SetError("unexpected reply to GET: " + reply);
      return HSF_ERR_PROTOCOL;
    }
    *out = hsf_value_f64(std::strtod(reply.c_str() + 4, nullptr));
    ++ok_reads_;
    return HSF_OK;
  }

  HSFStatus Write(const HSFDeviceAddress* addr, const HSFValue* value) override {
    if (!addr || !value) return HSF_ERR_INVALID_ARG;
    if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;

    double d = 0.0;
    switch (value->kind) {
      case HSF_VALUE_F64:  d = value->as.f64; break;
      case HSF_VALUE_I64:  d = static_cast<double>(value->as.i64); break;
      case HSF_VALUE_U64:  d = static_cast<double>(value->as.u64); break;
      case HSF_VALUE_BOOL: d = value->as.b ? 1.0 : 0.0; break;
      default:
        /* Refuse rather than coerce: writing a blob to an analogue point is a
         * configuration mistake, and silently writing 0 would hide it. */
        SetError(std::string("cannot write a ") + hsf_value_kind_name(value->kind));
        return HSF_ERR_INVALID_ARG;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", d);
    std::string reply;
    HSFStatus st = Exchange("SET " + hsf::ToString(addr->point_name) + " " + buf + "\n",
                            &reply);
    if (st < 0) return st;
    if (reply.compare(0, 2, "OK") != 0) {
      SetError("device refused the write: " + reply);
      return HSF_ERR_PROTOCOL;
    }
    return HSF_OK;
  }

  HSFStatus Health() override {
    if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;
    std::string reply;
    HSFStatus st = Exchange("PING\n", &reply);
    if (st < 0) {
      SetConnected(false);
      return st;
    }
    const bool pong = reply.compare(0, 4, "PONG") == 0;
    SetConnected(pong);
    return pong ? HSF_OK : HSF_ERR_PROTOCOL;
  }

  uint64_t OkReads() const { return ok_reads_; }

 private:
  /* Write a request, read one newline-terminated reply.
   *
   * The loop is the part worth copying: a reply may arrive in several pieces,
   * so this accumulates until it sees the terminator instead of assuming one
   * read is one message. Getting that wrong is the single most common driver
   * bug, and it usually passes on a fast local link and fails on real hardware.
   */
  HSFStatus Exchange(const std::string& request, std::string* reply) {
    reply->clear();
    HSFStatus st = io_.WriteAll(request.data(), request.size(), timeout_ms_);
    if (st < 0) {
      SetError(std::string("write failed: ") + hsf_status_name(st));
      return st;
    }

    char chunk[128];
    for (int guard = 0; guard < 64; ++guard) {   /* bounded, never spins */
      size_t got = 0;
      st = io_.ReadAtLeast(chunk, 1, sizeof(chunk), timeout_ms_, &got);
      if (st < 0) {
        SetError(std::string("read failed: ") + hsf_status_name(st));
        return st;
      }
      reply->append(chunk, got);
      const size_t nl = reply->find('\n');
      if (nl != std::string::npos) {
        reply->resize(nl);           /* trim the terminator */
        return HSF_OK;
      }
    }
    SetError("reply had no terminator within 64 reads");
    return HSF_ERR_PROTOCOL;
  }

  HSFLoggerRef    log_{};
  HSFTransportRef transport_{};
  hsf::BlockingIo io_{HSFTransportRef{}};
  int             timeout_ms_ = 1000;
  uint64_t        ok_reads_ = 0;
};

/* --- the plugin ---------------------------------------------------------- */

class HelloPlugin {
 public:
  explicit HelloPlugin(const HSFPluginContext& ctx)
      : log_(ctx.log), driver_(ctx.log) {}

  HSFDriver* DriverSelf() { return driver_.Self(); }
  static const HSFDriverVTable* DriverVTable() { return HelloDriver::VTable(); }
  HSFLoggerRef Log() const { return log_; }

 private:
  HSFLoggerRef log_;
  HelloDriver  driver_;
};

HelloPlugin* Cast(HSFPlugin* p) { return reinterpret_cast<HelloPlugin*>(p); }

const HSFTransportKind kSupportedTransports[] = {
    HSF_TRANSPORT_SERIAL, HSF_TRANSPORT_TCP, HSF_TRANSPORT_MOCK};

HSFStatus GetDriver(HSFPlugin* self, HSFDriverRef* out, HSFDriverInfo* info) {
  if (!self || !out) return HSF_ERR_INVALID_ARG;
  out->vt = HelloPlugin::DriverVTable();
  out->self = Cast(self)->DriverSelf();

  if (info) {
    std::memset(info, 0, sizeof(*info));
    info->struct_size = static_cast<uint32_t>(sizeof(HSFDriverInfo));
    info->supported_transports = kSupportedTransports;
    info->supported_transport_count =
        sizeof(kSupportedTransports) / sizeof(kSupportedTransports[0]);
    info->tick_ms = 0;
    /* Honest: this driver blocks in Exchange via BlockingIo, so it needs a
     * thread of its own and must not be driven from the host's shared event
     * loop. A driver that implemented on_readable instead would say 0 here. */
    info->owns_thread = 1;
    info->max_frame_bytes = 256;
  }
  return HSF_OK;
}

HSFStatus Diagnostics(HSFPlugin* self, HSFStr* out_json) {
  if (!self || !out_json) return HSF_ERR_INVALID_ARG;
  /* Static so the borrowed pointer outlives the call, per the ABI contract.
   * Returning a pointer into a local would be the classic mistake here. */
  static std::string json;
  json = "{\"example\":true,\"protocol\":\"hello-line-v1\"}";
  *out_json = hsf::Borrow(json);
  return HSF_OK;
}

const HSFPluginVTable kVTable = [] {
  HSFPluginVTable vt{};
  vt.struct_size = static_cast<uint32_t>(sizeof(HSFPluginVTable));
  vt.get_driver = GetDriver;
  vt.register_lua = nullptr;    /* no Lua surface; null is a complete answer */
  vt.reconfigure = nullptr;     /* host will stop/start instead */
  vt.diagnostics = Diagnostics;
  return vt;
}();

const HSFPluginInfo kInfo = [] {
  HSFPluginInfo i{};
  i.struct_size = static_cast<uint32_t>(sizeof(HSFPluginInfo));
  i.id = HSF_STR_LIT("hsf.example.hello");
  i.name = HSF_STR_LIT("HSF Hello Example");
  i.version = HSF_STR_LIT("1.0.0");
  i.description = HSF_STR_LIT("Minimal driver plugin over a line protocol");
  i.author = HSF_STR_LIT("HSF");
  i.kind = HSF_PLUGIN_KIND_DRIVER;
  i.permissions = HSF_PERM_SERIAL | HSF_PERM_NETWORK;
  i.platform = HSF_STR_LIT(HSF_PLATFORM_TRIPLE);
  return i;
}();

HSFStatus Create(const HSFPluginContext* ctx, HSFPlugin** out_plugin,
                 const HSFPluginVTable** out_vtable) {
  /* Guarded: `new` can throw, and an exception crossing the ABI would take the
   * gateway down rather than failing this one plugin. */
  return hsf::Guard([&]() -> HSFStatus {
    HelloPlugin* p = new HelloPlugin(*ctx);
    *out_plugin = reinterpret_cast<HSFPlugin*>(p);
    *out_vtable = &kVTable;
    HSF_LOG_I(ctx->log, "hello plugin created");
    return HSF_OK;
  });
}

void Destroy(HSFPlugin* p) {
  hsf::GuardVoid([&] { delete Cast(p); });
}

}  // namespace

HSF_PLUGIN_DEFINE(&kInfo, Create, Destroy)
