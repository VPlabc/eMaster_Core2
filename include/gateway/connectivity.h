#ifndef EMASTER_GATEWAY_CONNECTIVITY_H
#define EMASTER_GATEWAY_CONNECTIVITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMASTER_CONNECTIVITY_ABI_VERSION 1u

typedef struct emaster_connectivity emaster_connectivity_t;

enum emaster_connectivity_kind { EMASTER_CONNECTIVITY_REST = 1, EMASTER_CONNECTIVITY_MQTT = 2, EMASTER_CONNECTIVITY_RABBITMQ = 3 };
enum emaster_connectivity_state { EMASTER_CONNECTIVITY_CREATED = 1, EMASTER_CONNECTIVITY_STARTED = 2, EMASTER_CONNECTIVITY_STOPPED = 3 };
enum emaster_connectivity_error { EMASTER_CONNECTIVITY_OK = 0, EMASTER_CONNECTIVITY_INVALID_ARGUMENT = -1, EMASTER_CONNECTIVITY_NO_MEMORY = -2, EMASTER_CONNECTIVITY_NOT_FOUND = -3, EMASTER_CONNECTIVITY_ALREADY_EXISTS = -4, EMASTER_CONNECTIVITY_INTERNAL_ERROR = -5 };

/* All pointers are borrowed for the duration of the call. Handles are owned
 * by the caller and released with destroy/destroy_owner. Functions are
 * thread-safe, never throw across this ABI, and return a stable error code. */
uint32_t emaster_connectivity_abi_version(void);
emaster_connectivity_t* emaster_connectivity_create(void);
void emaster_connectivity_destroy(emaster_connectivity_t* manager);
int emaster_connectivity_create_connection(emaster_connectivity_t* manager, int kind, const char* owner, const char* name, uint64_t* out_id);
int emaster_connectivity_start(emaster_connectivity_t* manager, uint64_t id);
int emaster_connectivity_stop(emaster_connectivity_t* manager, uint64_t id);
int emaster_connectivity_restart(emaster_connectivity_t* manager, uint64_t id);
int emaster_connectivity_destroy_connection(emaster_connectivity_t* manager, uint64_t id);
size_t emaster_connectivity_destroy_owner(emaster_connectivity_t* manager, const char* owner);
int emaster_connectivity_state(const emaster_connectivity_t* manager, uint64_t id);
int emaster_connectivity_last_error(const emaster_connectivity_t* manager, char* output, size_t capacity, size_t* required);

#ifdef __cplusplus
}
#endif

#endif
