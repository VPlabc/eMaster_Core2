#include "modbus_driver.h"

#include <cstring>

namespace hsf_modbus {
namespace {

void U16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}
uint16_t GetU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

bool Address(const HSFDeviceAddress& a, uint16_t* out) {
  if (a.a2 < 0 || a.a2 > 65535) return false;
  *out = static_cast<uint16_t>(a.a2);
  return true;
}
}  // namespace

HSFStatus ModbusDriver::Initialize(HSFConfigRef config, HSFTransportRef transport) {
  if (!hsf_transport_valid(transport)) { SetError("no transport was supplied"); return HSF_ERR_CONFIG; }
  timeout_ms_ = static_cast<int>(hsf_cfg_i64(config, "timeout_ms", 2000));
  unit_id_ = static_cast<int>(hsf_cfg_i64(config, "unit_id", 1));
  if (timeout_ms_ < 1 || unit_id_ < 0 || unit_id_ > 255) {
    SetError("timeout_ms or unit_id is invalid"); return HSF_ERR_CONFIG;
  }
  transport_ = transport;
  io_ = hsf::BlockingIo(transport);
  SetState(HSF_DRIVER_READY);
  return HSF_OK;
}

HSFStatus ModbusDriver::Start() {
  HSFStatus st = transport_.vt->open(transport_.self);
  if (st == HSF_ERR_ALREADY_OPEN) st = HSF_OK;
  if (st < 0) { SetError("cannot open transport"); SetState(HSF_DRIVER_FAILED); return st; }
  SetState(HSF_DRIVER_RUNNING); SetConnected(true); SetError("");
  return HSF_OK;
}
HSFStatus ModbusDriver::Stop() {
  if (hsf_transport_valid(transport_)) transport_.vt->close(transport_.self);
  SetConnected(false); SetState(HSF_DRIVER_READY); return HSF_OK;
}

HSFStatus ModbusDriver::Exchange(uint8_t function, const std::vector<uint8_t>& request,
                                  std::vector<uint8_t>* response) {
  std::vector<uint8_t> frame;
  const uint16_t tx = ++transaction_;
  U16(frame, tx); U16(frame, 0); U16(frame, static_cast<uint16_t>(request.size() + 2));
  frame.push_back(static_cast<uint8_t>(unit_id_)); frame.push_back(function);
  frame.insert(frame.end(), request.begin(), request.end());
  HSFStatus st = io_.WriteAll(frame.data(), frame.size(), timeout_ms_);
  if (st < 0) return st;
  uint8_t header[7];
  st = io_.ReadExact(header, sizeof(header), timeout_ms_);
  if (st < 0) return st;
  const uint16_t length = GetU16(header + 4);
  if (GetU16(header) != tx || GetU16(header + 2) != 0 || length < 2 || length > 254 ||
      header[6] != static_cast<uint8_t>(unit_id_)) return HSF_ERR_PROTOCOL;
  response->resize(length - 1);
  st = io_.ReadExact(response->data(), response->size(), timeout_ms_);
  if (st < 0) return st;
  if ((*response)[0] == static_cast<uint8_t>(function | 0x80)) return HSF_ERR_PROTOCOL;
  return (*response)[0] == function ? HSF_OK : HSF_ERR_PROTOCOL;
}

HSFStatus ModbusDriver::ReadBits(const HSFDeviceAddress& a, HSFValue* out) {
  uint16_t address; if (!Address(a, &address)) return HSF_ERR_INVALID_ARG;
  const uint8_t function = a.a1 == 1 ? 1 : a.a1 == 2 ? 2 : 0;
  if (!function) return HSF_ERR_NOT_SUPPORTED;
  std::vector<uint8_t> req; U16(req, address); U16(req, 1); std::vector<uint8_t> reply;
  HSFStatus st = Exchange(function, req, &reply);
  if (st < 0 || reply.size() != 3 || reply[1] != 1) return st < 0 ? st : HSF_ERR_PROTOCOL;
  *out = hsf_value_bool(reply[2] & 1); return HSF_OK;
}

HSFStatus ModbusDriver::ReadRegister(const HSFDeviceAddress& a, HSFValue* out) {
  uint16_t address; if (!Address(a, &address)) return HSF_ERR_INVALID_ARG;
  const uint8_t function = a.a1 == 3 ? 3 : a.a1 == 4 ? 4 : 0;
  if (!function || (a.encoding != HSF_ENC_U16 && a.encoding != HSF_ENC_I16)) return HSF_ERR_NOT_SUPPORTED;
  std::vector<uint8_t> req; U16(req, address); U16(req, 1); std::vector<uint8_t> reply;
  HSFStatus st = Exchange(function, req, &reply);
  if (st < 0 || reply.size() != 4 || reply[1] != 2) return st < 0 ? st : HSF_ERR_PROTOCOL;
  const uint16_t value = GetU16(reply.data() + 2);
  *out = a.encoding == HSF_ENC_I16 ? hsf_value_i64(static_cast<int16_t>(value)) : hsf_value_u64(value);
  return HSF_OK;
}

HSFStatus ModbusDriver::Read(const HSFDeviceAddress* a, HSFValue* out) {
  if (!a || !out || state_ != HSF_DRIVER_RUNNING) return !a || !out ? HSF_ERR_INVALID_ARG : HSF_ERR_STATE;
  HSFStatus st = (a->a1 == 1 || a->a1 == 2) ? ReadBits(*a, out) : ReadRegister(*a, out);
  if (st == HSF_OK) { ++reads_ok_; SetConnected(true); SetError(""); }
  else { ++reads_failed_; SetConnected(false); SetError(hsf_status_name(st)); }
  return st;
}

HSFStatus ModbusDriver::Write(const HSFDeviceAddress* a, const HSFValue* value) {
  if (!a || !value || state_ != HSF_DRIVER_RUNNING) return !a || !value ? HSF_ERR_INVALID_ARG : HSF_ERR_STATE;
  uint16_t address; if (!Address(*a, &address)) return HSF_ERR_INVALID_ARG;
  uint8_t function = 0; std::vector<uint8_t> req;
  if (a->a1 == 1 && value->kind == HSF_VALUE_BOOL) {
    function = 5; U16(req, address); U16(req, value->as.b ? 0xFF00 : 0);
  } else if (a->a1 == 3 && (value->kind == HSF_VALUE_U64 || value->kind == HSF_VALUE_I64)) {
    const int64_t v = value->kind == HSF_VALUE_U64 ? static_cast<int64_t>(value->as.u64) : value->as.i64;
    if (v < 0 || v > 65535) return HSF_ERR_INVALID_ARG;
    function = 6; U16(req, address); U16(req, static_cast<uint16_t>(v));
  } else return HSF_ERR_NOT_SUPPORTED;
  std::vector<uint8_t> reply; HSFStatus st = Exchange(function, req, &reply);
  if (st == HSF_OK && reply.size() == 5 && std::memcmp(reply.data() + 1, req.data(), 4) == 0) {
    ++writes_ok_; SetConnected(true); SetError(""); return HSF_OK;
  }
  ++writes_failed_; SetConnected(false); SetError(st < 0 ? hsf_status_name(st) : "invalid write reply");
  return st < 0 ? st : HSF_ERR_PROTOCOL;
}

HSFStatus ModbusDriver::Health() {
  HSFDeviceAddress a = hsf_device_address_init(); a.a1 = 1; a.a2 = 0;
  HSFValue value{}; return Read(&a, &value);
}
void ModbusDriver::Status(HSFDriverStatus* out) {
  hsf::DriverBase<ModbusDriver>::Status(out); if (!out) return;
  out->reads_ok = reads_ok_; out->reads_failed = reads_failed_;
  out->writes_ok = writes_ok_; out->writes_failed = writes_failed_;
}

}  // namespace hsf_modbus

#ifndef HSF_PLUGIN_TESTING
namespace {
using hsf_modbus::ModbusDriver;
const HSFTransportKind kTransports[] = {HSF_TRANSPORT_TCP, HSF_TRANSPORT_MOCK};
struct Plugin { ModbusDriver driver; };
Plugin* Cast(HSFPlugin* p) { return reinterpret_cast<Plugin*>(p); }
HSFStatus GetDriver(HSFPlugin* p, HSFDriverRef* out, HSFDriverInfo* info) {
  if (!p || !out) return HSF_ERR_INVALID_ARG;
  out->self = Cast(p)->driver.Self(); out->vt = ModbusDriver::VTable();
  if (info) { std::memset(info, 0, sizeof(*info)); info->struct_size = sizeof(*info);
    info->supported_transports = kTransports; info->supported_transport_count = 2;
    info->owns_thread = 1; info->max_frame_bytes = 260; }
  return HSF_OK;
}
const HSFPluginVTable kVTable = [] { HSFPluginVTable v{}; v.struct_size = sizeof(v); v.get_driver = GetDriver; return v; }();
const HSFPluginInfo kInfo = [] { HSFPluginInfo i{}; i.struct_size = sizeof(i);
  i.id = HSF_STR_LIT("hsf.driver.modbus"); i.name = HSF_STR_LIT("HSF Modbus TCP Driver");
  i.version = HSF_STR_LIT("1.0.0"); i.description = HSF_STR_LIT("Modbus TCP driver"); i.author = HSF_STR_LIT("HSF");
  i.kind = HSF_PLUGIN_KIND_DRIVER; i.permissions = HSF_PERM_NETWORK; i.platform = HSF_STR_LIT(HSF_PLATFORM_TRIPLE); return i; }();
HSFStatus Create(const HSFPluginContext*, HSFPlugin** p, const HSFPluginVTable** v) {
  return hsf::Guard([&] { *p = reinterpret_cast<HSFPlugin*>(new Plugin()); *v = &kVTable; return HSF_OK; }); }
void Destroy(HSFPlugin* p) { hsf::GuardVoid([&] { delete Cast(p); }); }
}  // namespace
HSF_PLUGIN_DEFINE(&kInfo, Create, Destroy)
#endif
