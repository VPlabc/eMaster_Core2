#include "hsf/plugin.h"

struct HSFDriver {
  int running;
  int64_t value;
  HSFDriverStatus status;
};

struct HSFPlugin {
  HSFLoggerRef log;
  struct HSFDriver driver;
};

static const HSFPluginInfo kInfo = {
    sizeof(HSFPluginInfo), HSF_STR_LIT("hsf.driver.c_example"),
    HSF_STR_LIT("C Example Driver"), HSF_STR_LIT("0.1.0"),
    HSF_STR_LIT("A minimal driver implemented in C11."), HSF_STR_LIT("HSF"),
    HSF_PLUGIN_KIND_DRIVER, 0, HSF_STR_LIT(HSF_PLATFORM_TRIPLE)};

static HSFStatus driver_initialize(HSFDriver* self, HSFConfigRef config,
                                   HSFTransportRef transport) {
  (void)config;
  (void)transport;
  if (!self) return HSF_ERR_INVALID_ARG;
  self->running = 0;
  self->value = 42;
  self->status.struct_size = sizeof(self->status);
  self->status.state = HSF_DRIVER_READY;
  self->status.connected = 1;
  self->status.last_error = hsf_str_empty();
  self->status.connected_since_ms = 0;
  return HSF_OK;
}

static HSFStatus driver_start(HSFDriver* self) {
  if (!self) return HSF_ERR_INVALID_ARG;
  self->running = 1;
  self->status.state = HSF_DRIVER_RUNNING;
  return HSF_OK;
}

static HSFStatus driver_stop(HSFDriver* self) {
  if (!self) return HSF_ERR_INVALID_ARG;
  self->running = 0;
  self->status.state = HSF_DRIVER_READY;
  return HSF_OK;
}

static HSFStatus driver_read(HSFDriver* self, const HSFDeviceAddress* address,
                             HSFValue* out) {
  (void)address;
  if (!self || !out) return HSF_ERR_INVALID_ARG;
  if (!self->running) return HSF_ERR_STATE;
  self->status.reads_ok++;
  *out = hsf_value_i64(self->value);
  return HSF_OK;
}

static HSFStatus driver_write(HSFDriver* self, const HSFDeviceAddress* address,
                              const HSFValue* value) {
  (void)address;
  if (!self || !value) return HSF_ERR_INVALID_ARG;
  if (!self->running) return HSF_ERR_STATE;
  if (value->kind != HSF_VALUE_I64) return HSF_ERR_INVALID_ARG;
  self->value = value->as.i64;
  self->status.writes_ok++;
  return HSF_OK;
}

static void driver_status(const HSFDriver* self, HSFDriverStatus* out) {
  if (self && out) *out = self->status;
}

static HSFStatus driver_health(HSFDriver* self) {
  if (!self) return HSF_ERR_INVALID_ARG;
  return self->running ? HSF_OK : HSF_ERR_STATE;
}

static const HSFDriverVTable kDriverVTable = {
    sizeof(HSFDriverVTable), driver_initialize, driver_start, driver_stop,
    driver_read, driver_write, NULL, NULL, driver_status, driver_health,
    NULL, NULL, NULL};

static HSFStatus plugin_get_driver(HSFPlugin* self, HSFDriverRef* out,
                                   HSFDriverInfo* info) {
  static const HSFTransportKind transports[] = {HSF_TRANSPORT_MOCK};
  if (!self || !out || !info) return HSF_ERR_INVALID_ARG;
  out->self = &self->driver;
  out->vt = &kDriverVTable;
  info->struct_size = sizeof(*info);
  info->supported_transports = transports;
  info->supported_transport_count = 1;
  info->tick_ms = 0;
  info->owns_thread = 0;
  info->max_frame_bytes = 0;
  return HSF_OK;
}

static const HSFPluginVTable kPluginVTable = {
    sizeof(HSFPluginVTable), plugin_get_driver, NULL, NULL};

static HSFStatus create_plugin(const HSFPluginContext* context,
                               HSFPlugin** out_plugin,
                               const HSFPluginVTable** out_vtable) {
  static struct HSFPlugin plugin;
  if (!context || !out_plugin || !out_vtable) return HSF_ERR_INVALID_ARG;
  plugin.log = context->log;
  *out_plugin = &plugin;
  *out_vtable = &kPluginVTable;
  HSF_LOG_I(plugin.log, "C example plugin created");
  return HSF_OK;
}

static void destroy_plugin(HSFPlugin* plugin) {
  if (plugin) plugin->driver.running = 0;
}

HSF_PLUGIN_DEFINE(&kInfo, create_plugin, destroy_plugin)
