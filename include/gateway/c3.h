#ifndef EMASTER_GATEWAY_C3_H
#define EMASTER_GATEWAY_C3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable C ABI for the stateless C3 frame codec.
 *
 * Ownership: *_encode functions allocate `*out_bytes` with the C allocator;
 * release it with emaster_c3_free. Input buffers are borrowed for the call.
 * Thread safety: all functions are reentrant and may be called concurrently.
 * No function throws an exception across this boundary.
 */
enum emaster_c3_status {
    EMASTER_C3_OK = 0,
    EMASTER_C3_INCOMPLETE = 1,
    EMASTER_C3_MALFORMED = 2,
    EMASTER_C3_REJECTED = 3,
    EMASTER_C3_INVALID_ARGUMENT = -1,
    EMASTER_C3_NO_MEMORY = -2,
    EMASTER_C3_INTERNAL_ERROR = -3,
    EMASTER_C3_BUFFER_TOO_SMALL = -4
};

typedef struct emaster_c3_client emaster_c3_client_t;

uint16_t emaster_c3_crc16(const uint8_t* bytes, size_t length);

/* Returns the complete frame size, or zero when fewer than five bytes are
 * available to read the frame length. It does not validate CRC or markers. */
size_t emaster_c3_frame_size(const uint8_t* bytes, size_t length);

int emaster_c3_encode_connect_session_less(const uint8_t* password, size_t password_length,
                                           uint8_t** out_bytes, size_t* out_length);

/* Decodes a reply envelope without exposing C++ payload containers. */
int emaster_c3_decode_generic_reply(const uint8_t* bytes, size_t length);

emaster_c3_client_t* emaster_c3_client_create(void);
void emaster_c3_client_destroy(emaster_c3_client_t* client);
int emaster_c3_client_connect(emaster_c3_client_t* client, const char* connection,
                               size_t connection_length);
void emaster_c3_client_disconnect(emaster_c3_client_t* client);
int emaster_c3_client_is_connected(const emaster_c3_client_t* client);
int emaster_c3_client_control(emaster_c3_client_t* client, int operation, int param1, int param2,
                              int param3, int param4);

/* The output is a borrowed view copied into the caller's buffer. `required`
 * receives the byte count excluding the NUL terminator. */
int emaster_c3_client_get_device_param(emaster_c3_client_t* client, const char* items,
                                       size_t items_length, char* output, size_t output_capacity,
                                       size_t* required);
int emaster_c3_client_last_error(const emaster_c3_client_t* client, char* output,
                                 size_t output_capacity, size_t* required);

void emaster_c3_free(void* bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EMASTER_GATEWAY_C3_H */
