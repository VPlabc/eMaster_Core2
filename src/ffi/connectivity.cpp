#include "gateway/connectivity.h"

#include <map>
#include <algorithm>
#include <mutex>
#include <new>
#include <string>

struct emaster_connectivity {
    struct Entry { int kind; std::string owner; std::string name; int state; };
    mutable std::mutex mutex;
    std::map<uint64_t, Entry> entries;
    uint64_t next_id = 1;
    std::string last_error;
};

namespace {
void set_error(emaster_connectivity* manager, const char* message) { if (manager) manager->last_error = message ? message : ""; }
bool valid_kind(int kind) { return kind >= EMASTER_CONNECTIVITY_REST && kind <= EMASTER_CONNECTIVITY_RABBITMQ; }
int find(emaster_connectivity* manager, uint64_t id, emaster_connectivity::Entry** out) { auto it = manager->entries.find(id); if (it == manager->entries.end()) { set_error(manager, "connection not found"); return EMASTER_CONNECTIVITY_NOT_FOUND; } *out = &it->second; return EMASTER_CONNECTIVITY_OK; }
}

extern "C" uint32_t emaster_connectivity_abi_version(void) { return EMASTER_CONNECTIVITY_ABI_VERSION; }
extern "C" emaster_connectivity_t* emaster_connectivity_create(void) { try { return new emaster_connectivity(); } catch (...) { return nullptr; } }
extern "C" void emaster_connectivity_destroy(emaster_connectivity_t* manager) { delete manager; }
extern "C" int emaster_connectivity_create_connection(emaster_connectivity_t* manager, int kind, const char* owner, const char* name, uint64_t* out_id) {
    if (!manager || !valid_kind(kind) || !owner || !name || !out_id || !*owner || !*name) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT;
    try { std::lock_guard lock(manager->mutex); for (const auto& item : manager->entries) if (item.second.owner == owner && item.second.name == name) return EMASTER_CONNECTIVITY_ALREADY_EXISTS; const uint64_t id = manager->next_id++; manager->entries.emplace(id, emaster_connectivity::Entry{kind, owner, name, EMASTER_CONNECTIVITY_CREATED}); *out_id = id; manager->last_error.clear(); return EMASTER_CONNECTIVITY_OK; } catch (...) { set_error(manager, "allocation failure"); return EMASTER_CONNECTIVITY_NO_MEMORY; }
}
extern "C" int emaster_connectivity_start(emaster_connectivity_t* manager, uint64_t id) { if (!manager) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT; try { std::lock_guard lock(manager->mutex); emaster_connectivity::Entry* entry = nullptr; int result = find(manager, id, &entry); if (result == 0) entry->state = EMASTER_CONNECTIVITY_STARTED; return result; } catch (...) { return EMASTER_CONNECTIVITY_INTERNAL_ERROR; } }
extern "C" int emaster_connectivity_stop(emaster_connectivity_t* manager, uint64_t id) { if (!manager) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT; try { std::lock_guard lock(manager->mutex); emaster_connectivity::Entry* entry = nullptr; int result = find(manager, id, &entry); if (result == 0) entry->state = EMASTER_CONNECTIVITY_STOPPED; return result; } catch (...) { return EMASTER_CONNECTIVITY_INTERNAL_ERROR; } }
extern "C" int emaster_connectivity_restart(emaster_connectivity_t* manager, uint64_t id) { if (!manager) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT; try { std::lock_guard lock(manager->mutex); emaster_connectivity::Entry* entry = nullptr; int result = find(manager, id, &entry); if (result == 0) entry->state = EMASTER_CONNECTIVITY_STARTED; return result; } catch (...) { return EMASTER_CONNECTIVITY_INTERNAL_ERROR; } }
extern "C" int emaster_connectivity_destroy_connection(emaster_connectivity_t* manager, uint64_t id) { if (!manager) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT; try { std::lock_guard lock(manager->mutex); if (manager->entries.erase(id) == 0) return EMASTER_CONNECTIVITY_NOT_FOUND; return EMASTER_CONNECTIVITY_OK; } catch (...) { return EMASTER_CONNECTIVITY_INTERNAL_ERROR; } }
extern "C" size_t emaster_connectivity_destroy_owner(emaster_connectivity_t* manager, const char* owner) { if (!manager || !owner) return 0; try { std::lock_guard lock(manager->mutex); size_t count = 0; for (auto it = manager->entries.begin(); it != manager->entries.end();) { if (it->second.owner == owner) { it = manager->entries.erase(it); ++count; } else ++it; } return count; } catch (...) { return 0; } }
extern "C" int emaster_connectivity_state(const emaster_connectivity_t* manager, uint64_t id) { if (!manager) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT; try { std::lock_guard lock(manager->mutex); auto it = manager->entries.find(id); return it == manager->entries.end() ? EMASTER_CONNECTIVITY_NOT_FOUND : it->second.state; } catch (...) { return EMASTER_CONNECTIVITY_INTERNAL_ERROR; } }
extern "C" int emaster_connectivity_last_error(const emaster_connectivity_t* manager, char* output, size_t capacity, size_t* required) { if (!manager || !required) return EMASTER_CONNECTIVITY_INVALID_ARGUMENT; try { std::lock_guard lock(manager->mutex); *required = manager->last_error.size(); if (output && capacity) { const size_t count = manager->last_error.size() < capacity - 1 ? manager->last_error.size() : capacity - 1; std::copy_n(manager->last_error.data(), count, output); output[count] = '\0'; } return output || capacity == 0 ? EMASTER_CONNECTIVITY_OK : EMASTER_CONNECTIVITY_INVALID_ARGUMENT; } catch (...) { return EMASTER_CONNECTIVITY_INTERNAL_ERROR; } }
