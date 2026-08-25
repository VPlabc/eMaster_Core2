#include "c3_driver.h"

#include "hsf/zk_controller/C3Codec.h"

#include <cstring>

namespace hsf_c3protocol {
namespace {

HSFStatus ReadFrame(hsf::BlockingIo& io, int timeout_ms, std::vector<uint8_t>* frame) {
  uint8_t header[5]{};
  HSFStatus status = io.ReadExact(header, sizeof(header), timeout_ms);
  if (status < 0) return status;
  if (header[0] != hsf::c3::kFrameStart) return HSF_ERR_PROTOCOL;
  const size_t total = 5u + static_cast<size_t>(header[3] | (header[4] << 8)) + 3u;
  if (total > 4096u) return HSF_ERR_PROTOCOL;
  frame->assign(header, header + sizeof(header));
  frame->resize(total);
  return io.ReadExact(frame->data() + sizeof(header), total - sizeof(header), timeout_ms);
}

}  // namespace

HSFStatus C3Driver::Initialize(HSFConfigRef config, HSFTransportRef transport) {
  if (!hsf_transport_valid(transport)) {
    SetError("no transport was supplied");
    return HSF_ERR_CONFIG;
  }
  timeout_ms_ = static_cast<int>(hsf_cfg_i64(config, "timeout_ms", 2000));
  if (timeout_ms_ < 1) {
    SetError("timeout_ms is invalid");
    return HSF_ERR_CONFIG;
  }
  transport_ = transport;
  io_ = hsf::BlockingIo(transport);
  SetState(HSF_DRIVER_READY);
  return HSF_OK;
}

HSFStatus C3Driver::Start() {
  HSFStatus status = transport_.vt->open(transport_.self);
  if (status == HSF_ERR_ALREADY_OPEN) status = HSF_OK;
  if (status < 0) {
    SetError("cannot open transport");
    SetState(HSF_DRIVER_FAILED);
    return status;
  }
  SetState(HSF_DRIVER_RUNNING);
  return Health();
}

HSFStatus C3Driver::Stop() {
  if (hsf_transport_valid(transport_)) transport_.vt->close(transport_.self);
  SetConnected(false);
  SetState(HSF_DRIVER_READY);
  return HSF_OK;
}

HSFStatus C3Driver::Exchange(const std::vector<uint8_t>& request, std::vector<uint8_t>* reply) {
  HSFStatus status = io_.WriteAll(request.data(), request.size(), timeout_ms_);
  if (status < 0) return status;
  return ReadFrame(io_, timeout_ms_, reply);
}

HSFStatus C3Driver::ConnectSessionLess() {
  std::vector<uint8_t> reply;
  HSFStatus status = Exchange(hsf::c3::EncodeConnectSessionLess(""), &reply);
  if (status < 0) return status;
  return hsf::c3::DecodeGenericReply(reply).status == hsf::c3::ResponseStatus::kOk
             ? HSF_OK
             : HSF_ERR_PROTOCOL;
}

HSFStatus C3Driver::Health() {
  if (state_ != HSF_DRIVER_RUNNING) return HSF_ERR_STATE;
  const HSFStatus status = ConnectSessionLess();
  if (status == HSF_OK) {
    ++health_ok_;
    SetConnected(true);
    SetError("");
  } else {
    ++health_failed_;
    SetConnected(false);
    SetError(hsf_status_name(status));
  }
  return status;
}

HSFStatus C3Driver::Read(const HSFDeviceAddress* address, HSFValue* out) {
  if (!address || !out) return HSF_ERR_INVALID_ARG;
  if (address->a1 != 0) return HSF_ERR_NOT_SUPPORTED;
  *out = hsf_value_bool(connected_);
  return HSF_OK;
}

HSFStatus C3Driver::Write(const HSFDeviceAddress*, const HSFValue*) {
  return HSF_ERR_NOT_SUPPORTED;
}

void C3Driver::Status(HSFDriverStatus* out) {
  hsf::DriverBase<C3Driver>::Status(out);
  if (!out) return;
  out->reads_ok = health_ok_;
  out->reads_failed = health_failed_;
}

}  // namespace hsf_c3protocol

#ifndef HSF_PLUGIN_TESTING
namespace {
using hsf_c3protocol::C3Driver;
const HSFTransportKind kTransports[] = {HSF_TRANSPORT_TCP, HSF_TRANSPORT_MOCK};
struct Plugin { C3Driver driver; };
Plugin* Cast(HSFPlugin* p) { return reinterpret_cast<Plugin*>(p); }
HSFStatus GetDriver(HSFPlugin* p, HSFDriverRef* out, HSFDriverInfo* info) {
  if (!p || !out) return HSF_ERR_INVALID_ARG;
  out->self = Cast(p)->driver.Self();
  out->vt = C3Driver::VTable();
  if (info) {
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->supported_transports = kTransports;
    info->supported_transport_count = 2;
    info->max_frame_bytes = 4096;
  }
  return HSF_OK;
}
const HSFPluginVTable kVTable = [] { HSFPluginVTable v{}; v.struct_size = sizeof(v); v.get_driver = GetDriver; return v; }();
const HSFPluginInfo kInfo = [] {
  HSFPluginInfo i{};
  i.struct_size = sizeof(i);
  i.id = HSF_STR_LIT("hsf.driver.c3protocol");
  i.name = HSF_STR_LIT("HSF C3 Protocol Driver");
  i.version = HSF_STR_LIT("1.0.0");
  i.description = HSF_STR_LIT("ZKTeco C3/InBio protocol over TCP");
  i.author = HSF_STR_LIT("HSF");
  i.kind = HSF_PLUGIN_KIND_DRIVER;
  i.permissions = HSF_PERM_NETWORK;
  i.platform = HSF_STR_LIT(HSF_PLATFORM_TRIPLE);
  return i;
}();
HSFStatus Create(const HSFPluginContext*, HSFPlugin** p, const HSFPluginVTable** v) {
  return hsf::Guard([&] { *p = reinterpret_cast<HSFPlugin*>(new Plugin()); *v = &kVTable; return HSF_OK; });
}
void Destroy(HSFPlugin* p) { hsf::GuardVoid([&] { delete Cast(p); }); }
}  // namespace
HSF_PLUGIN_DEFINE(&kInfo, Create, Destroy)
#endif
