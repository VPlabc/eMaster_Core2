#include <assert.h>
#include <stdio.h>

#include "../src/modbus_driver_c.c"

struct HSFTransport {
  uint8_t response[MODBUS_MAX_ADU];
  size_t response_length;
  size_t response_offset;
  int open;
  int open_failures;
  unsigned seen[17];
};

static HSFTransportKind mock_kind(const HSFTransport* self) { (void)self; return HSF_TRANSPORT_MOCK; }
static HSFFraming mock_framing(const HSFTransport* self) { (void)self; return HSF_FRAMING_STREAM; }
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
  uint8_t function = request[7];
  uint8_t* response = self->response;
  size_t pdu_length;
  assert(length >= 12 && function <= 16);
  self->seen[function]++;
  memcpy(response, request, 7);
  response[7] = function;
  if (function == 1 || function == 2) {
    response[8] = 1;
    response[9] = 1;
    pdu_length = 3;
  } else if (function == 3 || function == 4) {
    response[8] = 2;
    response[9] = 0x12;
    response[10] = 0x34;
    pdu_length = 4;
  } else {
    memcpy(response + 8, request + 8, 4);
    pdu_length = 5;
  }
  put_u16(response + 4, (uint16_t)(pdu_length + 1));
  self->response_length = 7 + pdu_length;
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
static HSFStatus mock_flush(HSFTransport* self) { self->response_offset = self->response_length; return HSF_OK; }
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
  uint8_t coils[2] = {0x55, 0x01};
  uint8_t registers[4] = {0x00, 0x11, 0x00, 0x22};
  int function;
  memset(&mock, 0, sizeof(mock));
  memset(&driver, 0, sizeof(driver));
  transport.vt = &kMockVTable;
  transport.self = &mock;
  assert(driver_initialize(&driver, (HSFConfigRef){0}, transport) == HSF_OK);
  mock.open_failures = 1;
  /* Enable succeeds while offline; the first read reconnects automatically. */
  assert(driver_start(&driver) == HSF_OK);
  assert(driver.status.state == HSF_DRIVER_RUNNING);
  assert(driver.status.connected == 0);

  for (function = 1; function <= 4; ++function) {
    /* Also prove that a connection dropped after startup is reopened. */
    if (function == 2) mock.open = 0;
    address = hsf_device_address_init();
    address.a1 = function;
    address.a2 = 0;
    address.a3 = 1;
    address.encoding = HSF_ENC_U16;
    assert(driver_read(&driver, &address, &value) == HSF_OK);
  }

  address = hsf_device_address_init();
  address.a1 = 5; address.a2 = 0;
  value = hsf_value_bool(1);
  assert(driver_write(&driver, &address, &value) == HSF_OK);

  address.a1 = 6;
  value = hsf_value_u64(123);
  assert(driver_write(&driver, &address, &value) == HSF_OK);

  address.a1 = 15; address.a3 = 9;
  value = hsf_value_blob(coils, sizeof(coils));
  assert(driver_write(&driver, &address, &value) == HSF_OK);

  address.a1 = 16; address.a3 = 2;
  value = hsf_value_blob(registers, sizeof(registers));
  assert(driver_write(&driver, &address, &value) == HSF_OK);

  for (function = 1; function <= 6; ++function)
    if (function != 0) assert(mock.seen[function] == 1);
  assert(mock.seen[15] == 1 && mock.seen[16] == 1);
  assert(driver_stop(&driver) == HSF_OK);
  puts("modbus native C driver tests PASS");
  return 0;
}
