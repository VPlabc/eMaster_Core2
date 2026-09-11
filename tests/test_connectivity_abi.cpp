#include "gateway/connectivity.h"

#include <cassert>
#include <cstring>

int main() {
    assert(emaster_connectivity_abi_version() == EMASTER_CONNECTIVITY_ABI_VERSION);
    emaster_connectivity_t* manager = emaster_connectivity_create();
    assert(manager != nullptr);
    uint64_t rest = 0;
    assert(emaster_connectivity_create_connection(manager, EMASTER_CONNECTIVITY_REST, "app-a", "main", &rest) == EMASTER_CONNECTIVITY_OK);
    assert(emaster_connectivity_state(manager, rest) == EMASTER_CONNECTIVITY_CREATED);
    assert(emaster_connectivity_start(manager, rest) == EMASTER_CONNECTIVITY_OK);
    assert(emaster_connectivity_state(manager, rest) == EMASTER_CONNECTIVITY_STARTED);
    uint64_t duplicate = 0;
    assert(emaster_connectivity_create_connection(manager, EMASTER_CONNECTIVITY_MQTT, "app-a", "main", &duplicate) == EMASTER_CONNECTIVITY_ALREADY_EXISTS);
    assert(emaster_connectivity_stop(manager, rest) == EMASTER_CONNECTIVITY_OK);
    assert(emaster_connectivity_state(manager, rest) == EMASTER_CONNECTIVITY_STOPPED);
    assert(emaster_connectivity_restart(manager, rest) == EMASTER_CONNECTIVITY_OK);
    assert(emaster_connectivity_state(manager, rest) == EMASTER_CONNECTIVITY_STARTED);
    assert(emaster_connectivity_destroy_owner(manager, "app-a") == 1);
    assert(emaster_connectivity_state(manager, rest) == EMASTER_CONNECTIVITY_NOT_FOUND);
    char error[64] = {};
    size_t required = 0;
    assert(emaster_connectivity_last_error(manager, error, sizeof(error), &required) == EMASTER_CONNECTIVITY_OK);
    assert(std::strcmp(error, "") == 0);
    emaster_connectivity_destroy(manager);
    return 0;
}
