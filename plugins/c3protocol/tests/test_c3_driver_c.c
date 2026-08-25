#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>

#include "../src/c3_driver_c.c"

struct HSFTransport {
  uint8_t response[C3_MAX_FRAME];
  size_t response_length;
  size_t response_offset;
  int open;
  int open_failures;
  int connect_requests;
  int keyvalue_rtlog;
  int keyvalue_requests;
  int control_requests;
};

static HSFTransportKind mock_kind(const HSFTransport* self) {
  (void)self;
  return HSF_TRANSPORT_MOCK;
}
static HSFFraming mock_framing(const HSFTransport* self) {
  (void)self;
  return HSF_FRAMING_STREAM;
}
static HSFStatus mock_open(HSFTransport* self) {
  if (self->open_failures > 0) {
    self->open_failures--;
    return HSF_ERR_IO;
  }
  self->open = 1;
  return HSF_OK;
}
static void mock_close(HSFTransport* self) { self->open = 0; }
static int32_t mock_is_open(const HSFTransport* self) { return self->open; }
static HSFStatus mock_read(HSFTransport* self, void* buffer, size_t capacity,
                           size_t* transferred) {
  if (!self->open) return HSF_ERR_NOT_OPEN;
  size_t available = self->response_length - self->response_offset;
  size_t count = available < capacity ? available : capacity;
  if (!count) { *transferred = 0; return HSF_AGAIN; }
  memcpy(buffer, self->response + self->response_offset, count);
  self->response_offset += count;
  *transferred = count;
  return HSF_OK;
}
static HSFStatus mock_write(HSFTransport* self, const void* data, size_t length,
                            size_t* transferred) {
  const uint8_t* request = (const uint8_t*)data;
  uint8_t payload[16] = {0};
  size_t payload_length = 0;
  if (!self->open) return HSF_ERR_NOT_OPEN;
  assert(length >= 8);
  if (request[2] == C3_CONNECT_SESSIONLESS) {
    self->connect_requests++;
  } else if (request[2] == C3_RTLOG_BINARY) {
    if (self->keyvalue_rtlog) {
      memcpy(payload, "keyvalue", 8);
      payload_length = 8;
    } else {
      payload[9] = 1;
      payload[10] = 221;
      payload_length = sizeof(payload);
    }
  } else if (request[2] == C3_RTLOG_KEYVALUE) {
    static const char event[] = "cardno=1234,door=1,eventtype=221,verified=1";
    self->keyvalue_requests++;
    self->response_length = encode_frame(C3_REPLY_OK, (const uint8_t*)event,
                                         sizeof(event) - 1, self->response,
                                         sizeof(self->response));
    self->response_offset = 0;
    *transferred = length;
    return HSF_OK;
  } else if (request[2] == C3_CONTROL) {
    self->control_requests++;
  }
  self->response_length = encode_frame(C3_REPLY_OK, payload, payload_length,
                                       self->response, sizeof(self->response));
  self->response_offset = 0;
  *transferred = length;
  return HSF_OK;
}
static HSFStatus mock_wait(HSFTransport* self, uint32_t events, int32_t timeout_ms,
                           uint32_t* ready) {
  (void)timeout_ms;
  *ready = events & HSF_IO_WRITE;
  if ((events & HSF_IO_READ) && self->response_offset < self->response_length)
    *ready |= HSF_IO_READ;
  return *ready ? HSF_OK : HSF_ERR_TIMEOUT;
}
static HSFStatus mock_flush(HSFTransport* self) {
  self->response_offset = self->response_length;
  return HSF_OK;
}
static intptr_t mock_handle(const HSFTransport* self) { (void)self; return -1; }
static HSFStr mock_error(const HSFTransport* self) { (void)self; return hsf_str_empty(); }

static const HSFTransportVTable kMockVTable = {
    sizeof(HSFTransportVTable), mock_kind, mock_framing, mock_open, mock_close,
    mock_is_open, mock_read, mock_write, mock_wait, mock_flush, mock_handle,
    mock_error};

int main(void) {
  HSFTransport mock;
  HSFTransportRef transport;
  HSFDriver driver;
  HSFDeviceAddress address;
  HSFValue value;
  memset(&mock, 0, sizeof(mock));
  memset(&driver, 0, sizeof(driver));
  transport.vt = &kMockVTable;
  transport.self = &mock;

  assert(crc16_arc((const uint8_t*)"123456789", 9) == 0xBB3D);
  assert(driver_initialize(&driver, (HSFConfigRef){0}, transport) == HSF_OK);
  mock.open_failures = 2;
  /* Enable stays RUNNING while offline; the first operation reconnects. */
  assert(driver_start(&driver) == HSF_OK);
  assert(driver.status.state == HSF_DRIVER_RUNNING);
  assert(driver.status.connected == 0);

  address = hsf_device_address_init();
  address.a1 = 1;
  assert(driver_read(&driver, &address, &value) == HSF_OK);
  assert(value.kind == HSF_VALUE_BLOB && value.as.blob.len == 16);

  address.a1 = 2;
  address.a2 = 1;
  /* A transport dropped after startup is reopened with a fresh C3 session. */
  mock.open = 0;
  mock.keyvalue_rtlog = 1;
  assert(driver_read(&driver, &address, &value) == HSF_OK);
  assert(value.kind == HSF_VALUE_BOOL && value.as.b == 1);
  assert(mock.keyvalue_requests == 1);

  value = hsf_value_u64(500);
  address.a1 = 3;
  address.a2 = 1;
  assert(driver_write(&driver, &address, &value) == HSF_OK);
  address.a2 = 2;
  assert(driver_write(&driver, &address, &value) == HSF_OK);
  address.a1 = 4;
  address.a2 = 1;
  assert(driver_write(&driver, &address, &value) == HSF_OK);
  address.a2 = 2;
  assert(driver_write(&driver, &address, &value) == HSF_OK);
  assert(mock.control_requests == 10);
  assert(mock.connect_requests >= 2);

  assert(driver_stop(&driver) == HSF_OK);
  puts("c3 native C driver tests PASS");
  return 0;
}
