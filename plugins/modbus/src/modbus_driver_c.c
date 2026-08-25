#include "hsf/plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODBUS_MAX_ADU 260u
#define MODBUS_MAX_READ_BITS 2000u
#define MODBUS_MAX_READ_REGISTERS 125u
#define MODBUS_MAX_WRITE_BITS 1968u
#define MODBUS_MAX_WRITE_REGISTERS 123u

struct HSFDriver {
  HSFTransportRef transport;
  HSFDriverStatus status;
  int timeout_ms;
  int unit_id;
  int health_function;
  int health_address;
  int health_count;
  int running;
  uint16_t transaction;
  char last_error[160];
  uint8_t response[MODBUS_MAX_ADU];
  size_t response_length;
  uint8_t value_buffer[250];
};

struct HSFPlugin {
  HSFLoggerRef log;
  struct HSFDriver driver;
};

static const HSFTransportKind kTransports[] = {HSF_TRANSPORT_TCP, HSF_TRANSPORT_MOCK};

static const HSFPluginInfo kInfo = {
    sizeof(HSFPluginInfo), HSF_STR_LIT("hsf.driver.modbus"),
    HSF_STR_LIT("HSF Modbus TCP Driver"), HSF_STR_LIT("1.1.2"),
    HSF_STR_LIT("Native C11 Modbus TCP FC01/02/03/04/05/06/15/16 driver"),
    HSF_STR_LIT("HSF"), HSF_PLUGIN_KIND_DRIVER, HSF_PERM_NETWORK,
    HSF_STR_LIT(HSF_PLATFORM_TRIPLE)};

static void set_error(HSFDriver* self, const char* message) {
  snprintf(self->last_error, sizeof(self->last_error), "%s", message ? message : "");
  self->status.last_error = hsf_cstr(self->last_error);
}

static uint16_t get_u16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_u16(uint8_t* p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static HSFStatus await_io(HSFDriver* self, uint32_t events) {
  uint32_t ready = 0;
  HSFStatus status = self->transport.vt->wait(
      self->transport.self, events, self->timeout_ms, &ready);
  if (status < 0) return status;
  if (ready & HSF_IO_CLOSED) return HSF_ERR_CLOSED;
  if (ready & HSF_IO_ERROR) return HSF_ERR_IO;
  return (ready & events) ? HSF_OK : HSF_ERR_TIMEOUT;
}

static HSFStatus write_all(HSFDriver* self, const uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t transferred = 0;
    HSFStatus status = self->transport.vt->write(
        self->transport.self, data + offset, length - offset, &transferred);
    if (status == HSF_AGAIN) {
      status = await_io(self, HSF_IO_WRITE);
      if (status < 0) return status;
      continue;
    }
    if (status < 0) return status;
    if (!transferred) return HSF_ERR_CLOSED;
    offset += transferred;
  }
  return HSF_OK;
}

static HSFStatus read_exact(HSFDriver* self, uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t transferred = 0;
    HSFStatus status = self->transport.vt->read(
        self->transport.self, data + offset, length - offset, &transferred);
    if (status == HSF_AGAIN) {
      status = await_io(self, HSF_IO_READ);
      if (status < 0) return status;
      continue;
    }
    if (status < 0) return status;
    if (!transferred) return HSF_ERR_CLOSED;
    offset += transferred;
  }
  return HSF_OK;
}

static HSFStatus ensure_transport_open(HSFDriver* self) {
  HSFStatus status;
  if (!self || !hsf_transport_valid(self->transport)) return HSF_ERR_NOT_OPEN;
  if (self->transport.vt->is_open &&
      self->transport.vt->is_open(self->transport.self)) return HSF_OK;
  if (!self->transport.vt->open) return HSF_ERR_NOT_SUPPORTED;
  status = self->transport.vt->open(self->transport.self);
  return status == HSF_ERR_ALREADY_OPEN ? HSF_OK : status;
}

static int reconnectable(HSFStatus status) {
  return status == HSF_ERR_IO || status == HSF_ERR_NOT_OPEN ||
         status == HSF_ERR_CLOSED || status == HSF_ERR_TIMEOUT;
}

static HSFStatus exchange_once(HSFDriver* self, uint8_t function,
                               const uint8_t* payload, size_t payload_length) {
  uint8_t request[MODBUS_MAX_ADU];
  uint8_t header[7];
  uint16_t transaction = ++self->transaction;
  uint16_t length;
  HSFStatus status;
  if (payload_length + 8u > sizeof(request)) return HSF_ERR_INVALID_ARG;
  put_u16(request, transaction);
  put_u16(request + 2, 0);
  put_u16(request + 4, (uint16_t)(payload_length + 2));
  request[6] = (uint8_t)self->unit_id;
  request[7] = function;
  if (payload_length) memcpy(request + 8, payload, payload_length);
  status = write_all(self, request, payload_length + 8);
  if (status < 0) return status;
  status = read_exact(self, header, sizeof(header));
  if (status < 0) return status;
  length = get_u16(header + 4);
  if (get_u16(header) != transaction || get_u16(header + 2) != 0 ||
      header[6] != (uint8_t)self->unit_id || length < 2 || length > 254) {
    return HSF_ERR_PROTOCOL;
  }
  self->response_length = length - 1;
  status = read_exact(self, self->response, self->response_length);
  if (status < 0) return status;
  if (self->response[0] == (uint8_t)(function | 0x80u)) return HSF_ERR_PROTOCOL;
  return self->response[0] == function ? HSF_OK : HSF_ERR_PROTOCOL;
}

static HSFStatus exchange(HSFDriver* self, uint8_t function,
                          const uint8_t* payload, size_t payload_length) {
  HSFStatus status = ensure_transport_open(self);
  if (status < 0) return status;
  status = exchange_once(self, function, payload, payload_length);
  if (!reconnectable(status)) return status;

  if (self->transport.vt->close)
    self->transport.vt->close(self->transport.self);
  status = ensure_transport_open(self);
  if (status < 0) return status;
  return exchange_once(self, function, payload, payload_length);
}

static int address_and_count(const HSFDeviceAddress* address, uint16_t* start,
                             uint16_t* count) {
  int64_t requested = address->a3 > 0 ? address->a3 : 1;
  if (address->a2 < 0 || address->a2 > 65535 || requested < 1 || requested > 65535)
    return 0;
  *start = (uint16_t)address->a2;
  *count = (uint16_t)requested;
  return 1;
}

static HSFStatus read_point(HSFDriver* self, const HSFDeviceAddress* address,
                            HSFValue* out) {
  uint8_t function = (uint8_t)address->a1;
  uint16_t start, count;
  uint8_t request[4];
  uint8_t byte_count;
  HSFStatus status;
  if (function < 1 || function > 4) return HSF_ERR_NOT_SUPPORTED;
  if (!address_and_count(address, &start, &count)) return HSF_ERR_INVALID_ARG;
  if ((function <= 2 && count > MODBUS_MAX_READ_BITS) ||
      (function >= 3 && count > MODBUS_MAX_READ_REGISTERS)) return HSF_ERR_INVALID_ARG;
  put_u16(request, start);
  put_u16(request + 2, count);
  status = exchange(self, function, request, sizeof(request));
  if (status < 0) return status;
  if (self->response_length < 2) return HSF_ERR_PROTOCOL;
  byte_count = self->response[1];
  if (self->response_length != (size_t)byte_count + 2u ||
      byte_count > sizeof(self->value_buffer)) return HSF_ERR_PROTOCOL;
  memcpy(self->value_buffer, self->response + 2, byte_count);
  if (function <= 2 && count == 1) {
    *out = hsf_value_bool(self->value_buffer[0] & 1u);
  } else if (function >= 3 && count == 1) {
    uint16_t raw;
    if (byte_count != 2) return HSF_ERR_PROTOCOL;
    raw = get_u16(self->value_buffer);
    *out = address->encoding == HSF_ENC_I16
               ? hsf_value_i64((int16_t)raw) : hsf_value_u64(raw);
  } else {
    *out = hsf_value_blob(self->value_buffer, byte_count);
  }
  return HSF_OK;
}

static HSFStatus write_single(HSFDriver* self, uint8_t function,
                              uint16_t address, uint16_t raw) {
  uint8_t request[4];
  HSFStatus status;
  put_u16(request, address);
  put_u16(request + 2, raw);
  status = exchange(self, function, request, sizeof(request));
  if (status < 0) return status;
  if (self->response_length != 5 ||
      memcmp(self->response + 1, request, sizeof(request)) != 0) return HSF_ERR_PROTOCOL;
  return HSF_OK;
}

static HSFStatus write_multiple(HSFDriver* self, uint8_t function,
                                uint16_t address, uint16_t count,
                                const HSFBlob* blob) {
  uint8_t request[MODBUS_MAX_ADU];
  size_t byte_count;
  HSFStatus status;
  if (!blob || !blob->ptr) return HSF_ERR_INVALID_ARG;
  if (function == 15) {
    if (count < 1 || count > MODBUS_MAX_WRITE_BITS) return HSF_ERR_INVALID_ARG;
    byte_count = (count + 7u) / 8u;
  } else {
    if (count < 1 || count > MODBUS_MAX_WRITE_REGISTERS) return HSF_ERR_INVALID_ARG;
    byte_count = (size_t)count * 2u;
  }
  if (blob->len != byte_count || byte_count > 246u) return HSF_ERR_INVALID_ARG;
  put_u16(request, address);
  put_u16(request + 2, count);
  request[4] = (uint8_t)byte_count;
  memcpy(request + 5, blob->ptr, byte_count);
  status = exchange(self, function, request, byte_count + 5u);
  if (status < 0) return status;
  if (self->response_length != 5 ||
      memcmp(self->response + 1, request, 4) != 0) return HSF_ERR_PROTOCOL;
  return HSF_OK;
}

static HSFStatus driver_initialize(HSFDriver* self, HSFConfigRef config,
                                   HSFTransportRef transport) {
  if (!self || !hsf_transport_valid(transport)) return HSF_ERR_CONFIG;
  memset(self, 0, sizeof(*self));
  self->transport = transport;
  self->timeout_ms = (int)hsf_cfg_i64(config, "timeout_ms", 2000);
  self->unit_id = (int)hsf_cfg_i64(config, "unit_id", 1);
  self->health_function = (int)hsf_cfg_i64(config, "health_function", 1);
  self->health_address = (int)hsf_cfg_i64(config, "health_address", 0);
  self->health_count = (int)hsf_cfg_i64(config, "health_count", 1);
  if (self->timeout_ms < 1 || self->unit_id < 0 || self->unit_id > 255 ||
      self->health_function < 1 || self->health_function > 4 ||
      self->health_address < 0 || self->health_address > 65535 ||
      self->health_count < 1 ||
      (self->health_function <= 2 &&
       self->health_count > (int)MODBUS_MAX_READ_BITS) ||
      (self->health_function >= 3 &&
       self->health_count > (int)MODBUS_MAX_READ_REGISTERS))
    return HSF_ERR_CONFIG;
  self->status.struct_size = sizeof(self->status);
  self->status.state = HSF_DRIVER_READY;
  self->status.last_error = hsf_str_empty();
  return HSF_OK;
}

static HSFStatus driver_start(HSFDriver* self) {
  HSFStatus status;
  if (!self) return HSF_ERR_INVALID_ARG;
  self->running = 1;
  self->status.state = HSF_DRIVER_RUNNING;
  status = ensure_transport_open(self);
  if (status < 0) {
    self->status.connected = 0;
    set_error(self, "transport offline; reconnecting on the next operation");
    return HSF_OK;
  }
  self->status.connected = 1;
  set_error(self, "");
  return HSF_OK;
}

static HSFStatus driver_stop(HSFDriver* self) {
  if (!self) return HSF_ERR_INVALID_ARG;
  if (hsf_transport_valid(self->transport)) self->transport.vt->close(self->transport.self);
  self->running = 0;
  self->status.connected = 0;
  self->status.state = HSF_DRIVER_READY;
  return HSF_OK;
}

static HSFStatus driver_read(HSFDriver* self, const HSFDeviceAddress* address,
                             HSFValue* out) {
  HSFStatus status;
  if (!self || !address || !out) return HSF_ERR_INVALID_ARG;
  if (!self->running) return HSF_ERR_STATE;
  status = read_point(self, address, out);
  if (status == HSF_OK) {
    self->status.reads_ok++;
    self->status.connected = 1;
    set_error(self, "");
  } else {
    self->status.reads_failed++;
    self->status.connected = 0;
    set_error(self, hsf_status_name(status));
  }
  return status;
}

static HSFStatus driver_write(HSFDriver* self, const HSFDeviceAddress* address,
                              const HSFValue* value) {
  uint8_t function;
  uint16_t start, count;
  HSFStatus status;
  if (!self || !address || !value) return HSF_ERR_INVALID_ARG;
  if (!self->running) return HSF_ERR_STATE;
  if (!address_and_count(address, &start, &count)) return HSF_ERR_INVALID_ARG;
  function = (uint8_t)address->a1;
  if (function == 1) function = 5;
  if (function == 3) function = 6;
  if (function == 5 && value->kind == HSF_VALUE_BOOL) {
    status = write_single(self, 5, start, value->as.b ? 0xFF00u : 0u);
  } else if (function == 6 &&
             (value->kind == HSF_VALUE_U64 || value->kind == HSF_VALUE_I64)) {
    int64_t raw = value->kind == HSF_VALUE_U64 ? (int64_t)value->as.u64 : value->as.i64;
    status = raw < 0 || raw > 65535 ? HSF_ERR_INVALID_ARG
                                   : write_single(self, 6, start, (uint16_t)raw);
  } else if ((function == 15 || function == 16) && value->kind == HSF_VALUE_BLOB) {
    status = write_multiple(self, function, start, count, &value->as.blob);
  } else {
    status = HSF_ERR_NOT_SUPPORTED;
  }
  if (status == HSF_OK) {
    self->status.writes_ok++;
    self->status.connected = 1;
    set_error(self, "");
  } else {
    self->status.writes_failed++;
    self->status.connected = 0;
    set_error(self, hsf_status_name(status));
  }
  return status;
}

static void driver_status(const HSFDriver* self, HSFDriverStatus* out) {
  if (self && out) *out = self->status;
}

static HSFStatus driver_health(HSFDriver* self) {
  HSFDeviceAddress address = hsf_device_address_init();
  HSFValue value;
  if (!self) return HSF_ERR_INVALID_ARG;
  address.a1 = self->health_function;
  address.a2 = self->health_address;
  address.a3 = self->health_count;
  return driver_read(self, &address, &value);
}

static const HSFDriverVTable kDriverVTable = {
    sizeof(HSFDriverVTable), driver_initialize, driver_start, driver_stop,
    driver_read, driver_write, NULL, NULL, driver_status, driver_health,
    NULL, NULL, NULL, NULL};

static HSFStatus plugin_get_driver(HSFPlugin* self, HSFDriverRef* out,
                                   HSFDriverInfo* info) {
  if (!self || !out || !info) return HSF_ERR_INVALID_ARG;
  out->self = &self->driver;
  out->vt = &kDriverVTable;
  memset(info, 0, sizeof(*info));
  info->struct_size = sizeof(*info);
  info->supported_transports = kTransports;
  info->supported_transport_count = sizeof(kTransports) / sizeof(kTransports[0]);
  info->owns_thread = 1;
  info->max_frame_bytes = MODBUS_MAX_ADU;
  return HSF_OK;
}

static const HSFPluginVTable kPluginVTable = {
    sizeof(HSFPluginVTable), plugin_get_driver, NULL, NULL, NULL};

static HSFStatus create_plugin(const HSFPluginContext* context,
                               HSFPlugin** out_plugin,
                               const HSFPluginVTable** out_vtable) {
  HSFPlugin* plugin;
  if (!context || !out_plugin || !out_vtable) return HSF_ERR_INVALID_ARG;
  plugin = (HSFPlugin*)calloc(1, sizeof(*plugin));
  if (!plugin) return HSF_ERR_NO_MEMORY;
  plugin->log = context->log;
  *out_plugin = plugin;
  *out_vtable = &kPluginVTable;
  return HSF_OK;
}

static void destroy_plugin(HSFPlugin* plugin) {
  if (!plugin) return;
  if (plugin->driver.running) driver_stop(&plugin->driver);
  free(plugin);
}

HSF_PLUGIN_DEFINE(&kInfo, create_plugin, destroy_plugin)
