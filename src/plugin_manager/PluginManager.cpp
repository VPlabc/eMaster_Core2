#include "hsf/plugin_manager/PluginManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "hsf/Logger.h"

namespace hsf {
namespace {

std::string Str(HSFStr s) {
  return (s.ptr && s.len) ? std::string(s.ptr, s.len) : std::string();
}

void LogInfo(const std::string& m) { Logger::Instance().Info(LogCategory::Lua, m); }
void LogWarn(const std::string& m) { Logger::Instance().Warning(LogCategory::Lua, m); }
void LogErr(const std::string& m) { Logger::Instance().Error(LogCategory::Lua, m); }

}  // namespace

const char* PluginStateName(PluginState s) {
  switch (s) {
    case PluginState::kDiscovered: return "DISCOVERED";
    case PluginState::kValidated:  return "VALIDATED";
    case PluginState::kInstalled:  return "INSTALLED";
    case PluginState::kDisabled:   return "DISABLED";
    case PluginState::kEnabled:    return "ENABLED";
    case PluginState::kRunning:    return "RUNNING";
    case PluginState::kFailed:     return "FAILED";
  }
  return "UNKNOWN";
}

nlohmann::json PluginRecord::ToJson() const {
  nlohmann::json j;
  j["id"] = manifest.id;
  j["name"] = manifest.name;
  j["version"] = manifest.version;
  j["api_version"] = manifest.api_version;
  j["description"] = manifest.description;
  j["author"] = manifest.author;
  j["execution"] = manifest.execution;
  j["directory"] = manifest.directory;
  j["platforms"] = manifest.platforms;
  j["transports"] = manifest.transports;
  j["permissions"] = manifest.permissions;
  j["state"] = PluginStateName(state);
  j["enabled"] = enabled;
  j["error"] = last_error;
  j["has_driver"] = has_driver;
  if (has_driver) {
    j["driver"] = {{"owns_thread", driver_info.owns_thread != 0},
                   {"tick_ms", driver_info.tick_ms},
                   {"max_frame_bytes", driver_info.max_frame_bytes}};
  }
  return j;
}

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
  // Not merely letting the map destruct: every plugin must be stopped and
  // destroyed while its library is still mapped, and Release() is the only
  // place that ordering is written down.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& kv : plugins_) {
    if (kv.second) Release(*kv.second);
  }
  plugins_.clear();
}

void PluginManager::SetPluginRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mutex_);
  root_ = root;
}

void PluginManager::SetDataRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mutex_);
  data_root_ = root;
}

void PluginManager::SetConfiguration(nlohmann::json all) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = all.is_object() ? std::move(all) : nlohmann::json::object();
}

PluginManager::Entry* PluginManager::Find(const std::string& id) {
  auto it = plugins_.find(id);
  return it == plugins_.end() ? nullptr : it->second.get();
}

const PluginManager::Entry* PluginManager::Find(const std::string& id) const {
  auto it = plugins_.find(id);
  return it == plugins_.end() ? nullptr : it->second.get();
}

PluginRecord PluginManager::ToRecord(const Entry& e) {
  PluginRecord r;
  r.manifest = e.manifest;
  r.state = e.state;
  r.last_error = e.last_error;
  r.enabled = e.enabled;
  r.has_driver = e.has_driver;
  r.driver_info = e.driver_info;
  return r;
}

// The ordering that matters. Stop the driver, destroy the instance, THEN close
// the library. Any other order runs code out of unmapped memory.
void PluginManager::Release(Entry& e) {
  if (e.driver.vt && e.driver.self && e.driver.vt->stop) {
    try {
      e.driver.vt->stop(e.driver.self);
    } catch (...) {
      // A throwing stop() is a plugin bug; it must not stop us unloading.
      LogErr("Plugin " + e.manifest.id + " threw from stop() during unload");
    }
  }
  e.driver = HSFDriverRef{};
  e.has_driver = false;

  if (e.instance && e.library) {
    try {
      e.library->Destroy(e.instance);
    } catch (...) {
      LogErr("Plugin " + e.manifest.id + " threw from destroy()");
    }
  }
  e.instance = nullptr;
  e.vtable = nullptr;

  // The transport goes back to the factory that owns it, AFTER the driver has
  // stopped and the instance is gone -- a driver's destructor may legitimately
  // write a last byte, and freeing the port under it would be a use-after-free
  // in the plugin rather than in us.
  if (e.transport.self) {
    transports_.Destroy(e.transport);
    e.transport = HSFTransportRef{};
  }

  // Host services outlive the instance because the instance may touch them on
  // the way down (a final log line is normal and legitimate).
  e.config.reset();
  e.logger.reset();

  if (e.library) e.library->Close();
  e.library.reset();
}

size_t PluginManager::Discover() {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    root = root_;
  }
  if (root.empty()) return 0;

  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    // Not an error worth shouting about: a gateway with no plugins installed is
    // the normal case today.
    return 0;
  }

  std::vector<ManifestResult> found;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_directory()) continue;
    const std::filesystem::path manifest = entry.path() / "manifest.json";
    if (!std::filesystem::is_regular_file(manifest, ec)) continue;
    found.push_back(LoadManifest(manifest.string(), HSF_PLUGIN_API_VERSION));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (ManifestResult& r : found) {
    if (!r.ok) {
      // A rejected manifest is recorded, not skipped silently. An operator who
      // dropped a plugin in and saw nothing appear needs to find out why from
      // the UI, not from a log they did not know to read.
      const std::string id = r.manifest.id.empty() ? r.manifest.manifest_path : r.manifest.id;
      auto& slot = plugins_[id];
      if (!slot) slot = std::make_unique<Entry>();
      if (slot->state == PluginState::kRunning) continue;  // leave a running one alone
      slot->manifest = r.manifest;
      if (slot->manifest.id.empty()) slot->manifest.id = id;
      slot->state = PluginState::kFailed;
      slot->last_error = std::string(ManifestErrorName(r.error)) + ": " + r.message;
      LogWarn("Plugin at " + id + " rejected -- " + r.message);
      continue;
    }

    auto& slot = plugins_[r.manifest.id];
    if (!slot) {
      slot = std::make_unique<Entry>();
      LogInfo("Discovered plugin " + r.manifest.id + " " + r.manifest.version + " (" +
              r.manifest.directory + ")");
    }
    // A rescan must not disturb a plugin that is already up. Its manifest on
    // disk may even have changed; that takes effect on the next load, not
    // underneath a running driver.
    if (slot->state == PluginState::kRunning || slot->state == PluginState::kEnabled) continue;
    slot->manifest = r.manifest;
    slot->state = PluginState::kValidated;
    slot->last_error.clear();
  }
  return plugins_.size();
}

bool PluginManager::Load(const std::string& id, std::string& error) {
  // Copy what the load needs, then work with the lock released: dlopen and the
  // plugin's own create() are both of unknown duration.
  PluginManifest manifest;
  std::string data_root;
  nlohmann::json section = nlohmann::json::object();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(id);
    if (!e) {
      error = "no such plugin: " + id;
      return false;
    }
    if (e->instance) return true;  // already loaded
    if (e->state == PluginState::kFailed && e->library == nullptr && e->manifest.entry.empty()) {
      error = e->last_error.empty() ? "plugin failed validation" : e->last_error;
      return false;
    }
    manifest = e->manifest;
    data_root = data_root_;
    if (config_.contains(id) && config_[id].is_object()) section = config_[id];
  }

  auto library = std::make_unique<PluginLibrary>();
  const std::string entry_path =
      (std::filesystem::path(manifest.directory) / manifest.entry).string();
  std::string open_error;
  if (!library->Open(entry_path, open_error)) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) {
      e->state = PluginState::kFailed;
      e->last_error = open_error;
    }
    error = open_error;
    LogErr("Plugin " + id + ": " + open_error);
    return false;
  }

  // The binary's own answer, not the manifest's claim. The manifest is a text
  // file anyone can edit; when the two disagree the compiled version wins,
  // because that is what determines the memory layout.
  const uint32_t plugin_abi = library->AbiVersion();
  if (!HSF_API_VERSION_COMPATIBLE(HSF_PLUGIN_API_VERSION, plugin_abi)) {
    error = "plugin " + id + " was built against Plugin API " +
            std::to_string(HSF_API_VERSION_MAJOR_OF(plugin_abi)) + "." +
            std::to_string(HSF_API_VERSION_MINOR_OF(plugin_abi)) + ", this gateway provides " +
            std::to_string(HSF_API_VERSION_MAJOR_OF(HSF_PLUGIN_API_VERSION)) + "." +
            std::to_string(HSF_API_VERSION_MINOR_OF(HSF_PLUGIN_API_VERSION));
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) {
      e->state = PluginState::kFailed;
      e->last_error = error;
    }
    LogErr(error);
    return false;
  }

  // And the id must match too. A binary reporting a different id than its
  // manifest means the pair has been mixed up, and loading it would attach one
  // plugin's configuration to another's code.
  const HSFPluginInfo* info = library->Info();
  if (!info) {
    error = "plugin " + id + " returned no HSFPluginInfo";
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) { e->state = PluginState::kFailed; e->last_error = error; }
    return false;
  }
  const std::string binary_id = Str(info->id);
  if (binary_id != manifest.id) {
    error = "manifest declares id \"" + manifest.id + "\" but the binary reports \"" + binary_id +
            "\"; the manifest and plugin.so do not belong together";
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) { e->state = PluginState::kFailed; e->last_error = error; }
    LogErr(error);
    return false;
  }

  // Cross-check the declared permissions against what the code asks for. The
  // manifest is the operator-visible contract; a binary wanting more than its
  // manifest declares is refused, because the manifest is what an operator read
  // before approving the install (plan §14).
  const uint32_t declared = manifest.PermissionBits();
  if ((info->permissions & ~declared) != 0) {
    error = "plugin " + id + " requests permissions its manifest does not declare";
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) { e->state = PluginState::kFailed; e->last_error = error; }
    LogErr(error);
    return false;
  }

  auto logger = std::make_unique<PluginLogger>(manifest.id, std::string());
  auto config = std::make_unique<PluginConfig>(manifest.id, section);

  std::string data_dir;
  if (!data_root.empty()) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(data_root) / manifest.id;
    std::filesystem::create_directories(dir, ec);
    if (!ec) data_dir = dir.string();
  }

  HSFPluginContext ctx{};
  ctx.struct_size = static_cast<uint32_t>(sizeof(HSFPluginContext));
  ctx.host_api_version = HSF_PLUGIN_API_VERSION;
  ctx.log = logger->Ref();
  ctx.config = config->Ref();
  ctx.events = events_.RefFor(manifest.id);
  ctx.transports = transports_.Ref();
  ctx.data_dir = hsf_cstr(data_dir.c_str());
  ctx.instance_id = hsf_cstr("");

  HSFPlugin* instance = nullptr;
  const HSFPluginVTable* vtable = nullptr;
  HSFStatus st = HSF_ERR_UNKNOWN;
  try {
    st = library->Create(&ctx, &instance, &vtable);
  } catch (...) {
    // A create() that throws across the ABI is already undefined behaviour;
    // catching here at least keeps the gateway alive to report it.
    st = HSF_ERR_INTERNAL;
  }
  if (st < 0 || !instance || !vtable) {
    error = "plugin " + id + " failed to create: " + hsf_status_name(st);
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) { e->state = PluginState::kFailed; e->last_error = error; }
    LogErr(error);
    return false;
  }

  HSFDriverRef driver{};
  HSFDriverInfo driver_info{};
  bool has_driver = false;
  if (info->kind == HSF_PLUGIN_KIND_DRIVER && vtable->get_driver) {
    if (vtable->get_driver(instance, &driver, &driver_info) == HSF_OK && driver.vt && driver.self) {
      has_driver = true;
    }
  }

  // The driver's transport, built from the "transport" block of its own
  // configuration. The DRIVER never sees the device path or the baud rate --
  // that separation is the whole point of plan §8, and it is what lets the same
  // Modbus driver run over USB serial on one machine and the main UART on
  // another with no code change.
  HSFTransportRef transport{};
  if (has_driver && section.contains("transport") && section["transport"].is_object()) {
    std::string transport_error;
    transport = transports_.Create(section["transport"], transport_error);
    if (!transport.vt) {
      // Not fatal to the load. The plugin is installed and its problem is
      // reported; an operator fixing the address and re-enabling should not have
      // to reinstall it.
      LogWarn("Plugin " + id + ": " + transport_error);
      std::lock_guard<std::mutex> lock(mutex_);
      if (Entry* e = Find(id)) e->last_error = transport_error;
    } else {
      // Refuse a combination the driver said it cannot speak, here rather than
      // at the first read, and name both halves.
      const HSFTransportKind kind = transport.vt->kind(transport.self);
      bool supported = driver_info.supported_transport_count == 0;
      for (size_t i = 0; i < driver_info.supported_transport_count; ++i) {
        if (driver_info.supported_transports[i] == kind) { supported = true; break; }
      }
      if (!supported) {
        const std::string why = std::string("plugin ") + id + " does not support a " +
                                hsf_transport_kind_name(kind) + " transport";
        LogWarn(why);
        transports_.Destroy(transport);
        transport = HSFTransportRef{};
        std::lock_guard<std::mutex> lock(mutex_);
        if (Entry* e = Find(id)) e->last_error = why;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(id);
    if (!e) {
      // Removed while we were loading. Undo rather than leak.
      library->Destroy(instance);
      error = "plugin " + id + " disappeared during load";
      return false;
    }
    e->library = std::move(library);
    e->logger = std::move(logger);
    e->config = std::move(config);
    e->instance = instance;
    e->vtable = vtable;
    e->driver = driver;
    e->transport = transport;
    e->driver_info = driver_info;
    e->has_driver = has_driver;
    e->state = PluginState::kInstalled;
    e->last_error.clear();
  }
  LogInfo("Loaded plugin " + id + " " + manifest.version +
          (has_driver ? " (driver)" : " (no driver)"));
  return true;
}

bool PluginManager::Unload(const std::string& id, std::string& error) {
  std::unique_ptr<Entry> detached;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(id);
    if (!e) {
      error = "no such plugin: " + id;
      return false;
    }
    if (!e->instance && !e->library) {
      e->state = PluginState::kValidated;
      return true;
    }
    // Move the whole entry out and release it OUTSIDE the lock: Release() calls
    // the plugin's stop() and destroy(), either of which may block, and holding
    // the lock through them would stall every status query.
    detached = std::make_unique<Entry>();
    std::swap(*detached, *e);
    e->manifest = detached->manifest;
    e->enabled = detached->enabled;
    e->state = PluginState::kValidated;
    e->last_error.clear();
  }
  Release(*detached);
  LogInfo("Unloaded plugin " + id);
  return true;
}

bool PluginManager::Enable(const std::string& id, std::string& error) {
  if (!Load(id, error)) return false;

  HSFDriverRef driver{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(id);
    if (!e) {
      error = "no such plugin: " + id;
      return false;
    }
    e->enabled = true;
    if (e->state == PluginState::kRunning) return true;
    e->state = PluginState::kEnabled;
    driver = e->driver;
  }

  // A plugin with no driver is enabled and that is all there is to do -- a
  // service plugin has nothing to start.
  if (!driver.vt || !driver.self) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) e->state = PluginState::kRunning;
    return true;
  }

  HSFStatus st = HSF_ERR_UNKNOWN;
  try {
    // initialize() then start(), the order the ABI documents. initialize is
    // called every enable, not once per load: the operator may have changed the
    // settings between disabling and re-enabling, and that is the whole point
    // of the switch.
    HSFConfigRef cfg{};
    HSFTransportRef transport{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Entry* e = Find(id);
      if (e && e->config) cfg = e->config->Ref();
      if (e) transport = e->transport;
    }
    st = driver.vt->initialize ? driver.vt->initialize(driver.self, cfg, transport) : HSF_OK;
    if (st >= 0 && driver.vt->start) st = driver.vt->start(driver.self);
  } catch (...) {
    st = HSF_ERR_INTERNAL;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  Entry* e = Find(id);
  if (!e) {
    error = "plugin " + id + " disappeared while starting";
    return false;
  }
  if (st < 0) {
    // ENABLED but not RUNNING, with a reason. Not FAILED: the operator's intent
    // is still "enabled", and a device that comes back should be able to start
    // without them touching the switch again.
    e->state = PluginState::kEnabled;
    e->last_error = std::string("start failed: ") + hsf_status_name(st);
    error = e->last_error;
    LogWarn("Plugin " + id + " is enabled but did not start: " + error);
    return false;
  }
  e->state = PluginState::kRunning;
  e->last_error.clear();
  LogInfo("Started plugin " + id);
  return true;
}

bool PluginManager::Disable(const std::string& id, std::string& error) {
  HSFDriverRef driver{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(id);
    if (!e) {
      error = "no such plugin: " + id;
      return false;
    }
    e->enabled = false;
    driver = e->driver;
  }

  if (driver.vt && driver.self && driver.vt->stop) {
    try {
      driver.vt->stop(driver.self);
    } catch (...) {
      LogErr("Plugin " + id + " threw from stop()");
    }
  }

  // The library stays open. Re-enabling is then fast and, more importantly,
  // cannot fail on a plugin.so that has been replaced or removed since -- an
  // operator toggling a switch should not be able to lose the plugin.
  std::lock_guard<std::mutex> lock(mutex_);
  if (Entry* e = Find(id)) {
    e->state = PluginState::kDisabled;
    e->last_error.clear();
  }
  LogInfo("Disabled plugin " + id);
  return true;
}

bool PluginManager::Install(const std::string& source_dir, std::string& installed_id, std::string& error) {
  std::error_code ec;
  const std::filesystem::path source = std::filesystem::weakly_canonical(source_dir, ec);
  if (ec || !std::filesystem::is_directory(source, ec)) {
    error = "plugin package directory does not exist";
    return false;
  }

  ManifestResult checked = LoadManifest((source / "manifest.json").string(), HSF_PLUGIN_API_VERSION);
  if (!checked.ok) {
    error = std::string(ManifestErrorName(checked.error)) + ": " + checked.message;
    return false;
  }

  std::string root;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    root = root_;
  }
  if (root.empty()) {
    error = "plugin root is not configured";
    return false;
  }

  const std::filesystem::path target = std::filesystem::path(root) / checked.manifest.id;
  const std::filesystem::path rollback_root = std::filesystem::path(root) / ".rollback";
  const std::filesystem::path rollback = rollback_root / checked.manifest.id;
  const std::filesystem::path staging = std::filesystem::path(root) /
                                        ("." + checked.manifest.id + ".installing");
  const std::filesystem::path canonical_target = std::filesystem::weakly_canonical(target, ec);
  if (!ec && source == canonical_target) {
    error = "source directory is already the installed plugin";
    return false;
  }

  // Never copy a package into a live library. Unload first, then stage and
  // atomically rename the directory into place.
  std::string unload_error;
  Unload(checked.manifest.id, unload_error);
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(rollback_root, ec);
  if (ec) { error = "cannot create plugin rollback directory"; return false; }
  if (std::filesystem::exists(rollback, ec)) std::filesystem::remove_all(rollback, ec);
  if (std::filesystem::exists(target, ec)) std::filesystem::rename(target, rollback, ec);
  if (ec) { error = "cannot save the current plugin for rollback"; return false; }
  std::filesystem::copy(source, staging, std::filesystem::copy_options::recursive, ec);
  if (ec) {
    std::filesystem::rename(rollback, target, ec);
    error = "cannot copy plugin package: " + ec.message();
    return false;
  }
  std::filesystem::rename(staging, target, ec);
  if (ec) {
    std::filesystem::remove_all(staging, ec);
    std::filesystem::rename(rollback, target, ec);
    error = "cannot activate plugin package: " + ec.message();
    return false;
  }

  installed_id = checked.manifest.id;
  Discover();
  return true;
}

bool PluginManager::ImportUploaded(const std::string& manifest_json, const std::vector<unsigned char>& binary,
                                   std::string& installed_id, std::string& error) {
  nlohmann::json manifest;
  try { manifest = nlohmann::json::parse(manifest_json); }
  catch (const std::exception& e) { error = std::string("invalid manifest: ") + e.what(); return false; }
  const std::string id = manifest.value("id", std::string());
  const std::string entry = manifest.value("entry", std::string());
  if (id.empty() || entry.empty() || binary.empty()) { error = "manifest id, entry and plugin binary are required"; return false; }
  std::string root;
  { std::lock_guard<std::mutex> lock(mutex_); root = root_; }
  if (root.empty()) { error = "plugin root is not configured"; return false; }
  const auto staging = std::filesystem::path(root) / ("." + id + ".uploading");
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging, ec);
  if (ec) { error = "cannot create plugin upload staging directory"; return false; }
  { std::ofstream out(staging / "manifest.json", std::ios::binary); out << manifest.dump(2); }
  { std::ofstream out(staging / entry, std::ios::binary); out.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size())); }
  bool ok = Install(staging.string(), installed_id, error);
  std::filesystem::remove_all(staging, ec);
  return ok;
}

bool PluginManager::Rollback(const std::string& id, std::string& error) {
  std::string root;
  { std::lock_guard<std::mutex> lock(mutex_); root = root_; }
  const std::filesystem::path current = std::filesystem::path(root) / id;
  const std::filesystem::path previous = std::filesystem::path(root) / ".rollback" / id;
  const std::filesystem::path temporary = std::filesystem::path(root) / ("." + id + ".rollback");
  std::error_code ec;
  if (!std::filesystem::is_directory(previous, ec)) { error = "no rollback package is available"; return false; }
  std::string ignored;
  Unload(id, ignored);
  std::filesystem::remove_all(temporary, ec);
  if (std::filesystem::exists(current, ec)) std::filesystem::rename(current, temporary, ec);
  if (ec) { error = "cannot prepare plugin rollback"; return false; }
  std::filesystem::rename(previous, current, ec);
  if (ec) { std::filesystem::rename(temporary, current, ec); error = "cannot activate rollback"; return false; }
  std::filesystem::remove_all(temporary, ec);
  Discover();
  return true;
}

bool PluginManager::Remove(const std::string& id, std::string& error) {
  std::string root;
  { std::lock_guard<std::mutex> lock(mutex_); root = root_; }
  std::string ignored;
  Unload(id, ignored);
  std::error_code ec;
  const auto target = std::filesystem::path(root) / id;
  if (!std::filesystem::is_directory(target, ec)) { error = "plugin is not installed"; return false; }
  std::filesystem::remove_all(target, ec);
  if (ec) { error = "cannot delete plugin: " + ec.message(); return false; }
  Discover();
  return true;
}

bool PluginManager::HealthCheck(const std::string& id, std::string& error) {
  HSFDriverRef driver{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* e = Find(id);
    if (!e) {
      error = "no such plugin: " + id;
      return false;
    }
    if (e->state != PluginState::kRunning) {
      error = "plugin " + id + " is " + PluginStateName(e->state) + ", not RUNNING";
      return false;
    }
    driver = e->driver;
  }
  if (!driver.vt || !driver.self || !driver.vt->health) {
    error = "plugin " + id + " does not implement a health check";
    return false;
  }
  HSFStatus st = HSF_ERR_UNKNOWN;
  try {
    st = driver.vt->health(driver.self);
  } catch (...) {
    st = HSF_ERR_INTERNAL;
  }
  if (st < 0) {
    error = std::string("health check failed: ") + hsf_status_name(st);
    // Record it so the UI can show why without repeating the probe.
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = Find(id)) e->last_error = error;
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (Entry* e = Find(id)) e->last_error.clear();
  return true;
}

std::vector<PluginRecord> PluginManager::List() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PluginRecord> out;
  out.reserve(plugins_.size());
  for (const auto& kv : plugins_) {
    if (kv.second) out.push_back(ToRecord(*kv.second));
  }
  return out;
}

bool PluginManager::Get(const std::string& id, PluginRecord* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Entry* e = Find(id);
  if (!e) return false;
  if (out) *out = ToRecord(*e);
  return true;
}

nlohmann::json PluginManager::ToJson() const {
  nlohmann::json plugins = nlohmann::json::array();
  for (const PluginRecord& r : List()) plugins.push_back(r.ToJson());
  nlohmann::json j;
  j["plugins"] = plugins;
  j["api_version"] = std::to_string(HSF_API_VERSION_MAJOR_OF(HSF_PLUGIN_API_VERSION)) + "." +
                     std::to_string(HSF_API_VERSION_MINOR_OF(HSF_PLUGIN_API_VERSION));
  j["platform"] = HSF_PLATFORM_TRIPLE;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    j["plugin_root"] = root_;
  }
  j["events_dropped"] = events_.Dropped();
  j["event_queue_depth"] = events_.QueueDepth();
  return j;
}

HSFDriverRef PluginManager::Driver(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Entry* e = Find(id);
  return e ? e->driver : HSFDriverRef{};
}

size_t PluginManager::DispatchEvents(size_t max_events) {
  return events_.Dispatch(max_events);
}

}  // namespace hsf
