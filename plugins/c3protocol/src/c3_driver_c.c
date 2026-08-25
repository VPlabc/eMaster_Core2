#define _POSIX_C_SOURCE 200809L

#include "hsf/plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define C3_FRAME_START 0xAAu
#define C3_FRAME_END 0x55u
#define C3_VERSION 0x01u
#define C3_CONNECT_SESSIONLESS 0x01u
#define C3_CONTROL 0x05u
#define C3_RTLOG_BINARY 0x0Bu
#define C3_RTLOG_KEYVALUE 0x79u
#define C3_REPLY_OK 0xC8u
#define C3_REPLY_ERROR 0xC9u
#define C3_MAX_FRAME 4096u

struct HSFDriver {
  HSFTransportRef transport;
  HSFDriverStatus status;
  int timeout_ms;
  int running;
  int rtlog_keyvalue;
  char password[64];
  char last_error[160];
  uint8_t frame[C3_MAX_FRAME];
  uint8_t value_buffer[C3_MAX_FRAME];
  size_t value_length;
  uint8_t input_state[256];
  uint8_t input_known[256];
};

struct HSFPlugin {
  HSFLoggerRef log;
  struct HSFDriver driver;
};

static const HSFTransportKind kTransports[] = {HSF_TRANSPORT_TCP, HSF_TRANSPORT_MOCK};

static const HSFPluginInfo kInfo = {
    sizeof(HSFPluginInfo), HSF_STR_LIT("hsf.driver.c3protocol"),
    HSF_STR_LIT("HSF C3 Protocol Driver"), HSF_STR_LIT("1.1.2"),
    HSF_STR_LIT("Native C11 ZKTeco C3/InBio protocol driver"), HSF_STR_LIT("HSF"),
    HSF_PLUGIN_KIND_DRIVER, HSF_PERM_NETWORK, HSF_STR_LIT(HSF_PLATFORM_TRIPLE)};

static void set_error(HSFDriver* self, const char* message) {
  if (!self) return;
  snprintf(self->last_error, sizeof(self->last_error), "%s", message ? message : "");
  self->status.last_error = hsf_cstr(self->last_error);
}

static uint16_t read_u16_le(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t crc16_arc(const uint8_t* data, size_t length) {
  uint16_t crc = 0;
  size_t i;
  for (i = 0; i < length; ++i) {
    int bit;
    crc ^= data[i];
    for (bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
  }
  return crc;
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
    if (transferred == 0) return HSF_ERR_CLOSED;
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
    if (transferred == 0) return HSF_ERR_CLOSED;
    offset += transferred;
  }
  return HSF_OK;
}

static size_t encode_frame(uint8_t command, const uint8_t* payload,
                           size_t payload_length, uint8_t* out, size_t capacity) {
  uint16_t crc;
  size_t frame_length = payload_length + 8;
  if (payload_length > 0xFFFFu || frame_length > capacity) return 0;
  out[0] = C3_FRAME_START;
  out[1] = C3_VERSION;
  out[2] = command;
  out[3] = (uint8_t)payload_length;
  out[4] = (uint8_t)(payload_length >> 8);
  if (payload_length) memcpy(out + 5, payload, payload_length);
  crc = crc16_arc(out + 1, payload_length + 4);
  out[5 + payload_length] = (uint8_t)crc;
  out[6 + payload_length] = (uint8_t)(crc >> 8);
  out[7 + payload_length] = C3_FRAME_END;
  return frame_length;
}

static HSFStatus read_frame(HSFDriver* self, size_t* frame_length) {
  uint16_t payload_length;
  size_t total;
  uint16_t expected_crc;
  HSFStatus status = read_exact(self, self->frame, 5);
  if (status < 0) return status;
  payload_length = read_u16_le(self->frame + 3);
  total = 5u + payload_length + 3u;
  if (total > sizeof(self->frame)) return HSF_ERR_PROTOCOL;
  status = read_exact(self, self->frame + 5, total - 5);
  if (status < 0) return status;
  expected_crc = read_u16_le(self->frame + 5 + payload_length);
  if (self->frame[0] != C3_FRAME_START || self->frame[total - 1] != C3_FRAME_END ||
      crc16_arc(self->frame + 1, 4u + payload_length) != expected_crc ||
      (self->frame[2] != C3_REPLY_OK && self->frame[2] != C3_REPLY_ERROR)) {
    return HSF_ERR_PROTOCOL;
  }
  if (self->frame[2] == C3_REPLY_ERROR) return HSF_ERR_PROTOCOL;
  *frame_length = total;
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

static void disconnect_transport(HSFDriver* self) {
  if (self && hsf_transport_valid(self->transport) && self->transport.vt->close)
    self->transport.vt->close(self->transport.self);
  if (self) self->status.connected = 0;
}

static HSFStatus exchange_once(HSFDriver* self, uint8_t command,
                               const uint8_t* payload, size_t payload_length,
                               size_t* reply_length) {
  uint8_t request[256];
  size_t request_length = encode_frame(command, payload, payload_length,
                                       request, sizeof(request));
  HSFStatus status;
  if (!request_length) return HSF_ERR_INVALID_ARG;
  status = write_all(self, request, request_length);
  if (status < 0) return status;
  return read_frame(self, reply_length);
}

static HSFStatus connect_sessionless(HSFDriver* self) {
  size_t reply_length = 0;
  HSFStatus status = ensure_transport_open(self);
  if (status == HSF_OK) {
    status = exchange_once(self, C3_CONNECT_SESSIONLESS,
                           (const uint8_t*)self->password, strlen(self->password),
                           &reply_length);
  }
  if (reconnectable(status)) {
    disconnect_transport(self);
    status = ensure_transport_open(self);
    if (status == HSF_OK) {
      status = exchange_once(self, C3_CONNECT_SESSIONLESS,
                             (const uint8_t*)self->password, strlen(self->password),
                             &reply_length);
    }
  }
  self->status.connected = status == HSF_OK;
  if (status < 0 && reconnectable(status)) disconnect_transport(self);
  return status;
}

static HSFStatus exchange(HSFDriver* self, uint8_t command,
                          const uint8_t* payload, size_t payload_length,
                          size_t* reply_length) {
  HSFStatus status;
  if (!self->status.connected || !self->transport.vt->is_open ||
      !self->transport.vt->is_open(self->transport.self)) {
    status = connect_sessionless(self);
    if (status < 0) return status;
  }
  status = exchange_once(self, command, payload, payload_length, reply_length);
  if (!reconnectable(status)) return status;

  disconnect_transport(self);
  status = connect_sessionless(self);
  if (status < 0) return status;
  return exchange_once(self, command, payload, payload_length, reply_length);
}

static HSFStatus read_rtlog(HSFDriver* self) {
  size_t frame_length = 0;
  uint16_t payload_length;
  size_t offset;
  HSFStatus status = exchange(self,
                              self->rtlog_keyvalue ? C3_RTLOG_KEYVALUE
                                                   : C3_RTLOG_BINARY,
                              NULL, 0, &frame_length);
  (void)frame_length;
  if (status < 0) return status;
  payload_length = read_u16_le(self->frame + 3);
  if (!self->rtlog_keyvalue && (payload_length % 16u) != 0u) {
    /* A successful non-16-byte binary reply means this panel uses the
       key/value RTLog command. Switch once and stay in that mode. */
    self->rtlog_keyvalue = 1;
    status = exchange(self, C3_RTLOG_KEYVALUE, NULL, 0, &frame_length);
    if (status < 0) return status;
    payload_length = read_u16_le(self->frame + 3);
  }
  if (payload_length > sizeof(self->value_buffer)) return HSF_ERR_PROTOCOL;
  memcpy(self->value_buffer, self->frame + 5, payload_length);
  self->value_length = payload_length;
  if (self->rtlog_keyvalue) {
    if (payload_length >= sizeof(self->value_buffer)) return HSF_ERR_PROTOCOL;
    self->value_buffer[payload_length] = 0;
    const char* text = (const char*)self->value_buffer;
    const char* end = text + payload_length;
    const char* door = strstr(text, "door=");
    const char* event = strstr(text, "eventtype=");
    if (door && event && door < end && event < end) {
      int input = atoi(door + 5);
      int event_type = atoi(event + 10);
      if ((event_type == 220 || event_type == 221) && input > 0 && input < 256) {
        self->input_state[input] = event_type == 221 ? 1u : 0u;
        self->input_known[input] = 1u;
      }
    }
    return HSF_OK;
  }
  for (offset = 0; offset < payload_length; offset += 16) {
    const uint8_t* record = self->value_buffer + offset;
    uint8_t input = record[9];
    uint8_t event_type = record[10];
    if ((event_type == 220u || event_type == 221u) && input != 0u) {
      self->input_state[input] = event_type == 221u ? 1u : 0u;
      self->input_known[input] = 1u;
    }
  }
  return HSF_OK;
}

static HSFStatus control(HSFDriver* self, uint8_t operation, uint8_t number,
                         uint8_t address_type, uint8_t duration) {
  uint8_t payload[5] = {operation, number, address_type, duration, 0};
  size_t reply_length = 0;
  return exchange(self, C3_CONTROL, payload, sizeof(payload), &reply_length);
}

static void sleep_ms(uint64_t duration_ms) {
#if defined(_WIN32)
  Sleep((DWORD)duration_ms);
#else
  struct timespec request;
  request.tv_sec = (time_t)(duration_ms / 1000u);
  request.tv_nsec = (long)((duration_ms % 1000u) * 1000000u);
  while (nanosleep(&request, &request) != 0) {}
#endif
}

static HSFStatus pulse_output(HSFDriver* self, int number, int auxiliary,
                              uint64_t duration_ms) {
  HSFStatus status;
  HSFStatus zero_duration;
  HSFStatus cancel_latch;
  uint64_t hold_seconds;
  if (number < 1 || number > 255 || duration_ms > 60000u) return HSF_ERR_INVALID_ARG;
  hold_seconds = (duration_ms + 999u) / 1000u;
  if (hold_seconds < 1u) hold_seconds = 1u;
  status = control(self, 1, (uint8_t)number, auxiliary ? 2u : 1u,
                   (uint8_t)hold_seconds);
  if (status < 0) return status;
  sleep_ms(duration_ms);
  if (auxiliary) {
    return control(self, 1, (uint8_t)number, 2u, 0u);
  }
  /* Panels differ on how a door output is released. Send both accepted forms.
     The bounded hold above remains a fail-safe if neither release takes effect. */
  zero_duration = control(self, 1, (uint8_t)number, 1u, 0u);
  cancel_latch = control(self, 4, (uint8_t)number, 0u, 0u);
  return zero_duration == HSF_OK || cancel_latch == HSF_OK
             ? HSF_OK
             : (zero_duration < 0 ? zero_duration : cancel_latch);
}

static HSFStatus driver_initialize(HSFDriver* self, HSFConfigRef config,
                                   HSFTransportRef transport) {
  HSFStr password;
  size_t password_length;
  if (!self || !hsf_transport_valid(transport)) return HSF_ERR_CONFIG;
  memset(self, 0, sizeof(*self));
  self->transport = transport;
  self->timeout_ms = (int)hsf_cfg_i64(config, "timeout_ms", 2000);
  if (self->timeout_ms < 1) return HSF_ERR_CONFIG;
  password = hsf_cfg_str(config, "password", "");
  password_length = password.len < sizeof(self->password) - 1
                        ? password.len : sizeof(self->password) - 1;
  if (password.ptr && password_length) memcpy(self->password, password.ptr, password_length);
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
  status = connect_sessionless(self);
  self->status.connected = status == HSF_OK;
  set_error(self, status == HSF_OK ? "" : hsf_status_name(status));
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
  if (address->a1 == 0) {
    *out = hsf_value_bool(self->status.connected);
    return HSF_OK;
  }
  status = read_rtlog(self);
  if (status == HSF_OK && address->a1 == 1) {
    *out = hsf_value_blob(self->value_buffer, self->value_length);
  } else if (status == HSF_OK && address->a1 == 2) {
    int input = (int)address->a2;
    if (input < 1 || input > 255) status = HSF_ERR_INVALID_ARG;
    else if (!self->input_known[input]) status = HSF_ERR_NOT_FOUND;
    else *out = hsf_value_bool(self->input_state[input]);
  } else if (status == HSF_OK) {
    status = HSF_ERR_NOT_SUPPORTED;
  }
  if (status == HSF_OK) {
    self->status.reads_ok++;
    self->status.connected = 1;
    set_error(self, "");
  } else {
    self->status.reads_failed++;
    if (reconnectable(status)) self->status.connected = 0;
    set_error(self, hsf_status_name(status));
  }
  return status;
}

static HSFStatus driver_write(HSFDriver* self, const HSFDeviceAddress* address,
                              const HSFValue* value) {
  uint64_t duration_ms;
  HSFStatus status;
  if (!self || !address || !value) return HSF_ERR_INVALID_ARG;
  if (!self->running) return HSF_ERR_STATE;
  if (value->kind == HSF_VALUE_U64) duration_ms = value->as.u64;
  else if (value->kind == HSF_VALUE_I64 && value->as.i64 >= 0)
    duration_ms = (uint64_t)value->as.i64;
  else return HSF_ERR_INVALID_ARG;
  if (address->a1 != 3 && address->a1 != 4) return HSF_ERR_NOT_SUPPORTED;
  status = pulse_output(self, (int)address->a2, address->a1 == 4, duration_ms);
  if (status == HSF_OK) {
    self->status.writes_ok++;
    self->status.connected = 1;
    set_error(self, "");
  } else {
    self->status.writes_failed++;
    if (reconnectable(status)) self->status.connected = 0;
    set_error(self, hsf_status_name(status));
  }
  return status;
}

static void driver_status(const HSFDriver* self, HSFDriverStatus* out) {
  if (self && out) *out = self->status;
}

static HSFStatus driver_health(HSFDriver* self) {
  HSFStatus status;
  if (!self) return HSF_ERR_INVALID_ARG;
  if (!self->running) return HSF_ERR_STATE;
  status = connect_sessionless(self);
  self->status.connected = status == HSF_OK;
  if (status == HSF_OK) self->status.reads_ok++;
  else self->status.reads_failed++;
  set_error(self, status == HSF_OK ? "" : hsf_status_name(status));
  return status;
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
  info->max_frame_bytes = C3_MAX_FRAME;
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
