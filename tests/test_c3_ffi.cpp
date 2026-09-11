#include "gateway/c3.h"

#include "hsf/zk_controller/C3Codec.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  const uint8_t password[] = {'s', 'e', 'c', 'r', 'e', 't'};
  uint8_t* frame = nullptr;
  size_t frame_length = 0;
  assert(emaster_c3_encode_connect_session_less(password, sizeof(password), &frame,
                                                &frame_length) == EMASTER_C3_OK);
  assert(frame != nullptr && frame_length > 0);
  assert(emaster_c3_frame_size(frame, frame_length) == frame_length);
  assert(emaster_c3_crc16(frame + 1, frame_length - 4) ==
         hsf::c3::Crc16(frame + 1, frame_length - 4));
  emaster_c3_free(frame);

  assert(emaster_c3_frame_size(nullptr, 0) == 0);
  assert(emaster_c3_frame_size(nullptr, 1) == 0);
  assert(emaster_c3_encode_connect_session_less(nullptr, 1, &frame, &frame_length) ==
         EMASTER_C3_INVALID_ARGUMENT);
  assert(emaster_c3_decode_generic_reply(nullptr, 1) == EMASTER_C3_INVALID_ARGUMENT);
  assert(emaster_c3_decode_generic_reply(nullptr, 0) == EMASTER_C3_INCOMPLETE);

  emaster_c3_client_t* client = emaster_c3_client_create();
  assert(client != nullptr);
  assert(emaster_c3_client_is_connected(client) == 0);
  assert(emaster_c3_client_control(client, 1, 1, 1, 1, 0) == EMASTER_C3_INTERNAL_ERROR);
  char error[128] = {};
  size_t required = 0;
  assert(emaster_c3_client_last_error(client, error, sizeof(error), &required) == EMASTER_C3_OK);
  assert(required > 0);
  assert(emaster_c3_client_last_error(client, nullptr, 0, &required) ==
         EMASTER_C3_BUFFER_TOO_SMALL);
  emaster_c3_client_disconnect(client);
  emaster_c3_client_destroy(client);
  std::cout << "C3 C ABI tests passed\n";
  return 0;
}
