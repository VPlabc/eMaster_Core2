#include "gateway/c3.h"

#include "hsf/zk_controller/C3Client.h"
#include "hsf/zk_controller/C3Codec.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {

int MapStatus(hsf::c3::ResponseStatus status) {
  switch (status) {
    case hsf::c3::ResponseStatus::kOk: return EMASTER_C3_OK;
    case hsf::c3::ResponseStatus::kIncomplete: return EMASTER_C3_INCOMPLETE;
    case hsf::c3::ResponseStatus::kMalformed: return EMASTER_C3_MALFORMED;
    case hsf::c3::ResponseStatus::kRejected: return EMASTER_C3_REJECTED;
  }
  return EMASTER_C3_INTERNAL_ERROR;
}

hsf::c3::Bytes CopyBytes(const uint8_t* bytes, size_t length) {
  if (length == 0) return {};
  return hsf::c3::Bytes(bytes, bytes + length);
}

}  // namespace

struct emaster_c3_client {
  hsf::C3Client impl;
};

extern "C" uint16_t emaster_c3_crc16(const uint8_t* bytes, size_t length) {
  if (bytes == nullptr && length != 0) return 0;
  return hsf::c3::Crc16(bytes, length);
}

extern "C" size_t emaster_c3_frame_size(const uint8_t* bytes, size_t length) {
  if (bytes == nullptr && length != 0) return 0;
  return hsf::c3::FrameSize(CopyBytes(bytes, length));
}

extern "C" int emaster_c3_encode_connect_session_less(const uint8_t* password,
                                                        size_t password_length,
                                                        uint8_t** out_bytes,
                                                        size_t* out_length) {
  if (out_bytes == nullptr || out_length == nullptr ||
      (password == nullptr && password_length != 0)) {
    return EMASTER_C3_INVALID_ARGUMENT;
  }
  *out_bytes = nullptr;
  *out_length = 0;
  try {
    const std::string text(reinterpret_cast<const char*>(password), password_length);
    const hsf::c3::Bytes frame = hsf::c3::EncodeConnectSessionLess(text);
    auto* result = static_cast<uint8_t*>(std::malloc(frame.size()));
    if (result == nullptr && !frame.empty()) return EMASTER_C3_NO_MEMORY;
    if (!frame.empty()) std::memcpy(result, frame.data(), frame.size());
    *out_bytes = result;
    *out_length = frame.size();
    return EMASTER_C3_OK;
  } catch (const std::bad_alloc&) {
    return EMASTER_C3_NO_MEMORY;
  } catch (...) {
    return EMASTER_C3_INTERNAL_ERROR;
  }
}

extern "C" int emaster_c3_decode_generic_reply(const uint8_t* bytes, size_t length) {
  if (bytes == nullptr && length != 0) return EMASTER_C3_INVALID_ARGUMENT;
  try {
    const hsf::c3::GenericReply reply =
        hsf::c3::DecodeGenericReply(CopyBytes(bytes, length));
    return MapStatus(reply.status);
  } catch (const std::bad_alloc&) {
    return EMASTER_C3_NO_MEMORY;
  } catch (...) {
    return EMASTER_C3_INTERNAL_ERROR;
  }
}

extern "C" emaster_c3_client_t* emaster_c3_client_create(void) {
  try {
    return new emaster_c3_client();
  } catch (...) {
    return nullptr;
  }
}

extern "C" void emaster_c3_client_destroy(emaster_c3_client_t* client) { delete client; }

extern "C" int emaster_c3_client_connect(emaster_c3_client_t* client, const char* connection,
                                           size_t connection_length) {
  if (client == nullptr || (connection == nullptr && connection_length != 0)) {
    return EMASTER_C3_INVALID_ARGUMENT;
  }
  try {
    return client->impl.Connect(std::string(connection ? connection : "", connection_length))
               ? EMASTER_C3_OK
               : EMASTER_C3_INTERNAL_ERROR;
  } catch (const std::bad_alloc&) {
    return EMASTER_C3_NO_MEMORY;
  } catch (...) {
    return EMASTER_C3_INTERNAL_ERROR;
  }
}

extern "C" void emaster_c3_client_disconnect(emaster_c3_client_t* client) {
  if (client != nullptr) client->impl.Disconnect();
}

extern "C" int emaster_c3_client_is_connected(const emaster_c3_client_t* client) {
  return client != nullptr && client->impl.IsConnected() ? 1 : 0;
}

extern "C" int emaster_c3_client_control(emaster_c3_client_t* client, int operation, int param1,
                                           int param2, int param3, int param4) {
  if (client == nullptr) return EMASTER_C3_INVALID_ARGUMENT;
  try {
    return client->impl.ControlDevice(operation, param1, param2, param3, param4) == 0
               ? EMASTER_C3_OK
               : EMASTER_C3_INTERNAL_ERROR;
  } catch (const std::bad_alloc&) {
    return EMASTER_C3_NO_MEMORY;
  } catch (...) {
    return EMASTER_C3_INTERNAL_ERROR;
  }
}

int CopyString(const std::string& value, char* output, size_t capacity, size_t* required) {
  if (required != nullptr) *required = value.size();
  if (output == nullptr && capacity != 0) return EMASTER_C3_INVALID_ARGUMENT;
  if (capacity == 0) return value.empty() ? EMASTER_C3_OK : EMASTER_C3_BUFFER_TOO_SMALL;
  const size_t copied = value.size() < capacity - 1 ? value.size() : capacity - 1;
  if (copied != 0) std::memcpy(output, value.data(), copied);
  output[copied] = '\0';
  return copied == value.size() ? EMASTER_C3_OK : EMASTER_C3_BUFFER_TOO_SMALL;
}

extern "C" int emaster_c3_client_get_device_param(emaster_c3_client_t* client, const char* items,
                                                    size_t items_length, char* output,
                                                    size_t output_capacity, size_t* required) {
  if (client == nullptr || (items == nullptr && items_length != 0)) {
    return EMASTER_C3_INVALID_ARGUMENT;
  }
  try {
    const std::string result = client->impl.GetDeviceParam(
        std::string(items ? items : "", items_length), 0, nullptr);
    return CopyString(result, output, output_capacity, required);
  } catch (const std::bad_alloc&) {
    return EMASTER_C3_NO_MEMORY;
  } catch (...) {
    return EMASTER_C3_INTERNAL_ERROR;
  }
}

extern "C" int emaster_c3_client_last_error(const emaster_c3_client_t* client, char* output,
                                              size_t output_capacity, size_t* required) {
  if (client == nullptr) return EMASTER_C3_INVALID_ARGUMENT;
  try {
    return CopyString(client->impl.LastError(), output, output_capacity, required);
  } catch (const std::bad_alloc&) {
    return EMASTER_C3_NO_MEMORY;
  } catch (...) {
    return EMASTER_C3_INTERNAL_ERROR;
  }
}

extern "C" void emaster_c3_free(void* bytes) { std::free(bytes); }
