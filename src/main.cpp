#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "hsf/CardClientManager.h"
#include "hsf/ConfigManager.h"
#include "hsf/LogStore.h"
#include "hsf/LuaRuntimeManager.h"
#include "hsf/Logger.h"
#include "hsf/ModbusClient.h"
#include "hsf/ModbusRegistry.h"
#include "hsf/MqClient.h"
#include "hsf/RestClient.h"
#include "hsf/RfidClient.h"
#include "hsf/RuntimeVariables.h"
#include "hsf/SerialPort.h"
#include "hsf/WebServer.h"
#include "hsf/lua_package/PackageManager.h"
#include "hsf/plugin_manager/PluginManager.h"
#include "hsf/security/SecurityStore.h"
#include "hsf/update/SignatureVerifier.h"
#include "hsf/update/UpdateInstaller.h"
#include "hsf/update/UpdateManager.h"
#include "hsf/zk_controller/ZkController.h"

namespace {
std::atomic<bool> g_running{true};
// Set instead of (or alongside) g_running being cleared when the gateway is
// coming down to run a newly installed release rather than to stay down.
std::atomic<bool> g_restarting{false};
void HandleSignal(int) { g_running.store(false); }

// Exit status used when the gateway stops in order to be started again -- an
// installed update, or a rollback to the release before it. Distinct from 0 so
// a supervisor, a log or a person can tell "finished" from "start me again",
// and chosen as EX_TEMPFAIL so it reads as transient to anything that already
// knows sysexits. systemd's Restart=always brings the service back either way;
// the code is for the humans and for a supervisor configured more narrowly.
constexpr int kRestartExitCode = 75;

// Directory holding the running executable. Everything relocatable is
// resolved relative to this, so an extracted release package never reaches
// back into the machine it was built on (request/release.md section 17).
std::filesystem::path ExecutableDir(const char* argv0) {
  std::error_code ec;
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH] = {};
  DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(buffer).parent_path();
  }
#else
  // /proc/self/exe is Linux-only; macOS falls through to the argv[0] path
  // below, which is correct whenever the program was launched by path (which
  // run.sh and the packaged layout always do).
  std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !self.empty()) return self.parent_path();
#endif
  if (argv0 && *argv0) {
    std::filesystem::path guess = std::filesystem::absolute(argv0, ec);
    if (!ec) return guess.parent_path();
  }
  return std::filesystem::current_path(ec);
}

// Finds `name` (e.g. "web") in the layouts the gateway can legitimately run
// from, most-specific first:
//
//   <exe>/web                          exe sitting directly beside web/
//   <exe>/../web                       release package: bin/ beside web/
//   <exe>/../share/hsf_gateway/web     `cmake --install` prefix layout
//   <exe>/../../web                    build-win/Release/ inside the tree
//   HSF_WEB_ROOT                       configure-time source path
//
// The `../web` entry is what makes an extracted package self-contained --
// scripts/package.* stage bin/ as a sibling of web/ and config/. Without it
// the search falls all the way through to the compile-time source path and
// a packaged gateway quietly serves files from the machine it was built on,
// which is the exact failure request/release.md section 17 exists to catch.
//
// Falling back to the compile-time path last is what keeps running straight
// out of the build directory working, without letting a packaged binary
// silently depend on it.
std::string ResolveResourceDir(const std::filesystem::path& exeDir, const std::string& name,
                                const std::string& compiledDefault) {
  const std::filesystem::path candidates[] = {
      exeDir / name,
      exeDir / ".." / name,
      exeDir / ".." / "share" / "hsf_gateway" / name,
      exeDir / ".." / ".." / name,
  };
  std::error_code ec;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_directory(candidate, ec)) {
      return std::filesystem::weakly_canonical(candidate, ec).string();
    }
  }
  return compiledDefault;
}

void PrintVersion() {
  // Deliberately plain stdout, not the Logger: `--version` must work before
  // any config or log directory exists, and its output gets scraped by
  // packaging scripts (request/release.md section 27).
  std::cout << "eMaster Gateway\n"
            << "Version:  " << HSF_VERSION << "\n"
            << "Commit:   " << HSF_GIT_COMMIT << "\n"
            << "Built:    " << HSF_BUILD_DATE << "\n"
            << "Platform: " << HSF_PLATFORM << "\n"
            << "Compiler: " << HSF_COMPILER_ID << "\n"
            << "ZK controller: "
#if defined(HSF_ENABLE_ZK)
            << "PullSDK and C3 (choose with zk.backend)"
#else
            << "C3 over TCP (PullSDK is 32-bit Windows only)"
#endif
            << "\n"
            // First thing to check when an over-the-air update refuses to
            // install: a build without a crypto backend rejects every
            // signature, by design (docs/ota-update.md section 3).
            << "Update signatures: " << hsf::SignatureVerifier::Backend() << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version" || arg == "-v") {
      PrintVersion();
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: hsf_gateway [config-path]\n"
                << "  config-path   SQLite config database (default: config/config.db beside the\n"
                << "                executable, else the configure-time source path)\n"
                << "  --version     print version and build metadata\n"
                << "  --help        show this message" << std::endl;
      return 0;
    }
  }

  const std::filesystem::path exeDir = ExecutableDir(argc > 0 ? argv[0] : nullptr);

  // A config path given on the command line always wins; otherwise prefer a
  // config/ directory shipped alongside the executable over the source tree.
  std::string configPath;
  if (argc > 1 && argv[1][0] != '-') {
    configPath = argv[1];
  } else {
    const std::string configDir =
        ResolveResourceDir(exeDir, "config", std::filesystem::path(HSF_DEFAULT_CONFIG_PATH).parent_path().string());
    configPath = (std::filesystem::path(configDir) / "config.db").string();
  }

  std::filesystem::path logDir = std::filesystem::path(configPath).parent_path() / "logs";
  hsf::Logger::Instance().Init(logDir.string());
  hsf::Logger::Instance().Info(hsf::LogCategory::System, "eMaster Gateway starting");

  if (!hsf::ConfigManager::Instance().Load(configPath)) {
    hsf::Logger::Instance().Error(hsf::LogCategory::System, "Failed to load config from " + configPath);
    return 1;
  }

  // --- OTA boot guard (request/CICD.md section 3) --------------------------
  //
  // Deliberately the FIRST thing after the configuration is readable, and
  // before a single port, socket or script is touched. A freshly installed
  // release that dies while opening the serial port has to be counted as a
  // failed boot, and a counter incremented later in startup would never see
  // it -- the process would crash-loop on the new version forever with the
  // rollback sitting one line below the crash. Recording the attempt here and
  // clearing it two minutes into a healthy run (UpdateManager::RunBootCheck)
  // is what makes "it starts" the actual test.
  const hsf::UpdateConfig updateConfig = hsf::ConfigManager::Instance().GetUpdate();
  {
    const hsf::UpdateInstaller::Layout layout =
        hsf::UpdateInstaller::Detect(exeDir.string(), updateConfig.install_root);
    std::string rolledBackTo;
    if (hsf::UpdateInstaller::CheckBoot(layout, HSF_VERSION, updateConfig.max_boot_attempts, rolledBackTo)) {
      hsf::Logger::Instance().Error(hsf::LogCategory::System,
                                     "Rolled back to " + rolledBackTo + "; restarting to run it");
      if (!updateConfig.restart_command.empty()) {
        std::system(updateConfig.restart_command.c_str());
      }
      return kRestartExitCode;
    }
  }

  // Security database: accounts, sessions, lockouts and the audit trail
  // (request/AdvanceUpdate.md Phase 1). Fatal if it cannot be opened, unlike
  // the structured log store below: with auth.enabled defaulting on, a gateway
  // that cannot read its user table is a gateway that would either refuse
  // every request or -- far worse -- be tempted to let them all through.
  {
    const std::string securityPath =
        (std::filesystem::path(configPath).parent_path() / "security.db").string();
    std::string seededPassword;
    if (!hsf::SecurityStore::Instance().Load(securityPath, seededPassword)) {
      hsf::Logger::Instance().Error(hsf::LogCategory::System,
                                     "Failed to open the security database at " + securityPath);
      return 1;
    }
    if (!seededPassword.empty()) {
      // Printed once, here, and never stored anywhere it can be read back --
      // the database holds only the Argon2id hash. Straight to stdout as well
      // as the log, because on a first boot the operator is watching the
      // console and this is the only chance to see it.
      const std::string banner(64, '=');
      std::cout << "\n" << banner << "\n"
                << "  A new security database was created.\n"
                << "  Sign in at the web UI with:\n\n"
                << "      username:  admin\n"
                << "      password:  " << seededPassword << "\n\n"
                << "  This password is shown ONCE and is not recoverable.\n"
                << "  Change it after signing in.\n"
                << banner << "\n" << std::endl;
      hsf::Logger::Instance().Warning(hsf::LogCategory::System,
                                       "Seeded the 'admin' account; its one-time password was printed to "
                                       "the console. Change it after first sign-in.");
    }
    const hsf::AuthConfig authConfig = hsf::ConfigManager::Instance().GetAuth();
    hsf::SecurityStore::Instance().PruneAudit(authConfig.audit_retention_days);
    if (!authConfig.enabled) {
      hsf::Logger::Instance().Warning(hsf::LogCategory::System,
                                       "API authentication is DISABLED (auth.enabled=false). Every endpoint "
                                       "on this port is reachable without credentials.");
    }
  }

  std::string clientsPath = (std::filesystem::path(configPath).parent_path() / "clients.db").string();
  if (!hsf::CardClientManager::Instance().Load(clientsPath)) {
    hsf::Logger::Instance().Error(hsf::LogCategory::System, "Failed to load card clients from " + clientsPath);
    return 1;
  }

  // Structured business logs (request/upgrade.md sections 7-11): its own
  // database beside config.db and clients.db, seeded with the log type
  // definitions from log_definitions.json.
  //
  // A failure here is deliberately not fatal, unlike the two above: the
  // gateway's job is issuing cards, and losing the searchable log is worth a
  // loud complaint but not a refusal to start. Log.Write() then reports the
  // store as unavailable instead of silently dropping rows.
  const std::filesystem::path configDirPath = std::filesystem::path(configPath).parent_path();

  // Plugins are deliberately outside the executable and configuration trees:
  // an installed driver can be added, replaced, or removed without rebuilding
  // Core, while its writable state stays beside the operator's configuration.
  // A missing directory is normal on existing installations and Discover()
  // treats it as an empty plugin set.
  hsf::PluginManager plugins;
  {
    // Keep plugins outside the disposable `current` release tree. In the
    // managed layout this resolves to /opt/hsf-gateway/plugins.
    std::filesystem::path pluginRoot = configDirPath / ".." / "plugins";
    std::error_code ec;
    pluginRoot = std::filesystem::weakly_canonical(pluginRoot, ec);
    if (ec) pluginRoot = exeDir / ".." / "plugins";
    plugins.SetPluginRoot(pluginRoot.string());
    plugins.SetDataRoot((configDirPath / "plugins").string());
    // Plugin schemas are owned by the plugin. Core stores and forwards the
    // opaque `plugins.<plugin-id>` sections without guessing their fields.
    nlohmann::json pluginConfig = hsf::ConfigManager::Instance().GetCategory("plugins");
    if (!pluginConfig.is_object()) pluginConfig = nlohmann::json::object();
    plugins.SetConfiguration(pluginConfig);
    const size_t discovered = plugins.Discover();
    hsf::Logger::Instance().Info(hsf::LogCategory::System,
                                 "Plugin discovery: " + std::to_string(discovered) + " plugin(s) found in " +
                                     pluginRoot.string());
    for (const auto& item : pluginConfig.items()) {
      if (!item.value().is_object() || !item.value().value("enabled", false)) continue;
      std::string error;
      if (plugins.Enable(item.key(), error)) {
        hsf::Logger::Instance().Info(hsf::LogCategory::System,
                                     "Auto-enabled plugin " + item.key());
      } else {
        hsf::Logger::Instance().Error(hsf::LogCategory::System,
                                      "Could not auto-enable plugin " + item.key() + ": " + error);
      }
    }
  }
  hsf::LoggingConfig loggingConfig = hsf::ConfigManager::Instance().GetLogging();

  // DEBUG is off unless asked for. Applied here rather than at Logger::Init so
  // the handful of lines logged before the config is readable still come out.
  hsf::Logger::Instance().SetMinLevel(loggingConfig.debug_enabled ? hsf::LogLevel::Debug : hsf::LogLevel::Info);
  if (hsf::LogStore::Instance().Load((configDirPath / "logs.db").string())) {
    std::filesystem::path definitionsPath = loggingConfig.definitions_path;
    if (definitionsPath.is_relative()) definitionsPath = configDirPath / definitionsPath;
    hsf::LogStore::Instance().LoadDefinitions(definitionsPath.string());

    int64_t pruned = hsf::LogStore::Instance().PruneOlderThan(loggingConfig.retention_days);
    if (pruned > 0) {
      hsf::Logger::Instance().Info(hsf::LogCategory::System,
                                    "Removed " + std::to_string(pruned) + " structured log entries older than " +
                                        std::to_string(loggingConfig.retention_days) + " days");
    }
  } else {
    hsf::Logger::Instance().Error(hsf::LogCategory::System,
                                   "Structured log store unavailable; Log.Write() will fail until fixed");
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  hsf::RestClient rest;
  rest.Configure(hsf::ConfigManager::Instance().GetRest());

  hsf::SerialPort serial;
  // TDM-800 LED display (request/LEDBoard.md), driven from Lua via
  // Serial2.* / led.lua. Not opened here: led.lua's Led.Open() owns that,
  // so a missing/wrong LED port never blocks gateway startup.
  hsf::SerialPort serial2;
  hsf::ModbusClient modbus;
  modbus.Configure(hsf::ConfigManager::Instance().GetModbus());

  hsf::RfidClient rfid;
  rfid.Configure(hsf::ConfigManager::Instance().GetRfid());

  // No Configure() step, unlike the modules above -- connect params come
  // from the Lua script's own zk.connect(ip, port, timeout, password) call
  // (request/HSF_Machine_ZK_Controller_Lua_Integration.md section 1), not
  // persisted config. Start() below only spins up the background thread
  // that's ready to act on that call whenever the script makes it.
  hsf::ZkController zk;
  // Which protocol reaches the panel. "auto" resolves to PullSDK only where its
  // 32-bit Windows DLL can load, and to C3-over-TCP everywhere else -- which is
  // what gives a Linux gateway working doors instead of
  // NotSupportedOnThisPlatform from every zk.* call.
  zk.SetBackend(hsf::ConfigManager::Instance().GetZk().backend);
  // Lets RfidClient drive/observe the controller when rfid.mode is "zk".
  // Wired before Start() below, since that is when the mode takes effect.
  rfid.SetZkController(&zk);

  // RabbitMQ. Its thread starts unconditionally but idles while mq.enabled is
  // off, so turning the broker on from the Configuration page needs no restart
  // -- the same live-reconfiguration the PLC gets in the poll thread below.
  hsf::MqClient mq;
  mq.Configure(hsf::ConfigManager::Instance().GetMq());

  // Owns every running script (request/upgrade.md sections 20-28). Also owns
  // the single serial/ZK callback slots, fanning each event out to all live
  // runtimes -- see LuaRuntimeManager::Bind.
  hsf::LuaRuntimeManager lua;
  lua.Bind(&rest, &serial, &serial2, &modbus, &rfid, &zk, &mq, &plugins);

  rfid.SetCardCallback([&lua](const std::string& uid) {
    hsf::RuntimeVariables::Instance().Set("LastRfidUid", uid);
    hsf::Logger::Instance().Info(hsf::LogCategory::Rfid, "Card UID received: " + uid);
    lua.QueueEventAll("OnRfidCardRead", uid);
  });

  // Both ports keep themselves connected from here on (docs/FixSerial.md §8,
  // §22): the gateway opens them once at startup and a background worker
  // reopens either one if it drops, so no script ever waits on a reconnect and
  // a reader unplugged mid-shift comes back on its own. This is also what makes
  // Serial.Close() safe to treat as "release" rather than "close".
  serial.SetAutoReopen(true);
  serial2.SetAutoReopen(true);

  if (!serial.Open(hsf::ConfigManager::Instance().GetSerial())) {
    hsf::Logger::Instance().Warning(hsf::LogCategory::Serial,
                                     "Serial port not available at startup; retrying in the background");
  }

  // Opened here too, rather than left to led.lua's first Led.Show(): with the
  // port persistent there is nothing to gain from opening it lazily, and the
  // retry worker can only keep it alive once it has been asked for.
  if (!serial2.Open(hsf::ConfigManager::Instance().GetSerial2())) {
    hsf::Logger::Instance().Warning(hsf::LogCategory::Serial,
                                     "LED display port not available at startup; retrying in the background");
  }

  if (!modbus.Connect()) {
    hsf::Logger::Instance().Warning(hsf::LogCategory::Modbus,
                                     "PLC not reachable at startup; retrying every 5s in the background");
  }

  rfid.Start();
  zk.Start();
  mq.Start();

  // The startup script, if one is configured. No thread of its own any more:
  // LuaRuntimeManager gives every script its own thread and returns as soon as
  // the top-level code settles, so a script with no yield point (an
  // unconditional `while true do end`) no longer blocks startup and the web
  // server still comes up -- which is what makes the Lua Editor's Stop button
  // reachable at all.
  // Production Lua packaging (request/AdvanceUpdate.md Phase 2). The signing
  // key is created only where one already exists or where the operator asks
  // for one; a gateway that merely RUNS packages is provisioned with the
  // public half and never holds the secret.
  hsf::PackageManager packages(configDirPath.string());
  {
    std::string error;
    if (!packages.Initialise(/*wantSigningKey=*/false, error)) {
      hsf::Logger::Instance().Error(hsf::LogCategory::Lua, "Lua packaging unavailable: " + error);
    }
  }

  // A deployed package wins over lua.script_path. Both are "what this gateway
  // runs", and having the plaintext script silently start alongside a deployed
  // production artifact would run the business logic twice -- two state
  // machines fighting over the same doors.
  bool startedPackage = false;
  for (const std::string& file : packages.RunningFiles()) {
    hsf::LuaBundle bundle;
    hsf::LuaPackage::Metadata metadata;
    std::string error;
    if (!packages.OpenBundle(file, bundle, metadata, error)) {
      hsf::Logger::Instance().Error(hsf::LogCategory::Lua,
                                     "Deployed package " + file + " will not open: " + error);
    } else {
      std::vector<std::pair<std::string, std::string>> modules;
      modules.reserve(bundle.Count());
      for (const auto& module : bundle.Modules()) {
        modules.emplace_back(module.name, std::string(reinterpret_cast<const char*>(module.bytecode.data()),
                                                       module.bytecode.size()));
      }
      const std::vector<unsigned char>* entry = bundle.EntryBytecode();
      if (entry != nullptr &&
          lua.StartPackage(file, modules,
                            std::string(reinterpret_cast<const char*>(entry->data()), entry->size()),
                            error) == hsf::LuaRuntimeManager::StartResult::kStarted) {
        startedPackage = true;
        hsf::Logger::Instance().Info(hsf::LogCategory::Lua,
                                      "Started deployed Lua package " + file + " (version " +
                                          metadata.version + ", " + std::to_string(bundle.Count()) +
                                          " modules)");
      } else {
        hsf::Logger::Instance().Error(hsf::LogCategory::Lua,
                                       "Deployed package " + file + " failed to start: " + error);
      }
    }
  }

  // A deployed package that the operator explicitly stopped must stay stopped;
  // do not silently replace it with lua.script_path on the next boot.
  if (!startedPackage && !packages.HasActive()) {
    std::string scriptPath = hsf::ConfigManager::Instance().GetLua().script_path;
    if (scriptPath.empty()) {
      hsf::Logger::Instance().Info(hsf::LogCategory::Lua,
                                    "No startup script configured; start one from the Lua Editor");
    } else {
      std::string error;
      if (lua.StartFile(scriptPath, error) != hsf::LuaRuntimeManager::StartResult::kStarted) {
        hsf::Logger::Instance().Error(hsf::LogCategory::Lua, "Failed to start " + scriptPath + ": " + error);
      }
    }
  }

  hsf::WebServer web;

  // Declared AFTER the web server on purpose. Its background thread pushes
  // status frames through a callback holding WebServer's pimpl, so it has to
  // be the first of the two destroyed -- locals are destroyed in reverse order
  // of declaration, and getting this backwards would let a download-progress
  // frame land in a freed object during shutdown. WebServer::Stop() clears the
  // callback as well; both, because either alone is a comment away from being
  // deleted by someone who only sees the other.
  hsf::UpdateManager updater;
  updater.Configure(updateConfig, exeDir.string(), HSF_PLATFORM, HSF_VERSION,
                     std::filesystem::path(configPath).parent_path().string());
  // Shuts the gateway down the same way SIGTERM does -- scripts stopped, doors
  // left in a defined state, databases closed -- and marks the exit as "start
  // me again" rather than "I am done".
  updater.SetRestartCallback([] {
    g_restarting.store(true);
    g_running.store(false);
  });

  // Resolved once, and LOGGED, because "Not found: index.html" is otherwise
  // an unsolvable message: the search falls back to HSF_WEB_ROOT, the source
  // path of whatever machine built the binary, so a gateway deployed without
  // its web/ directory beside it reports a missing file while pointing at a
  // directory that never existed on this host. Saying where it looked turns
  // that into a one-line diagnosis.
  const std::string webRoot = ResolveResourceDir(exeDir, "web", HSF_WEB_ROOT);
  if (!std::filesystem::is_directory(webRoot)) {
    hsf::Logger::Instance().Error(hsf::LogCategory::System,
                                   "Web root does not exist: " + webRoot +
                                       " -- the UI will 404. Run the gateway from a release layout "
                                       "(bin/ beside web/) or copy web/ next to the binary.");
  } else {
    hsf::Logger::Instance().Info(hsf::LogCategory::System, "Web root: " + webRoot);
  }
  web.Configure(hsf::ConfigManager::Instance().GetWeb(), webRoot);
  web.SetModules(&rest, &serial, &serial2, &modbus, &rfid, &lua, &mq, &zk, &updater);
  web.SetPackageManager(&packages);
  web.SetPluginManager(&plugins);
  web.Start();
  updater.Start();

  // Polls PLC input coils for changes so Lua's OnPlcInputChanged handler
  // fires on transitions, and mirrors module connection state into the
  // runtime variables the dashboard already displays.
  std::thread pollThread([&] {
    hsf::ModbusConfig modbusConfig = hsf::ConfigManager::Instance().GetModbus();
    std::vector<bool> lastInputs(static_cast<size_t>(modbusConfig.input_coil_count), false);
    bool haveLastInputs = false;

    // PLC auto-reconnect. The PLC is the one connection the gateway drives
    // itself (Serial reopens per script, RFID and ZK own their own retry
    // loops), and previously a single failed connect at startup left it
    // down until a restart -- so a PLC that booted slower than the gateway
    // never came back.
    constexpr int kPlcReconnectIntervalMs = 5000;
    auto lastReconnectAttempt = std::chrono::steady_clock::now();
    std::string lastEndpoint = modbusConfig.ip + ":" + std::to_string(modbusConfig.port);

    // Slower than the PLC retry: this is a real HTTP request to someone
    // else's server, so probing it every few seconds would be rude and
    // would show up in their access logs as a flood.
    constexpr int kRestProbeIntervalMs = 15000;
    // Zero-initialised so the first probe fires immediately rather than
    // leaving REST status blank for the first 15 seconds after boot.
    std::chrono::steady_clock::time_point lastRestProbe{};

    while (g_running.load()) {
      // Re-read each pass so an address change on the Configuration page
      // takes effect without a restart, the same way RfidClient does.
      modbusConfig = hsf::ConfigManager::Instance().GetModbus();

      std::string endpoint = modbusConfig.ip + ":" + std::to_string(modbusConfig.port);
      if (endpoint != lastEndpoint) {
        hsf::Logger::Instance().Info(hsf::LogCategory::Modbus,
                                      "PLC address changed to " + endpoint + "; reconnecting");
        modbus.Disconnect();
        modbus.Configure(modbusConfig);
        lastEndpoint = endpoint;
        // Retry immediately rather than waiting out the interval -- the
        // operator just asked for this change and is watching for it.
        lastReconnectAttempt = std::chrono::steady_clock::now() -
                                std::chrono::milliseconds(kPlcReconnectIntervalMs);
      }

      auto now = std::chrono::steady_clock::now();
      if (!modbus.IsConnected() &&
          now - lastReconnectAttempt >= std::chrono::milliseconds(kPlcReconnectIntervalMs)) {
        lastReconnectAttempt = now;
        if (modbus.Connect()) {
          hsf::Logger::Instance().Info(hsf::LogCategory::Modbus, "PLC reconnected at " + endpoint);
          // Stale transitions from before the outage would fire a burst of
          // spurious OnPlcInputChanged events on the first read back.
          haveLastInputs = false;
        }
      }

      // REST health probe. HTTP is stateless, so there is no connection to
      // "reconnect" -- but the dashboard's REST status was driven solely by
      // the last request a Lua script happened to make, which meant it read
      // "never connected" forever on a gateway whose script does not call
      // out, and never recovered on its own once a call had failed. A
      // periodic GET of the configured URL keeps the status truthful and is
      // the retry.
      if (now - lastRestProbe >= std::chrono::milliseconds(kRestProbeIntervalMs)) {
        lastRestProbe = now;
        hsf::RestConfig restConfig = hsf::ConfigManager::Instance().GetRest();
        // Nothing to probe (and nothing meaningful to report) without a URL.
        if (!restConfig.url.empty()) {
          rest.TestConnection(restConfig);  // updates LastRequestSucceeded()
        }
      }

      // Broker settings re-read for the same reason as the PLC's above: a
      // change on the Configuration page has to take effect without a restart.
      // MqClient only drops the connection when something that actually shapes
      // it changed, so this is a no-op on every other pass.
      mq.Configure(hsf::ConfigManager::Instance().GetMq());

      // Same reason: ticking the DEBUG box on the Configuration page takes
      // effect on the next pass instead of needing a restart.
      hsf::Logger::Instance().SetMinLevel(hsf::ConfigManager::Instance().GetLogging().debug_enabled
                                               ? hsf::LogLevel::Debug
                                               : hsf::LogLevel::Info);

      hsf::RuntimeVariables::Instance().Set("PLCConnected", modbus.IsConnected());
      hsf::RuntimeVariables::Instance().Set("ReaderConnected", rfid.IsConnected());
      hsf::RuntimeVariables::Instance().Set("MqConnected", mq.IsConnected());

      std::vector<bool> inputs;
      // Discrete inputs (FC02), for the same reason as the registered points
      // below -- this block feeds Lua's OnPlcInputChanged, which was equally
      // dead on PLCs that map inputs outside the coil space.
      if (modbus.IsConnected() &&
          modbus.ReadDiscreteInputs(modbusConfig.input_coil_start, modbusConfig.input_coil_count, inputs)) {
        for (size_t i = 0; i < inputs.size(); ++i) {
          hsf::RuntimeVariables::Instance().Set("Input" + std::to_string(i), inputs[i]);
          if (haveLastInputs && i < lastInputs.size() && inputs[i] != lastInputs[i]) {
            lua.DispatchEventAll("OnPlcInputChanged", static_cast<long long>(i), inputs[i] ? 1.0 : 0.0);
          }
        }
        lastInputs = inputs;
        haveLastInputs = true;
      }

      // Points the running Lua script named via Modbus.RegisterInput/
      // RegisterOutput. Read one address at a time rather than as a block:
      // registrations are arbitrary addresses chosen by the script and may
      // be sparse or outside the configured coil windows above, so there's
      // no single range that covers them. Fine at this scale (a handful of
      // points per poll); if a script ever registers dozens, batch them by
      // contiguous run instead.
      if (modbus.IsConnected() && !hsf::ModbusRegistry::Instance().Empty()) {
        bool bit = false;
        // Coils (FC01) and discrete inputs (FC02) are separate address
        // spaces. Which one a given physical input lives in is vendor
        // dependent, and some PLCs answer BOTH at the same address while
        // only one carries the live value -- so this cannot be probed, and
        // the point's `source` decides. kAuto tries FC02 and falls back to
        // FC01 only when the PLC actually rejects it.
        for (const auto& point : hsf::ModbusRegistry::Instance().Inputs()) {
          bool read = false;
          int usedFc = 0;

          if (point.source != hsf::ModbusSource::kCoil) {
            if (modbus.ReadDiscreteInput(point.address, bit)) {
              read = true;
              usedFc = 2;
            }
          }
          if (!read && point.source != hsf::ModbusSource::kDiscreteInput) {
            if (modbus.ReadCoil(point.address, bit)) {
              read = true;
              usedFc = 1;
            }
          }

          if (read) {
            hsf::ModbusRegistry::Instance().UpdateInput(point.name, bit, usedFc);
            // Latch an auto point onto whichever space answered, so the next
            // poll doesn't pay for the failing function code again.
            if (point.source == hsf::ModbusSource::kAuto) {
              hsf::ModbusRegistry::Instance().SetResolvedSource(
                  point.name,
                  usedFc == 2 ? hsf::ModbusSource::kDiscreteInput : hsf::ModbusSource::kCoil);
              hsf::Logger::Instance().Info(
                  hsf::LogCategory::Modbus,
                  "Input \"" + point.name + "\" @" + std::to_string(point.address) +
                      " resolved to " + (usedFc == 2 ? "discrete inputs (FC02)" : "coils (FC01)") +
                      ". If the value never changes, the other space likely holds the live "
                      "state -- pass \"coil\" or \"discrete\" to Modbus.RegisterInput.");
            }
          }
        }
        for (const auto& point : hsf::ModbusRegistry::Instance().Outputs()) {
          if (modbus.ReadCoil(point.address, bit)) {
            hsf::ModbusRegistry::Instance().UpdateOutput(point.name, bit);
          }
        }

        // Typed register points. Holding registers (FC03) and input
        // registers (FC04) are, again, separate address spaces -- FC04 being
        // the read-only one.
        for (const auto& point : hsf::ModbusRegistry::Instance().Registers()) {
          const int count = hsf::RegisterCount(point.format);
          std::vector<uint16_t> words;
          const bool ok = point.input_register ? modbus.ReadInputRegisters(point.address, count, words)
                                                : modbus.ReadHoldingRegisters(point.address, count, words);
          if (!ok) {
            hsf::ModbusRegistry::Instance().UpdateRegisterError(
                point.name, std::string("read failed (FC0") + (point.input_register ? "4" : "3") + ")");
            continue;
          }

          nlohmann::json decoded;
          std::string decodeError;
          if (hsf::DecodeRegisters(point.format, words, decoded, decodeError)) {
            hsf::ModbusRegistry::Instance().UpdateRegister(point.name, words, decoded);
          } else {
            hsf::ModbusRegistry::Instance().UpdateRegisterError(point.name, decodeError);
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(modbusConfig.poll_interval_ms));
    }
  });

  while (g_running.load()) {
    // Plugin callbacks are dispatched by Core, not directly from arbitrary
    // plugin threads. This keeps the SDK event bus bounded and gives future
    // Lua/device bindings one predictable hand-off point.
    plugins.DispatchEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  hsf::Logger::Instance().Info(hsf::LogCategory::System,
                                g_restarting.load() ? "Restarting to run the installed update" : "Shutting down");
  // Before web.Stop(), which clears the callback this thread might be holding.
  updater.Stop();
  pollThread.join();
  web.Stop();
  rfid.Stop();
  zk.Stop();
  // Before the scripts are stopped, not after: this joins the broker thread, and
  // that thread is what still holds anything a script asked to publish.
  mq.Stop();
  modbus.Disconnect();
  serial.Close();
  serial2.Close();
  // Interrupts every running script and joins its thread, however stuck it is.
  lua.StopAll();

  return g_restarting.load() ? kRestartExitCode : 0;
}
