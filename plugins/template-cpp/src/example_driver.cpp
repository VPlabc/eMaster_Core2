/* Example driver — the file you replace.
 *
 * Structured so the parts you change and the parts you keep are separate:
 *
 *   ExampleDriver::BuildRequest / ParseReply   <- YOUR protocol goes here
 *   everything below the "plugin plumbing" line <- copy unchanged
 *
 * The protocol methods are deliberately pure functions over byte buffers, with
 * no I/O in them. That is what lets the unit tests in tests/test_driver.cpp
 * check framing, encoding and error handling with no transport at all — and it
 * is the single most useful habit when writing a driver, because protocol bugs
 * are cheap to find in a pure function and expensive to find over a wire.
 */
#include "example_driver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example {

/* =====================  YOUR PROTOCOL  ==================================== */

/* Frame a read request. This example uses a text line; a binary protocol would
 * push bytes and a checksum instead. */
Bytes ExampleDriver::BuildRequest(const std::string& point) const {
  const std::string s = "GET " + point + "\n";
  return Bytes(s.begin(), s.end());
}

Bytes ExampleDriver::BuildWriteRequest(const std::string& point, double v) const {
  char num[32];
  std::snprintf(num, sizeof(num), "%g", v);
  const std::string s = "SET " + point + " " + num + "\n";
  return Bytes(s.begin(), s.end());
}

/* How much of `buf` is one complete reply, or 0 if more is needed.
 *
 * Separating "is it complete?" from "what does it mean?" is what makes a driver
 * robust against fragmentation. A transport hands over whatever arrived, which
 * may be half a frame or two frames; only this function knows where the
 * boundary is. */
size_t ExampleDriver::FrameLength(const Bytes& buf) const {
  for (size_t i = 0; i < buf.size(); ++i) {
    if (buf[i] == '\n') return i + 1;
  }
  return 0;
}

/* Interpret one complete frame. Never called with a partial one. */
HSFStatus ExampleDriver::ParseReply(const Bytes& frame, HSFValue* out,
                                    std::string* error) const {
  std::string s(frame.begin(), frame.end());
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();

  if (s.rfind("VAL ", 0) == 0) {
    const char* p = s.c_str() + 4;
    char* end = nullptr;
    const double d = std::strtod(p, &end);
    if (end == p) {
      *error = "VAL with an unparseable number: " + s;
      return HSF_ERR_PROTOCOL;
    }
    if (out) *out = hsf_value_f64(d);
    return HSF_OK;
  }
  if (s == "OK") {
    if (out) *out = hsf_value_null();
    return HSF_OK;
  }
  if (s == "PONG") {
    if (out) *out = hsf_value_bool(1);
    return HSF_OK;
  }
  /* An error reply from the device is a PROTOCOL failure with the device's own
   * words attached — not a generic code. Whoever reads the log needs to know
   * what the device said. */
  *error = "device replied: " + s;
  return HSF_ERR_PROTOCOL;
}

/* =====================  PLUGIN PLUMBING  ================================= */

HSFStatus ExampleDriver::Initialize(HSFConfigRef config, HSFTransportRef transport) {
  timeout_ms_ = static_cast<int>(hsf_cfg_i64(config, "timeout_ms", 1000));
  if (timeout_ms_ < 1) timeout_ms_ = 1;
  retries_ = static_cast<int>(hsf_cfg_i64(config, "retries", 2));
  if (retries_ < 0) retries_ = 0;
  unit_id_ = static_cast<int>(hsf_cfg_i64(config, "unit_id", 1));

  if (!hsf_transport_valid(transport)) {
    SetError("no transport was supplied");
    SetState(HSF_DRIVER_FAILED);
    return HSF_ERR_CONFIG;
  }
  transport_ = transport;
  io_ = hsf::BlockingIo(transport);
  SetState(HSF_DRIVER_READY);
  return HSF_OK;
}

HSFStatus ExampleDriver::Start() {
  if (state_ == HSF_DRIVER_RUNNING) return HSF_OK;
  if (!hsf_transport_valid(transport_)) return HSF_ERR_STATE;

  HSFStatus st = transport_.vt->open(transport_.self);
  if (st == HSF_ERR_ALREADY_OPEN) st = HSF_OK;
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

HSFStatus ExampleDriver::Stop() {
  if (hsf_transport_valid(transport_)) transport_.vt->close(transport_.self);
  SetConnected(false);
  SetState(HSF_DRIVER_READY);
  return HSF_OK;
}

HSFStatus ExampleDriver::Read(const HSFDeviceAddress* addr, HSFValue* out) {
  if (!addr || !out) return HSF_ERR_INVALID_ARG;
  if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;
  return Transact(BuildRequest(hsf::ToString(addr->point_name)), out);
}

HSFStatus ExampleDriver::Write(const HSFDeviceAddress* addr, const HSFValue* v) {
  if (!addr || !v) return HSF_ERR_INVALID_ARG;
  if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;

  double d = 0.0;
  switch (v->kind) {
    case HSF_VALUE_F64:  d = v->as.f64; break;
    case HSF_VALUE_I64:  d = static_cast<double>(v->as.i64); break;
    case HSF_VALUE_U64:  d = static_cast<double>(v->as.u64); break;
    case HSF_VALUE_BOOL: d = v->as.b ? 1.0 : 0.0; break;
    default:
      SetError(std::string("cannot write a ") + hsf_value_kind_name(v->kind));
      return HSF_ERR_INVALID_ARG;
  }
  HSFValue ignored = hsf_value_null();
  return Transact(BuildWriteRequest(hsf::ToString(addr->point_name), d), &ignored);
}

HSFStatus ExampleDriver::Health() {
  if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;
  HSFValue v = hsf_value_null();
  const std::string ping = "PING\n";
  HSFStatus st = Transact(Bytes(ping.begin(), ping.end()), &v);
  SetConnected(st == HSF_OK);
  return st;
}

/* One request, one reply, with retries.
 *
 * Note what happens on a retry: the input buffer is flushed first. On a shared
 * RS485 bus the remains of a previous or someone else's reply are still sitting
 * there, and parsing them as the start of ours turns one timeout into a run of
 * protocol errors. */
HSFStatus ExampleDriver::Transact(const Bytes& request, HSFValue* out) {
  HSFStatus last = HSF_ERR_INTERNAL;

  for (int attempt = 0; attempt <= retries_; ++attempt) {
    if (attempt > 0 && transport_.vt->flush_input) {
      transport_.vt->flush_input(transport_.self);
    }

    last = io_.WriteAll(request.data(), request.size(), timeout_ms_);
    if (last < 0) {
      SetError(std::string("write failed: ") + hsf_status_name(last));
      ++writes_failed_;
      continue;
    }

    Bytes acc;
    char chunk[256];
    bool framed = false;
    for (int i = 0; i < 64 && !framed; ++i) {
      size_t got = 0;
      last = io_.ReadAtLeast(chunk, 1, sizeof(chunk), timeout_ms_, &got);
      if (last < 0) break;
      acc.insert(acc.end(), chunk, chunk + got);
      const size_t n = FrameLength(acc);
      if (n > 0) {
        acc.resize(n);
        framed = true;
      }
    }

    if (!framed) {
      if (last >= 0) last = HSF_ERR_PROTOCOL;
      SetError(std::string("no complete reply: ") + hsf_status_name(last));
      ++reads_failed_;
      continue;
    }

    std::string err;
    last = ParseReply(acc, out, &err);
    if (last == HSF_OK) {
      ++reads_ok_;
      SetError("");
      return HSF_OK;
    }
    SetError(err);
    ++reads_failed_;
  }

  /* Out of retries. connected_ goes false so the dashboard shows the device as
   * unreachable while the driver itself is still correctly RUNNING. */
  SetConnected(false);
  return last;
}

void ExampleDriver::Status(HSFDriverStatus* out) {
  hsf::DriverBase<ExampleDriver>::Status(out);
  if (!out) return;
  out->reads_ok = reads_ok_;
  out->reads_failed = reads_failed_;
  out->writes_failed = writes_failed_;
}

}  // namespace example

/* The exported ABI. Omitted when building the unit tests, which link these
 * sources directly and would otherwise get duplicate entry points. */
#ifndef HSF_PLUGIN_TESTING

namespace {

const HSFTransportKind kTransports[] = {HSF_TRANSPORT_SERIAL, HSF_TRANSPORT_TCP,
                                        HSF_TRANSPORT_MOCK};

struct Plugin {
  example::ExampleDriver driver;
};

Plugin* Cast(HSFPlugin* p) { return reinterpret_cast<Plugin*>(p); }

HSFStatus GetDriver(HSFPlugin* self, HSFDriverRef* out, HSFDriverInfo* info) {
  if (!self || !out) return HSF_ERR_INVALID_ARG;
  out->vt = example::ExampleDriver::VTable();
  out->self = Cast(self)->driver.Self();
  if (info) {
    std::memset(info, 0, sizeof(*info));
    info->struct_size = static_cast<uint32_t>(sizeof(HSFDriverInfo));
    info->supported_transports = kTransports;
    info->supported_transport_count = sizeof(kTransports) / sizeof(kTransports[0]);
    info->owns_thread = 1;   /* Transact blocks; see driver.h on this field */
    info->max_frame_bytes = 512;
  }
  return HSF_OK;
}

const HSFPluginVTable kVTable = [] {
  HSFPluginVTable vt{};
  vt.struct_size = static_cast<uint32_t>(sizeof(HSFPluginVTable));
  vt.get_driver = GetDriver;
  return vt;
}();

const HSFPluginInfo kInfo = [] {
  HSFPluginInfo i{};
  i.struct_size = static_cast<uint32_t>(sizeof(HSFPluginInfo));
  i.id = HSF_STR_LIT("hsf.driver.example");
  i.name = HSF_STR_LIT("Example Driver");
  i.version = HSF_STR_LIT("0.1.0");
  i.description = HSF_STR_LIT("Replace this with your protocol");
  i.author = HSF_STR_LIT("Your Name");
  i.kind = HSF_PLUGIN_KIND_DRIVER;
  i.permissions = HSF_PERM_SERIAL | HSF_PERM_NETWORK;
  i.platform = HSF_STR_LIT(HSF_PLATFORM_TRIPLE);
  return i;
}();

HSFStatus Create(const HSFPluginContext* ctx, HSFPlugin** out_plugin,
                 const HSFPluginVTable** out_vtable) {
  (void)ctx;
  return hsf::Guard([&]() -> HSFStatus {
    *out_plugin = reinterpret_cast<HSFPlugin*>(new Plugin());
    *out_vtable = &kVTable;
    return HSF_OK;
  });
}

void Destroy(HSFPlugin* p) { hsf::GuardVoid([&] { delete Cast(p); }); }

}  // namespace

HSF_PLUGIN_DEFINE(&kInfo, Create, Destroy)

#endif /* HSF_PLUGIN_TESTING */
