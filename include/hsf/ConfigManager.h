#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace hsf {

// Identity of this gateway installation. Nothing in C++ reads these -- they
// exist because Lua scripts need them (request/upgrade.md section 5's own
// example is Config.Get("system.machine_id")), and hard-coding a machine id
// in a script means every deployment ships a different script.
struct SystemConfig {
  std::string machine_id = "GW001";
  std::string device_name = "eMaster Gateway";
};

struct WebConfig {
  int port = 8080;
  std::string bind_address = "0.0.0.0";

  // The SmartLocker floor-plan UI, served on its OWN port from web/locker/ --
  // a separate listener rather than another page on 8080 because it is a
  // different audience: the gateway UI is for whoever configures and debugs the
  // machine, this one is the operator's wall display, and putting it on its own
  // port is what lets a firewall or a kiosk browser be pointed at exactly one
  // of them. Same process, same bind address, second Crow app.
  //
  // Off by default: an installation that is not a locker cabinet must not start
  // listening on a second port after an upgrade.
  bool locker_ui_enabled = false;
  int locker_ui_port = 8081;

  // Where that UI reads locker and employee rows from -- the database the
  // SmartLocker Lua application writes with Db.*. Relative paths resolve
  // against the config directory, like logging.definitions_path. Opened
  // READ-ONLY: the UI displays and requests, the script decides.
  std::string locker_db_path = "smartlocker.db";
};

struct RestConfig {
  std::string url;
  std::string api_key;
  int timeout_ms = 5000;
  int retry_count = 3;
  bool ssl_enable = true;
};

struct SerialConfig {
  std::string port = "/dev/ttyUSB0";
  int baudrate = 115200;
  int data_bits = 8;
  int stop_bits = 1;
  char parity = 'N';  // 'N', 'E', 'O'
};

struct ModbusConfig {
  std::string ip = "192.168.1.10";
  int port = 502;
  int input_coil_start = 0;
  int input_coil_count = 8;
  int output_coil_start = 0;
  int output_coil_count = 4;
  int holding_register_start = 0;
  int holding_register_count = 16;
  int poll_interval_ms = 500;
};

struct RfidConfig {
  // How the gateway obtains card reads and reader status:
  //
  //   "tcp_json"  Connect to a TCP server that pushes JSON per read; the
  //               card value is taken from the payload's data.Raw field.
  //               Status = that connection being up.
  //   "zk"        Drive a ZKTeco controller over PullSDK (ZkController),
  //               with its own heartbeat and auto-reconnect. Status = that
  //               controller connection.
  //   "card_api"  Readers push to POST /api/card/input instead, so there is
  //               no outbound connection to watch -- status comes from
  //               pinging the reader's IP.
  //
  // `ip`/`port` mean whichever endpoint the selected mode talks to.
  std::string mode = "tcp_json";
  std::string ip = "192.168.1.20";
  int port = 5000;
  int reconnect_interval_ms = 3000;
  int timeout_ms = 2000;
};

// ZKTeco access controller — the card reader at the scan position, reached
// from Lua as zk.connect(ip, port, timeout, password).
//
// Deliberately its own section rather than reusing `rfid` even though
// rfid.mode can be "zk": on a machine that reads citizen cards over serial and
// card UIDs from a ZK controller, `rfid` describes a DIFFERENT device (a
// tcp_json reader, say) at a different address. Folding the two together would
// mean one address field for two boxes.
//
// Nothing in C++ reads these — ZkController takes its parameters from the
// script's own zk.connect() call. They live here so that call can be
// Config.Get("zk.ip") instead of an address edited into a Lua file per site.
struct ZkConfig {
  std::string ip = "10.0.0.237";
  int port = 4370;
  int timeout_ms = 2000;
  // Comm password, "" for a controller that has none (which is what
  // config/scripts/zkcard.lua passed before this section existed).
  std::string password;

  // Which protocol to reach the panel with:
  //
  //   "auto"     PullSDK where it is compiled in (32-bit Windows), C3 elsewhere
  //   "pullsdk"  force the ZKTeco DLL; fails everywhere it cannot load
  //   "c3"       force the panel's own TCP protocol
  //
  // "auto" is what makes a Linux gateway able to open a door at all: PullSDK is
  // a 32-bit Windows-only DLL, so before the C3 backend existed every zk.* call
  // on Linux returned NotSupportedOnThisPlatform. Force "c3" on Windows too if
  // you would rather not depend on the vendor DLL.
  std::string backend = "auto";
};

// RabbitMQ / AMQP 0-9-1 broker (MqClient), reached from Lua as Mq.*.
//
// Defaults follow qt-mq-lab's RabbitMQ.md so the two talk to each other out of
// the box -- including its `dev`/`devpass` login, which is NOT RabbitMQ's stock
// guest/guest and has to exist on the broker. `enabled` is the exception: off,
// because an existing gateway that has never had a broker must not start
// dialling one after an upgrade.
struct MqConfig {
  bool enabled = false;
  std::string host = "localhost";
  int port = 5672;
  std::string vhost = "/";
  std::string user = "dev";
  std::string password = "devpass";

  // Topology. `declare_topology` off is for a broker where shared
  // infrastructure owns the exchange/queue and this gateway is only a client --
  // RabbitMQ.md's planned "Step 3". Declaring is idempotent, but only if the
  // arguments match what is already there, so a mismatched redeclare is a
  // channel error rather than a no-op.
  std::string exchange = "mqlab.direct";
  std::string exchange_type = "direct";  // direct | fanout | topic | headers
  std::string queue = "mqlab.messages";
  std::string routing_key = "mqlab.msg";
  bool declare_topology = true;

  // Consume side. `consume` off makes this a publish-only client -- the common
  // case for a gateway that reports events upstream and takes no commands.
  bool consume = true;
  int prefetch = 10;

  // Wrap published bodies in (and parse received bodies as) RabbitMQ.md's v1
  // envelope. Off means bodies go on the wire exactly as the script wrote them.
  bool envelope = true;

  int heartbeat_sec = 60;
  int reconnect_interval_ms = 5000;
  // Outbound messages held while the broker is unreachable. Finite on purpose:
  // a gateway that publishes per card read and cannot reach the broker for a
  // day would otherwise grow until it is killed.
  int publish_queue_limit = 1000;
  // Received messages waiting for Mq.Get(). The oldest is evicted when full
  // (counted in MqStatus::overflowed) -- see the ack note on MqClient.
  int inbox_limit = 100;
};

struct LuaConfig {
  std::string script_path = "config/scripts/main.lua";
};

// Structured logging (LogStore), not the runtime Logger -- the two are
// deliberately separate: Log.Info/Warning/... stay console+file+ring buffer
// for debugging, while Log.Write() rows land in SQLite and are searchable
// from the Logs page (request/upgrade.md section 12).
struct LoggingConfig {
  // Master switch. With this off, Log.Write() returns false with an
  // explanatory error rather than silently discarding business records.
  bool store_enabled = true;
  // Structured rows older than this are deleted at startup. 0 disables
  // pruning -- a gateway that must keep everything can say so, but the
  // default has to be finite or the database grows without bound.
  int retention_days = 90;
  // Where the log type/field definitions come from (request/upgrade.md
  // section 8). Relative paths resolve against the config directory, so the
  // default works both in the source tree and in an extracted package.
  std::string definitions_path = "log_definitions.json";
  // DEBUG-level runtime logging (Log.Debug from scripts, and the gateway's own
  // diagnostic detail such as per-step serial open timings). Off by default:
  // every line is a file write, and some of these sit in per-iteration paths.
  // Turn it on while diagnosing, then off again.
  bool debug_enabled = false;
};

// Over-the-air updates (UpdateManager, request/CICD.md). The gateway asks
// `url` what the newest build for its platform is, and offers it to whoever is
// looking at the web UI; nothing installs without a click.
//
// `enabled` is off by default, and that default is not laziness: a fleet that
// upgrades to a build with this section in it must not immediately start making
// outbound HTTPS requests to a host nobody has configured. Turning it on is the
// operator saying which release server they trust.
// API authentication, authorization and abuse protection
// (request/AdvanceUpdate.md sections 1.1, 1.3, 1.4, 1.11).
//
// `enabled` defaults TRUE, unlike every other feature switch added to this
// file. The others default off so that upgrading a fleet changes nothing;
// here, defaulting off would ship an authentication system that protects
// nobody until somebody remembers to turn it on, which is the failure mode the
// whole plan exists to remove. The switch exists at all so that an operator
// locked out of a machine in the field has a documented way back in
// (docs/security.md), not so that it can be left off.
struct AuthConfig {
  bool enabled = true;

  // Bearer token lifetime. Section 1.1 asks for 30 minutes.
  int token_lifetime_sec = 1800;
  // Concurrent sessions per account; 0 = unlimited. A modest cap makes a
  // stolen-credential spree visible instead of unbounded.
  int max_sessions_per_user = 10;

  // Brute-force lockout (section 1.3): 5 failures, 15 minutes. Counted per
  // username in SQLite, so reconnecting -- or restarting the gateway -- does
  // not reset it.
  int max_failed_attempts = 5;
  int lockout_seconds = 900;

  // Token-bucket limits (section 1.4). `login` is per source IP and
  // deliberately tight; `api` is the general per-IP allowance for everything
  // else and has to be loose enough for the dashboard's own polling.
  bool rate_limit_enabled = true;
  double login_rate_per_sec = 0.2;  // one attempt every 5s sustained
  // Higher than max_failed_attempts on purpose. These two both stop password
  // guessing, and whichever trips first is the one the attacker -- and the
  // locked-out user -- actually sees. With the burst at 5 the rate limiter
  // answered 429 on the sixth attempt and the account lockout never engaged,
  // which hides the more useful signal: 429 says "slow down", 423 says "this
  // account is locked for 15 minutes" and is what section 1.3 asks for. The
  // limiter stays as the backstop for the case the lockout cannot see -- an
  // attacker rotating usernames, where no single account ever reaches five.
  double login_burst = 10.0;
  double api_rate_per_sec = 40.0;
  double api_burst = 120.0;

  // Request body ceilings (section 1.12 C). Anything larger is refused before
  // it is parsed.
  int max_body_bytes = 65536;
  int max_lua_body_bytes = 1048576;

  // Security audit trail (section 1.10) and how long it is kept.
  bool audit_log = true;
  int audit_retention_days = 90;

  // Security response headers (section 1.8). Off only for someone debugging
  // the UI against a tool that the CSP blocks.
  bool security_headers = true;

  // CORS allowlist (section 1.12 B). Empty means same-origin only: no
  // Access-Control-Allow-Origin header is emitted at all, which is the correct
  // default for a UI served by the gateway itself. Comma-separated origins.
  std::string allowed_origins;

  // /api/app/<path> is registered by Lua scripts. With this false (the
  // default) those routes need a session like everything else; set it true for
  // an installation whose scripts expose an integration endpoint that external
  // equipment calls without a token.
  bool public_app_routes = false;

  // The SmartLocker floor plan runs on its own port and is normally a kiosk
  // display on a trusted segment, so it is NOT authenticated by default and
  // this preserves that. Note that it can open doors -- see docs/security.md
  // before deciding the segment really is trusted.
  bool protect_locker_ui = false;
};

struct UpdateConfig {
  bool enabled = false;
  // Manifest endpoint. Either a REST endpoint that answers per-device (the
  // gateway appends ?platform=&channel=&version=&device_id=) or a plain static
  // version.json -- both shapes are accepted, see UpdateManager::FetchManifest.
  std::string url;
  std::string channel = "stable";

  // Six hours, per request/CICD.md. A gateway that checks more often than it
  // could plausibly be updated is just noise in someone's access log.
  int check_interval_hours = 6;
  // Startup grace. The first check waits this long so it never competes with
  // opening the serial ports, connecting the PLC and starting the scripts.
  int initial_delay_sec = 30;

  // Refuse a package whose signature does not verify. Leave this on. Off is
  // for a lab where the release server is a laptop with no signing key -- it
  // reduces the check to "the bytes match the checksum the same server told us
  // to expect", which stops corruption and nothing else.
  bool require_signature = true;
  // PEM public key, relative paths resolving against the config directory.
  std::string public_key_path = "update_public_key.pem";
  bool ssl_verify = true;
  int timeout_sec = 30;

  // Managed-install root. Empty means "work it out from the running
  // executable" (<root>/releases/<version>/bin), which is right for anything
  // scripts/install-ota.sh laid out.
  std::string install_root;

  // How long the gateway must stay up before a freshly installed release is
  // declared good. Until then a crash on the next boot counts against it, and
  // enough of those roll `current` back.
  int health_confirm_sec = 120;
  int max_boot_attempts = 2;
  // Old releases to keep on disk beyond the running one and the rollback
  // target. These are tens of megabytes each on a device that may only have a
  // few hundred spare.
  int keep_releases = 2;

  // Run this instead of exiting, when a restart is needed (e.g.
  // "systemctl restart hsf-gateway"). Empty -- the default -- means the
  // gateway shuts down cleanly and exits, leaving the restart to the service
  // manager that started it. That is the safer default: it needs no privileges
  // the gateway would not otherwise have.
  std::string restart_command;
};

// Master switch for the Card Reader Client API (POST /api/card/input and
// its client-management routes) — see CardClientManager/CardCache. Unlike
// the other sections here, per-client API-Keys themselves live in
// CardClientManager's own database, not here.
struct CardApiConfig {
  bool enabled = true;
};

// Thread-safe holder for the whole gateway configuration. Loaded from and
// saved back to a single SQLite database file (default config/config.db) --
// one row per top-level section (`web`, `rest`, ...), each storing that
// section's JSON blob as text. If `path` doesn't exist yet but a sibling
// file with the same stem and a `.json` extension does (a pre-SQLite
// config.json), that file is imported once on first Load to seed the new
// database instead of silently resetting to defaults.
class ConfigManager {
 public:
  static ConfigManager& Instance();
  ~ConfigManager();

  bool Load(const std::string& path);
  bool Save() const;
  bool Save(const std::string& path);

  const std::string& Path() const { return path_; }

  // Directory Lua scripts live in: the startup script's own directory, or
  // <config dir>/scripts when no startup script is set (unchecking "Auto-run
  // on startup" clears script_path, and that must not make every other
  // script unreachable). Used both to list scripts and to build package.path
  // so require() can resolve sibling modules.
  std::string ScriptsDir() const;

  SystemConfig GetSystem() const;
  WebConfig GetWeb() const;
  RestConfig GetRest() const;
  SerialConfig GetSerial() const;
  // Second, independent serial port -- the TDM-800 LED display
  // (request/LEDBoard.md), reached from Lua as Serial2.* / led.lua. Same
  // shape as `serial`, just a separate port.
  SerialConfig GetSerial2() const;
  ModbusConfig GetModbus() const;
  RfidConfig GetRfid() const;
  ZkConfig GetZk() const;
  MqConfig GetMq() const;
  LuaConfig GetLua() const;
  LoggingConfig GetLogging() const;
  CardApiConfig GetCardApi() const;
  UpdateConfig GetUpdate() const;
  AuthConfig GetAuth() const;

  void SetSystem(const SystemConfig& cfg);
  void SetWeb(const WebConfig& cfg);
  void SetRest(const RestConfig& cfg);
  void SetSerial(const SerialConfig& cfg);
  void SetSerial2(const SerialConfig& cfg);
  void SetModbus(const ModbusConfig& cfg);
  void SetRfid(const RfidConfig& cfg);
  void SetZk(const ZkConfig& cfg);
  void SetMq(const MqConfig& cfg);
  void SetLua(const LuaConfig& cfg);
  void SetLogging(const LoggingConfig& cfg);
  void SetCardApi(const CardApiConfig& cfg);
  void SetUpdate(const UpdateConfig& cfg);
  void SetAuth(const AuthConfig& cfg);

  // Applies a partial or full JSON document (same shape as the config file)
  // on top of the current configuration. Used by the /api/config endpoint.
  bool ApplyJson(const nlohmann::json& patch);

  nlohmann::json ToJson() const;

  // --- dotted-path access, for the Lua Config.* API (request/upgrade.md
  // sections 5-6) ---------------------------------------------------------
  //
  // The path is "<section>.<key>", e.g. "modbus.ip", addressing the same
  // document ToJson() produces -- which is also the shape the REST /api/config
  // boundary uses, so a script and the Configuration page name a setting
  // identically. Deliberately NOT a raw SQL surface: section 6 requires Lua
  // to reach configuration only through this C++ API.

  // False when the section or key doesn't exist; `out` is then untouched.
  bool GetValue(const std::string& path, nlohmann::json& out) const;

  // Writes one value and persists the whole configuration. Fails on an
  // unknown path, or on a type the target field can't hold -- silently
  // dropping a bad Config.Set would leave a script believing it had
  // reconfigured the gateway.
  bool SetValue(const std::string& path, const nlohmann::json& value, std::string& error);

  bool HasValue(const std::string& path) const;

  // Whole section as an object; null when there is no such section.
  nlohmann::json GetCategory(const std::string& category) const;

 private:
  ConfigManager() = default;
  ConfigManager(const ConfigManager&) = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;

  nlohmann::json ToJsonUnlocked() const;

  mutable std::mutex mutex_;
  std::string path_;
  sqlite3* db_ = nullptr;
  SystemConfig system_;
  WebConfig web_;
  RestConfig rest_;
  SerialConfig serial_;
  SerialConfig serial2_;
  ModbusConfig modbus_;
  RfidConfig rfid_;
  ZkConfig zk_;
  MqConfig mq_;
  LuaConfig lua_;
  LoggingConfig logging_;
  CardApiConfig cardApi_;
  UpdateConfig update_;
  AuthConfig auth_;
  // Plugin sections are schema-owned by each plugin, so Core preserves the
  // object without interpreting its fields.
  nlohmann::json plugins_ = nlohmann::json::object();
};

}  // namespace hsf
