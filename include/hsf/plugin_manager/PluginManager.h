#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/plugin_manager/PluginHostServices.h"
#include "hsf/plugin_manager/PluginLibrary.h"
#include "hsf/plugin_manager/PluginManifest.h"
#include "hsf/plugin_manager/PluginTransports.h"

namespace hsf {

// The plugin lifecycle (plan §17).
//
//   DISCOVERED -> VALIDATED -> INSTALLED -> DISABLED <-> ENABLED -> RUNNING
//                     |            |                                   |
//                     +------------+-------------- FAILED -------------+
//
// DISABLED and ENABLED are configuration; RUNNING is fact. The distinction
// matters: an operator enables a plugin whose device is unplugged, and the UI
// has to show "enabled, not running, here is why" rather than flipping the
// switch back off.
enum class PluginState {
  kDiscovered = 0,
  kValidated,
  kInstalled,
  kDisabled,
  kEnabled,
  kRunning,
  kFailed
};

const char* PluginStateName(PluginState s);

struct PluginRecord {
  PluginManifest manifest;
  PluginState    state = PluginState::kDiscovered;
  std::string    last_error;      // empty unless something went wrong
  bool           enabled = false; // the operator's intent, persisted
  bool           has_driver = false;
  HSFDriverInfo  driver_info{};

  nlohmann::json ToJson() const;
};

// Owns every discovered plugin, its library handle, its instance and its host
// services.
//
// THREADING. The mutex guards the record map only. Loading, creating,
// starting and stopping a plugin all happen with the lock RELEASED, because
// every one of them calls into plugin code of unknown duration -- a driver
// whose start() blocks for a connection timeout must not freeze a status query
// from the web thread. That is the same rule LuaRuntimeManager already follows
// for the same reason.
//
// DESTRUCTION ORDER is load-bearing and is why the members are declared in the
// order they are: an Entry destroys its plugin instance, then its host
// services, then closes its library. Closing the library first would unmap the
// code that the destructor is about to call.
class PluginManager {
 public:
  PluginManager();
  ~PluginManager();

  PluginManager(const PluginManager&) = delete;
  PluginManager& operator=(const PluginManager&) = delete;

  // Where to look: <root>/<name>/manifest.json, one directory per plugin
  // (plan §11). Non-recursive, so an unpacked archive left inside a plugin
  // directory cannot be mistaken for a second plugin.
  void SetPluginRoot(const std::string& root);
  const std::string& PluginRoot() const { return root_; }

  // Where a plugin's own writable directory lives (its HSFPluginContext
  // data_dir). One subdirectory per plugin id, created on demand.
  void SetDataRoot(const std::string& root);

  // Configuration for all plugins, as { "<plugin id>": { … } }. Each plugin
  // sees only its own object.
  void SetConfiguration(nlohmann::json all);

  PluginEventBus& Events() { return events_; }

  // Scans the root. Existing records are updated in place, so a rescan does not
  // disturb a running plugin. Returns the number of plugins now known.
  size_t Discover();

  // Load and instantiate, without starting. Idempotent.
  bool Load(const std::string& id, std::string& error);

  // Stop, destroy, close. Idempotent, and safe on a plugin that never loaded.
  bool Unload(const std::string& id, std::string& error);

  // The operator's switch. Enable loads and starts; Disable stops but keeps the
  // library open, so re-enabling is fast and cannot fail on a file that has
  // since been replaced.
  bool Enable(const std::string& id, std::string& error);
  bool Disable(const std::string& id, std::string& error);

  // Install or update from an unpacked plugin package directory. The package
  // must contain manifest.json and the entry named by that manifest. Existing
  // installations are moved to the rollback slot before replacement.
  bool Install(const std::string& source_dir, std::string& installed_id, std::string& error);
  bool ImportUploaded(const std::string& manifest_json, const std::vector<unsigned char>& binary,
                      std::string& installed_id, std::string& error);
  bool Rollback(const std::string& id, std::string& error);
  bool Remove(const std::string& id, std::string& error);

  // Active reachability probe, forwarded to the driver. Distinct from state.
  bool HealthCheck(const std::string& id, std::string& error);

  std::vector<PluginRecord> List() const;
  bool                      Get(const std::string& id, PluginRecord* out) const;
  nlohmann::json            ToJson() const;

  // A loaded plugin's driver, for the Test Tool and the future device layer.
  // Returns an invalid ref when the plugin is not loaded or has no driver;
  // check with hsf_transport_valid's counterpart, driver.vt != nullptr.
  HSFDriverRef Driver(const std::string& id) const;

  // Drains the event queue. Called from the host's loop.
  size_t DispatchEvents(size_t max_events = 64);

 private:
  // Everything one plugin owns. Declaration order IS destruction order,
  // reversed -- see the class comment.
  struct Entry {
    PluginManifest manifest;
    PluginState    state = PluginState::kDiscovered;
    std::string    last_error;
    bool           enabled = false;

    // Declared before `library` so they are destroyed after it? No -- members
    // are destroyed in REVERSE declaration order, so `library` last means it is
    // declared FIRST. Do not reorder without reading Release().
    std::unique_ptr<PluginLibrary> library;

    std::unique_ptr<PluginLogger> logger;
    std::unique_ptr<PluginConfig> config;

    HSFPlugin*             instance = nullptr;
    const HSFPluginVTable* vtable = nullptr;
    HSFDriverRef           driver{};
    HSFTransportRef        transport{};
    HSFDriverInfo          driver_info{};
    bool                   has_driver = false;
  };

  // Stops and destroys the instance, then closes the library, in that order.
  // The one function that must never be reordered.
  // Not static: it also returns the entry's transport to the factory that owns
  // it, so it needs `this`.
  void Release(Entry& e);

  Entry* Find(const std::string& id);
  const Entry* Find(const std::string& id) const;
  static PluginRecord ToRecord(const Entry& e);

  mutable std::mutex mutex_;
  // map, not unordered_map: List() and ToJson() are read by the UI, and a
  // stable alphabetical order means the plugin list does not reshuffle itself
  // between refreshes.
  std::map<std::string, std::unique_ptr<Entry>> plugins_;

  std::string    root_;
  std::string    data_root_;
  nlohmann::json config_ = nlohmann::json::object();
  PluginEventBus events_;
  // Creates the transports plugins are handed, and owns them: a plugin gets a
  // borrowed ref and must never free it, since the two sides do not share an
  // allocator. Declared last so it outlives nothing that matters -- but every
  // Entry is released in the destructor before this is touched.
  TransportFactory transports_;
};

}  // namespace hsf
