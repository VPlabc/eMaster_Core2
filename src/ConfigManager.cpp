#include "hsf/ConfigManager.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "hsf/Logger.h"

namespace hsf {

using nlohmann::json;

namespace {

constexpr const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS config (section TEXT PRIMARY KEY, data TEXT NOT NULL);";

// Opens (creating if necessary) the SQLite database at `path` and ensures
// the `config` table exists. On failure, logs and returns nullptr.
sqlite3* OpenDatabase(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              "ConfigManager: failed to open " + path + ": " +
                                  (db ? sqlite3_errmsg(db) : "unknown error"));
    if (db) sqlite3_close(db);
    return nullptr;
  }

  char* errMsg = nullptr;
  if (sqlite3_exec(db, kCreateTableSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("ConfigManager: failed to create schema: ") +
                                  (errMsg ? errMsg : "unknown error"));
    sqlite3_free(errMsg);
    sqlite3_close(db);
    return nullptr;
  }

  return db;
}

// Upserts one section's JSON blob into the `config` table. Caller holds
// whatever locking is appropriate; this touches no ConfigManager state.
bool UpsertSection(sqlite3* db, const std::string& section, const json& value) {
  static const char* kUpsertSql =
      "INSERT INTO config(section, data) VALUES(?, ?) "
      "ON CONFLICT(section) DO UPDATE SET data = excluded.data;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, kUpsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("ConfigManager: prepare failed: ") + sqlite3_errmsg(db));
    return false;
  }

  std::string dump = value.dump();
  sqlite3_bind_text(stmt, 1, section.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, dump.c_str(), -1, SQLITE_TRANSIENT);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("ConfigManager: upsert of '") + section +
                                  "' failed: " + sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  return ok;
}

// If the `config` table is empty and a sibling `<stem>.json` file exists
// next to the database (a pre-SQLite config file), imports its top-level
// sections into the table once so an upgrade doesn't silently reset a real
// deployment's settings back to compiled-in defaults.
void MigrateLegacyJsonIfEmpty(sqlite3* db, const std::string& dbPath) {
  sqlite3_stmt* countStmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM config;", -1, &countStmt, nullptr) != SQLITE_OK) {
    return;
  }
  long long rowCount = 0;
  if (sqlite3_step(countStmt) == SQLITE_ROW) {
    rowCount = sqlite3_column_int64(countStmt, 0);
  }
  sqlite3_finalize(countStmt);
  if (rowCount > 0) return;

  std::filesystem::path legacyPath = std::filesystem::path(dbPath).replace_extension(".json");
  std::ifstream in(legacyPath);
  if (!in.is_open()) return;

  json root;
  try {
    in >> root;
  } catch (const std::exception& e) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("ConfigManager: failed to parse legacy ") +
                                  legacyPath.string() + " for migration: " + e.what());
    return;
  }
  if (!root.is_object()) return;

  for (const auto& [section, value] : root.items()) {
    UpsertSection(db, section, value);
  }
  Logger::Instance().Info(LogCategory::System,
                           "ConfigManager: migrated legacy " + legacyPath.string() + " into " + dbPath);
}

}  // namespace

ConfigManager& ConfigManager::Instance() {
  static ConfigManager instance;
  return instance;
}

ConfigManager::~ConfigManager() {
  if (db_) sqlite3_close(db_);
}

bool ConfigManager::Load(const std::string& path) {
  sqlite3* db = OpenDatabase(path);
  if (!db) return false;

  MigrateLegacyJsonIfEmpty(db, path);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT section, data FROM config;", -1, &stmt, nullptr) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("ConfigManager: failed to query config: ") + sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  std::vector<std::pair<std::string, json>> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    std::string section = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* dataText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    try {
      rows.emplace_back(section, json::parse(dataText ? dataText : ""));
    } catch (const std::exception& e) {
      Logger::Instance().Error(LogCategory::System,
                                "ConfigManager: parse error in section '" + section + "': " + e.what());
    }
  }
  sqlite3_finalize(stmt);

  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) sqlite3_close(db_);
  db_ = db;
  path_ = path;

  for (const auto& [section, value] : rows) {
    if (section == "system") {
      system_.machine_id = value.value("machine_id", system_.machine_id);
      system_.device_name = value.value("device_name", system_.device_name);
    } else if (section == "logging") {
      logging_.store_enabled = value.value("store_enabled", logging_.store_enabled);
      logging_.retention_days = value.value("retention_days", logging_.retention_days);
      logging_.definitions_path = value.value("definitions_path", logging_.definitions_path);
      logging_.debug_enabled = value.value("debug_enabled", logging_.debug_enabled);
    } else if (section == "web") {
      web_.port = value.value("port", web_.port);
      web_.bind_address = value.value("bind_address", web_.bind_address);
      web_.locker_ui_enabled = value.value("locker_ui_enabled", web_.locker_ui_enabled);
      web_.locker_ui_port = value.value("locker_ui_port", web_.locker_ui_port);
      web_.locker_db_path = value.value("locker_db_path", web_.locker_db_path);
    } else if (section == "rest") {
      rest_.url = value.value("url", rest_.url);
      rest_.api_key = value.value("api_key", rest_.api_key);
      rest_.timeout_ms = value.value("timeout_ms", rest_.timeout_ms);
      rest_.retry_count = value.value("retry_count", rest_.retry_count);
      rest_.ssl_enable = value.value("ssl_enable", rest_.ssl_enable);
    } else if (section == "serial") {
      serial_.port = value.value("port", serial_.port);
      serial_.baudrate = value.value("baudrate", serial_.baudrate);
      serial_.data_bits = value.value("data_bits", serial_.data_bits);
      serial_.stop_bits = value.value("stop_bits", serial_.stop_bits);
      std::string parity = value.value("parity", std::string(1, serial_.parity));
      serial_.parity = parity.empty() ? 'N' : parity[0];
    } else if (section == "serial2") {
      serial2_.port = value.value("port", serial2_.port);
      serial2_.baudrate = value.value("baudrate", serial2_.baudrate);
      serial2_.data_bits = value.value("data_bits", serial2_.data_bits);
      serial2_.stop_bits = value.value("stop_bits", serial2_.stop_bits);
      std::string parity = value.value("parity", std::string(1, serial2_.parity));
      serial2_.parity = parity.empty() ? 'N' : parity[0];
    } else if (section == "modbus") {
      modbus_.enabled = value.value("enabled", modbus_.enabled);
      modbus_.ip = value.value("ip", modbus_.ip);
      modbus_.port = value.value("port", modbus_.port);
      modbus_.input_coil_start = value.value("input_coil_start", modbus_.input_coil_start);
      modbus_.input_coil_count = value.value("input_coil_count", modbus_.input_coil_count);
      modbus_.output_coil_start = value.value("output_coil_start", modbus_.output_coil_start);
      modbus_.output_coil_count = value.value("output_coil_count", modbus_.output_coil_count);
      modbus_.holding_register_start = value.value("holding_register_start", modbus_.holding_register_start);
      modbus_.holding_register_count = value.value("holding_register_count", modbus_.holding_register_count);
      modbus_.slave_enabled = value.value("slave_enabled", modbus_.slave_enabled);
      modbus_.slave_bind = value.value("slave_bind", modbus_.slave_bind);
      modbus_.slave_port = value.value("slave_port", modbus_.slave_port);
      modbus_.slave_unit_id = value.value("slave_unit_id", modbus_.slave_unit_id);
      modbus_.discrete_input_start = value.value("discrete_input_start", modbus_.discrete_input_start);
      modbus_.discrete_input_count = value.value("discrete_input_count", modbus_.discrete_input_count);
      modbus_.input_register_start = value.value("input_register_start", modbus_.input_register_start);
      modbus_.input_register_count = value.value("input_register_count", modbus_.input_register_count);
      modbus_.rtu_port = value.value("rtu_port", modbus_.rtu_port);
      modbus_.rtu_master_enabled = value.value("rtu_master_enabled", modbus_.rtu_master_enabled);
      modbus_.rtu_baudrate = value.value("rtu_baudrate", modbus_.rtu_baudrate);
      modbus_.rtu_data_bits = value.value("rtu_data_bits", modbus_.rtu_data_bits);
      modbus_.rtu_stop_bits = value.value("rtu_stop_bits", modbus_.rtu_stop_bits);
      std::string rtuParity = value.value("rtu_parity", std::string(1, modbus_.rtu_parity));
      modbus_.rtu_parity = rtuParity.empty() ? 'N' : rtuParity[0];
      modbus_.rtu_unit_id = value.value("rtu_unit_id", modbus_.rtu_unit_id);
      modbus_.rtu_slave_enabled = value.value("rtu_slave_enabled", modbus_.rtu_slave_enabled);
      modbus_.rtu_slave_id = value.value("rtu_slave_id", modbus_.rtu_slave_id);
      modbus_.poll_interval_ms = value.value("poll_interval_ms", modbus_.poll_interval_ms);
    } else if (section == "rfid") {
      rfid_.mode = value.value("mode", rfid_.mode);
      rfid_.ip = value.value("ip", rfid_.ip);
      rfid_.port = value.value("port", rfid_.port);
      rfid_.reconnect_interval_ms = value.value("reconnect_interval_ms", rfid_.reconnect_interval_ms);
      rfid_.timeout_ms = value.value("timeout_ms", rfid_.timeout_ms);
    } else if (section == "zk") {
      zk_.enabled = value.value("enabled", zk_.enabled);
      zk_.ip = value.value("ip", zk_.ip);
      zk_.port = value.value("port", zk_.port);
      zk_.timeout_ms = value.value("timeout_ms", zk_.timeout_ms);
      zk_.password = value.value("password", zk_.password);
      zk_.backend = value.value("backend", zk_.backend);
    } else if (section == "mq") {
      mq_.enabled = value.value("enabled", mq_.enabled);
      mq_.host = value.value("host", mq_.host);
      mq_.port = value.value("port", mq_.port);
      mq_.vhost = value.value("vhost", mq_.vhost);
      mq_.user = value.value("user", mq_.user);
      mq_.password = value.value("password", mq_.password);
      mq_.exchange = value.value("exchange", mq_.exchange);
      mq_.exchange_type = value.value("exchange_type", mq_.exchange_type);
      mq_.queue = value.value("queue", mq_.queue);
      mq_.routing_key = value.value("routing_key", mq_.routing_key);
      mq_.declare_topology = value.value("declare_topology", mq_.declare_topology);
      mq_.consume = value.value("consume", mq_.consume);
      mq_.prefetch = value.value("prefetch", mq_.prefetch);
      mq_.envelope = value.value("envelope", mq_.envelope);
      mq_.heartbeat_sec = value.value("heartbeat_sec", mq_.heartbeat_sec);
      mq_.reconnect_interval_ms = value.value("reconnect_interval_ms", mq_.reconnect_interval_ms);
      mq_.publish_queue_limit = value.value("publish_queue_limit", mq_.publish_queue_limit);
      mq_.inbox_limit = value.value("inbox_limit", mq_.inbox_limit);
    } else if (section == "lua") {
      lua_.script_path = value.value("script_path", lua_.script_path);
    } else if (section == "card_api") {
      cardApi_.enabled = value.value("enabled", cardApi_.enabled);
    } else if (section == "update") {
      update_.enabled = value.value("enabled", update_.enabled);
      update_.url = value.value("url", update_.url);
      update_.channel = value.value("channel", update_.channel);
      update_.check_interval_hours = value.value("check_interval_hours", update_.check_interval_hours);
      update_.initial_delay_sec = value.value("initial_delay_sec", update_.initial_delay_sec);
      update_.require_signature = value.value("require_signature", update_.require_signature);
      update_.public_key_path = value.value("public_key_path", update_.public_key_path);
      update_.ssl_verify = value.value("ssl_verify", update_.ssl_verify);
      update_.timeout_sec = value.value("timeout_sec", update_.timeout_sec);
      update_.install_root = value.value("install_root", update_.install_root);
      update_.health_confirm_sec = value.value("health_confirm_sec", update_.health_confirm_sec);
      update_.max_boot_attempts = value.value("max_boot_attempts", update_.max_boot_attempts);
      update_.keep_releases = value.value("keep_releases", update_.keep_releases);
      update_.restart_command = value.value("restart_command", update_.restart_command);
    } else if (section == "auth") {
      auth_.enabled = value.value("enabled", auth_.enabled);
      auth_.token_lifetime_sec = value.value("token_lifetime_sec", auth_.token_lifetime_sec);
      auth_.max_sessions_per_user = value.value("max_sessions_per_user", auth_.max_sessions_per_user);
      auth_.max_failed_attempts = value.value("max_failed_attempts", auth_.max_failed_attempts);
      auth_.lockout_seconds = value.value("lockout_seconds", auth_.lockout_seconds);
      auth_.rate_limit_enabled = value.value("rate_limit_enabled", auth_.rate_limit_enabled);
      auth_.login_rate_per_sec = value.value("login_rate_per_sec", auth_.login_rate_per_sec);
      auth_.login_burst = value.value("login_burst", auth_.login_burst);
      auth_.api_rate_per_sec = value.value("api_rate_per_sec", auth_.api_rate_per_sec);
      auth_.api_burst = value.value("api_burst", auth_.api_burst);
      auth_.max_body_bytes = value.value("max_body_bytes", auth_.max_body_bytes);
      auth_.max_lua_body_bytes = value.value("max_lua_body_bytes", auth_.max_lua_body_bytes);
      auth_.audit_log = value.value("audit_log", auth_.audit_log);
      auth_.audit_retention_days = value.value("audit_retention_days", auth_.audit_retention_days);
      auth_.security_headers = value.value("security_headers", auth_.security_headers);
      auth_.allowed_origins = value.value("allowed_origins", auth_.allowed_origins);
      auth_.public_app_routes = value.value("public_app_routes", auth_.public_app_routes);
      auth_.protect_locker_ui = value.value("protect_locker_ui", auth_.protect_locker_ui);
    } else if (section == "plugins" && value.is_object()) {
      plugins_ = value;
    }
  }

  return true;
}

bool ConfigManager::Save() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || path_.empty()) return false;

  json root = ToJsonUnlocked();

  char* errMsg = nullptr;
  if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("ConfigManager: BEGIN failed: ") + (errMsg ? errMsg : "unknown error"));
    sqlite3_free(errMsg);
    return false;
  }

  bool ok = true;
  for (const auto& [section, value] : root.items()) {
    if (!UpsertSection(db_, section, value)) {
      ok = false;
      break;
    }
  }

  sqlite3_exec(db_, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
  return ok;
}

bool ConfigManager::Save(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path != path_) {
      sqlite3* db = OpenDatabase(path);
      if (!db) return false;
      if (db_) sqlite3_close(db_);
      db_ = db;
      path_ = path;
    }
  }
  return Save();
}

SystemConfig ConfigManager::GetSystem() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return system_;
}
WebConfig ConfigManager::GetWeb() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return web_;
}
RestConfig ConfigManager::GetRest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rest_;
}
std::string ConfigManager::ScriptsDir() const {
  std::string scriptPath;
  std::string configPath;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    scriptPath = lua_.script_path;
    configPath = path_;
  }

  std::filesystem::path script = scriptPath;
  if (!script.empty()) {
    std::error_code ec;
    if (std::filesystem::is_directory(script, ec)) {
      return script.string();
    }

    // script_path may point at a project entry script such as
    // scripts/CardDispenser/main.lua. The Lua root is the directory above the
    // project folder, not the project folder itself.
    if (script.extension() == ".lua" && !script.parent_path().empty()) {
      const auto projectDir = script.parent_path();
      if (std::filesystem::is_regular_file(projectDir / "config_schema.lua", ec)) {
        return projectDir.parent_path().string();
      }
      return projectDir.string();
    }

    if (!script.parent_path().empty()) {
      return script.parent_path().string();
    }
  }
  return (std::filesystem::path(configPath).parent_path() / "scripts").string();
}

SerialConfig ConfigManager::GetSerial() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return serial_;
}
SerialConfig ConfigManager::GetSerial2() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return serial2_;
}
ModbusConfig ConfigManager::GetModbus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return modbus_;
}
RfidConfig ConfigManager::GetRfid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rfid_;
}
MqConfig ConfigManager::GetMq() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mq_;
}

ZkConfig ConfigManager::GetZk() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return zk_;
}
LuaConfig ConfigManager::GetLua() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lua_;
}
LoggingConfig ConfigManager::GetLogging() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return logging_;
}
CardApiConfig ConfigManager::GetCardApi() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cardApi_;
}
UpdateConfig ConfigManager::GetUpdate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return update_;
}
AuthConfig ConfigManager::GetAuth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return auth_;
}

void ConfigManager::SetSystem(const SystemConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  system_ = cfg;
}
void ConfigManager::SetWeb(const WebConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  web_ = cfg;
}
void ConfigManager::SetRest(const RestConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  rest_ = cfg;
}
void ConfigManager::SetSerial(const SerialConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  serial_ = cfg;
}
void ConfigManager::SetSerial2(const SerialConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  serial2_ = cfg;
}
void ConfigManager::SetModbus(const ModbusConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  modbus_ = cfg;
}
void ConfigManager::SetRfid(const RfidConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  rfid_ = cfg;
}
void ConfigManager::SetZk(const ZkConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  zk_ = cfg;
}
void ConfigManager::SetMq(const MqConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  mq_ = cfg;
}
void ConfigManager::SetLua(const LuaConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  lua_ = cfg;
}
void ConfigManager::SetLogging(const LoggingConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  logging_ = cfg;
}
void ConfigManager::SetCardApi(const CardApiConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  cardApi_ = cfg;
}
void ConfigManager::SetUpdate(const UpdateConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  update_ = cfg;
}
void ConfigManager::SetAuth(const AuthConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  auth_ = cfg;
}

bool ConfigManager::ApplyJson(const nlohmann::json& patch) {
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    if (patch.contains("system")) {
      const auto& s = patch["system"];
      system_.machine_id = s.value("machine_id", system_.machine_id);
      system_.device_name = s.value("device_name", system_.device_name);
    }
    if (patch.contains("logging")) {
      const auto& l = patch["logging"];
      logging_.store_enabled = l.value("store_enabled", logging_.store_enabled);
      logging_.retention_days = l.value("retention_days", logging_.retention_days);
      logging_.definitions_path = l.value("definitions_path", logging_.definitions_path);
      logging_.debug_enabled = l.value("debug_enabled", logging_.debug_enabled);
    }
    if (patch.contains("web")) {
      const auto& w = patch["web"];
      web_.port = w.value("port", web_.port);
      web_.bind_address = w.value("bind_address", web_.bind_address);
      web_.locker_ui_enabled = w.value("locker_ui_enabled", web_.locker_ui_enabled);
      web_.locker_ui_port = w.value("locker_ui_port", web_.locker_ui_port);
      web_.locker_db_path = w.value("locker_db_path", web_.locker_db_path);
    }
    if (patch.contains("rest")) {
      const auto& r = patch["rest"];
      rest_.url = r.value("url", rest_.url);
      rest_.api_key = r.value("api_key", rest_.api_key);
      rest_.timeout_ms = r.value("timeout_ms", rest_.timeout_ms);
      rest_.retry_count = r.value("retry_count", rest_.retry_count);
      rest_.ssl_enable = r.value("ssl_enable", rest_.ssl_enable);
    }
    if (patch.contains("serial")) {
      const auto& s = patch["serial"];
      serial_.port = s.value("port", serial_.port);
      serial_.baudrate = s.value("baudrate", serial_.baudrate);
      serial_.data_bits = s.value("data_bits", serial_.data_bits);
      serial_.stop_bits = s.value("stop_bits", serial_.stop_bits);
      if (s.contains("parity")) {
        std::string parity = s["parity"].get<std::string>();
        serial_.parity = parity.empty() ? serial_.parity : parity[0];
      }
    }
    if (patch.contains("serial2")) {
      const auto& s = patch["serial2"];
      serial2_.port = s.value("port", serial2_.port);
      serial2_.baudrate = s.value("baudrate", serial2_.baudrate);
      serial2_.data_bits = s.value("data_bits", serial2_.data_bits);
      serial2_.stop_bits = s.value("stop_bits", serial2_.stop_bits);
      if (s.contains("parity")) {
        std::string parity = s["parity"].get<std::string>();
        serial2_.parity = parity.empty() ? serial2_.parity : parity[0];
      }
    }
    if (patch.contains("modbus")) {
      const auto& m = patch["modbus"];
      modbus_.enabled = m.value("enabled", modbus_.enabled);
      modbus_.ip = m.value("ip", modbus_.ip);
      modbus_.port = m.value("port", modbus_.port);
      modbus_.input_coil_start = m.value("input_coil_start", modbus_.input_coil_start);
      modbus_.input_coil_count = m.value("input_coil_count", modbus_.input_coil_count);
      modbus_.output_coil_start = m.value("output_coil_start", modbus_.output_coil_start);
      modbus_.output_coil_count = m.value("output_coil_count", modbus_.output_coil_count);
      modbus_.holding_register_start = m.value("holding_register_start", modbus_.holding_register_start);
      modbus_.holding_register_count = m.value("holding_register_count", modbus_.holding_register_count);
      modbus_.slave_enabled = m.value("slave_enabled", modbus_.slave_enabled);
      modbus_.slave_bind = m.value("slave_bind", modbus_.slave_bind);
      modbus_.slave_port = m.value("slave_port", modbus_.slave_port);
      modbus_.slave_unit_id = m.value("slave_unit_id", modbus_.slave_unit_id);
      modbus_.discrete_input_start = m.value("discrete_input_start", modbus_.discrete_input_start);
      modbus_.discrete_input_count = m.value("discrete_input_count", modbus_.discrete_input_count);
      modbus_.input_register_start = m.value("input_register_start", modbus_.input_register_start);
      modbus_.input_register_count = m.value("input_register_count", modbus_.input_register_count);
      modbus_.rtu_port = m.value("rtu_port", modbus_.rtu_port);
      modbus_.rtu_master_enabled = m.value("rtu_master_enabled", modbus_.rtu_master_enabled);
      modbus_.rtu_baudrate = m.value("rtu_baudrate", modbus_.rtu_baudrate);
      modbus_.rtu_data_bits = m.value("rtu_data_bits", modbus_.rtu_data_bits);
      modbus_.rtu_stop_bits = m.value("rtu_stop_bits", modbus_.rtu_stop_bits);
      std::string rtuParity = m.value("rtu_parity", std::string(1, modbus_.rtu_parity));
      modbus_.rtu_parity = rtuParity.empty() ? 'N' : rtuParity[0];
      modbus_.rtu_unit_id = m.value("rtu_unit_id", modbus_.rtu_unit_id);
      modbus_.rtu_slave_enabled = m.value("rtu_slave_enabled", modbus_.rtu_slave_enabled);
      modbus_.rtu_slave_id = m.value("rtu_slave_id", modbus_.rtu_slave_id);
      modbus_.poll_interval_ms = m.value("poll_interval_ms", modbus_.poll_interval_ms);
    }
    if (patch.contains("rfid")) {
      const auto& r = patch["rfid"];
      rfid_.mode = r.value("mode", rfid_.mode);
      rfid_.ip = r.value("ip", rfid_.ip);
      rfid_.port = r.value("port", rfid_.port);
      rfid_.reconnect_interval_ms = r.value("reconnect_interval_ms", rfid_.reconnect_interval_ms);
      rfid_.timeout_ms = r.value("timeout_ms", rfid_.timeout_ms);
    }
    if (patch.contains("zk")) {
      const auto& z = patch["zk"];
      zk_.enabled = z.value("enabled", zk_.enabled);
      zk_.ip = z.value("ip", zk_.ip);
      zk_.port = z.value("port", zk_.port);
      zk_.timeout_ms = z.value("timeout_ms", zk_.timeout_ms);
      zk_.password = z.value("password", zk_.password);
      zk_.backend = z.value("backend", zk_.backend);
    }
    if (patch.contains("mq")) {
      const auto& q = patch["mq"];
      mq_.enabled = q.value("enabled", mq_.enabled);
      mq_.host = q.value("host", mq_.host);
      mq_.port = q.value("port", mq_.port);
      mq_.vhost = q.value("vhost", mq_.vhost);
      mq_.user = q.value("user", mq_.user);
      mq_.password = q.value("password", mq_.password);
      mq_.exchange = q.value("exchange", mq_.exchange);
      mq_.exchange_type = q.value("exchange_type", mq_.exchange_type);
      mq_.queue = q.value("queue", mq_.queue);
      mq_.routing_key = q.value("routing_key", mq_.routing_key);
      mq_.declare_topology = q.value("declare_topology", mq_.declare_topology);
      mq_.consume = q.value("consume", mq_.consume);
      mq_.prefetch = q.value("prefetch", mq_.prefetch);
      mq_.envelope = q.value("envelope", mq_.envelope);
      mq_.heartbeat_sec = q.value("heartbeat_sec", mq_.heartbeat_sec);
      mq_.reconnect_interval_ms = q.value("reconnect_interval_ms", mq_.reconnect_interval_ms);
      mq_.publish_queue_limit = q.value("publish_queue_limit", mq_.publish_queue_limit);
      mq_.inbox_limit = q.value("inbox_limit", mq_.inbox_limit);
    }
    if (patch.contains("lua")) {
      lua_.script_path = patch["lua"].value("script_path", lua_.script_path);
    }
    if (patch.contains("card_api")) {
      cardApi_.enabled = patch["card_api"].value("enabled", cardApi_.enabled);
    }
    if (patch.contains("update")) {
      const auto& u = patch["update"];
      update_.enabled = u.value("enabled", update_.enabled);
      update_.url = u.value("url", update_.url);
      update_.channel = u.value("channel", update_.channel);
      update_.check_interval_hours = u.value("check_interval_hours", update_.check_interval_hours);
      update_.initial_delay_sec = u.value("initial_delay_sec", update_.initial_delay_sec);
      update_.require_signature = u.value("require_signature", update_.require_signature);
      update_.public_key_path = u.value("public_key_path", update_.public_key_path);
      update_.ssl_verify = u.value("ssl_verify", update_.ssl_verify);
      update_.timeout_sec = u.value("timeout_sec", update_.timeout_sec);
      update_.install_root = u.value("install_root", update_.install_root);
      update_.health_confirm_sec = u.value("health_confirm_sec", update_.health_confirm_sec);
      update_.max_boot_attempts = u.value("max_boot_attempts", update_.max_boot_attempts);
      update_.keep_releases = u.value("keep_releases", update_.keep_releases);
      update_.restart_command = u.value("restart_command", update_.restart_command);
    }
    if (patch.contains("auth")) {
      const auto& a = patch["auth"];
      auth_.enabled = a.value("enabled", auth_.enabled);
      auth_.token_lifetime_sec = a.value("token_lifetime_sec", auth_.token_lifetime_sec);
      auth_.max_sessions_per_user = a.value("max_sessions_per_user", auth_.max_sessions_per_user);
      auth_.max_failed_attempts = a.value("max_failed_attempts", auth_.max_failed_attempts);
      auth_.lockout_seconds = a.value("lockout_seconds", auth_.lockout_seconds);
      auth_.rate_limit_enabled = a.value("rate_limit_enabled", auth_.rate_limit_enabled);
      auth_.login_rate_per_sec = a.value("login_rate_per_sec", auth_.login_rate_per_sec);
      auth_.login_burst = a.value("login_burst", auth_.login_burst);
      auth_.api_rate_per_sec = a.value("api_rate_per_sec", auth_.api_rate_per_sec);
      auth_.api_burst = a.value("api_burst", auth_.api_burst);
      auth_.max_body_bytes = a.value("max_body_bytes", auth_.max_body_bytes);
      auth_.max_lua_body_bytes = a.value("max_lua_body_bytes", auth_.max_lua_body_bytes);
      auth_.audit_log = a.value("audit_log", auth_.audit_log);
      auth_.audit_retention_days = a.value("audit_retention_days", auth_.audit_retention_days);
      auth_.security_headers = a.value("security_headers", auth_.security_headers);
      auth_.allowed_origins = a.value("allowed_origins", auth_.allowed_origins);
      auth_.public_app_routes = a.value("public_app_routes", auth_.public_app_routes);
      auth_.protect_locker_ui = a.value("protect_locker_ui", auth_.protect_locker_ui);
    }
    if (patch.contains("plugins") && patch["plugins"].is_object()) {
      plugins_ = patch["plugins"];
    }
  } catch (const std::exception& e) {
    Logger::Instance().Error(LogCategory::System, std::string("ConfigManager: ApplyJson failed: ") + e.what());
    return false;
  }
  return true;
}

nlohmann::json ConfigManager::ToJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ToJsonUnlocked();
}

nlohmann::json ConfigManager::ToJsonUnlocked() const {
  json root;
  root["system"] = {{"machine_id", system_.machine_id}, {"device_name", system_.device_name}};
  root["web"] = {{"port", web_.port},
                  {"bind_address", web_.bind_address},
                  {"locker_ui_enabled", web_.locker_ui_enabled},
                  {"locker_ui_port", web_.locker_ui_port},
                  {"locker_db_path", web_.locker_db_path}};
  root["rest"] = {{"url", rest_.url},
                   {"api_key", rest_.api_key},
                   {"timeout_ms", rest_.timeout_ms},
                   {"retry_count", rest_.retry_count},
                   {"ssl_enable", rest_.ssl_enable}};
  root["serial"] = {{"port", serial_.port},
                     {"baudrate", serial_.baudrate},
                     {"data_bits", serial_.data_bits},
                     {"stop_bits", serial_.stop_bits},
                     {"parity", std::string(1, serial_.parity)}};
  root["serial2"] = {{"port", serial2_.port},
                      {"baudrate", serial2_.baudrate},
                      {"data_bits", serial2_.data_bits},
                      {"stop_bits", serial2_.stop_bits},
                      {"parity", std::string(1, serial2_.parity)}};
  root["modbus"] = {{"enabled", modbus_.enabled},
                     {"ip", modbus_.ip},
                     {"port", modbus_.port},
                     {"input_coil_start", modbus_.input_coil_start},
                     {"input_coil_count", modbus_.input_coil_count},
                     {"output_coil_start", modbus_.output_coil_start},
                     {"output_coil_count", modbus_.output_coil_count},
                     {"holding_register_start", modbus_.holding_register_start},
                     {"holding_register_count", modbus_.holding_register_count},
                     {"slave_enabled", modbus_.slave_enabled},
                     {"slave_bind", modbus_.slave_bind},
                     {"slave_port", modbus_.slave_port},
                     {"slave_unit_id", modbus_.slave_unit_id},
                     {"discrete_input_start", modbus_.discrete_input_start},
                     {"discrete_input_count", modbus_.discrete_input_count},
                     {"input_register_start", modbus_.input_register_start},
                     {"input_register_count", modbus_.input_register_count},
                     {"rtu_port", modbus_.rtu_port},
                     {"rtu_master_enabled", modbus_.rtu_master_enabled},
                     {"rtu_baudrate", modbus_.rtu_baudrate},
                     {"rtu_data_bits", modbus_.rtu_data_bits},
                     {"rtu_stop_bits", modbus_.rtu_stop_bits},
                     {"rtu_parity", std::string(1, modbus_.rtu_parity)},
                     {"rtu_unit_id", modbus_.rtu_unit_id},
                     {"rtu_slave_enabled", modbus_.rtu_slave_enabled},
                     {"rtu_slave_id", modbus_.rtu_slave_id},
                     {"poll_interval_ms", modbus_.poll_interval_ms}};
  root["rfid"] = {{"mode", rfid_.mode},
                  {"ip", rfid_.ip},
                  {"port", rfid_.port},
                  {"reconnect_interval_ms", rfid_.reconnect_interval_ms},
                  {"timeout_ms", rfid_.timeout_ms}};
  root["zk"] = {{"enabled", zk_.enabled},
                {"ip", zk_.ip},
                {"port", zk_.port},
                {"timeout_ms", zk_.timeout_ms},
                {"password", zk_.password},
                {"backend", zk_.backend}};
  root["mq"] = {{"enabled", mq_.enabled},
                {"host", mq_.host},
                {"port", mq_.port},
                {"vhost", mq_.vhost},
                {"user", mq_.user},
                {"password", mq_.password},
                {"exchange", mq_.exchange},
                {"exchange_type", mq_.exchange_type},
                {"queue", mq_.queue},
                {"routing_key", mq_.routing_key},
                {"declare_topology", mq_.declare_topology},
                {"consume", mq_.consume},
                {"prefetch", mq_.prefetch},
                {"envelope", mq_.envelope},
                {"heartbeat_sec", mq_.heartbeat_sec},
                {"reconnect_interval_ms", mq_.reconnect_interval_ms},
                {"publish_queue_limit", mq_.publish_queue_limit},
                {"inbox_limit", mq_.inbox_limit}};
  root["lua"] = {{"script_path", lua_.script_path}};
  root["logging"] = {{"store_enabled", logging_.store_enabled},
                      {"retention_days", logging_.retention_days},
                      {"definitions_path", logging_.definitions_path},
                      {"debug_enabled", logging_.debug_enabled}};
  root["card_api"] = {{"enabled", cardApi_.enabled}};
  root["update"] = {{"enabled", update_.enabled},
                    {"url", update_.url},
                    {"channel", update_.channel},
                    {"check_interval_hours", update_.check_interval_hours},
                    {"initial_delay_sec", update_.initial_delay_sec},
                    {"require_signature", update_.require_signature},
                    {"public_key_path", update_.public_key_path},
                    {"ssl_verify", update_.ssl_verify},
                    {"timeout_sec", update_.timeout_sec},
                    {"install_root", update_.install_root},
                    {"health_confirm_sec", update_.health_confirm_sec},
                    {"max_boot_attempts", update_.max_boot_attempts},
                    {"keep_releases", update_.keep_releases},
                    {"restart_command", update_.restart_command}};
  root["auth"] = {{"enabled", auth_.enabled},
                  {"token_lifetime_sec", auth_.token_lifetime_sec},
                  {"max_sessions_per_user", auth_.max_sessions_per_user},
                  {"max_failed_attempts", auth_.max_failed_attempts},
                  {"lockout_seconds", auth_.lockout_seconds},
                  {"rate_limit_enabled", auth_.rate_limit_enabled},
                  {"login_rate_per_sec", auth_.login_rate_per_sec},
                  {"login_burst", auth_.login_burst},
                  {"api_rate_per_sec", auth_.api_rate_per_sec},
                  {"api_burst", auth_.api_burst},
                  {"max_body_bytes", auth_.max_body_bytes},
                  {"max_lua_body_bytes", auth_.max_lua_body_bytes},
                  {"audit_log", auth_.audit_log},
                  {"audit_retention_days", auth_.audit_retention_days},
                  {"security_headers", auth_.security_headers},
                  {"allowed_origins", auth_.allowed_origins},
                  {"public_app_routes", auth_.public_app_routes},
                  {"protect_locker_ui", auth_.protect_locker_ui}};
  root["plugins"] = plugins_;
  return root;
}

// --- dotted-path access ---------------------------------------------------

namespace {

// Splits "modbus.ip" into ("modbus", "ip"). Returns false for anything that
// isn't exactly two non-empty parts: the configuration document is two levels
// deep by construction, and accepting "modbus" or "a.b.c" here would make
// Config.Get() fail in ways a script author can't tell apart from a typo.
bool SplitPath(const std::string& path, std::string& section, std::string& key) {
  auto dot = path.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= path.size()) return false;
  if (path.find('.', dot + 1) != std::string::npos) return false;
  section = path.substr(0, dot);
  key = path.substr(dot + 1);
  return true;
}

}  // namespace

bool ConfigManager::GetValue(const std::string& path, nlohmann::json& out) const {
  std::string section, key;
  if (!SplitPath(path, section, key)) return false;

  json root = ToJson();
  auto sectionIt = root.find(section);
  if (sectionIt == root.end() || !sectionIt->is_object()) return false;
  auto keyIt = sectionIt->find(key);
  if (keyIt == sectionIt->end()) return false;

  out = *keyIt;
  return true;
}

bool ConfigManager::HasValue(const std::string& path) const {
  json ignored;
  return GetValue(path, ignored);
}

nlohmann::json ConfigManager::GetCategory(const std::string& category) const {
  json root = ToJson();
  auto it = root.find(category);
  if (it == root.end()) return json(nullptr);
  return *it;
}

bool ConfigManager::SetValue(const std::string& path, const nlohmann::json& value, std::string& error) {
  std::string section, key;
  if (!SplitPath(path, section, key)) {
    error = "expected a \"<section>.<key>\" path, got \"" + path + "\"";
    return false;
  }

  // Refuse to create new keys. ApplyJson only copies fields it recognises,
  // so an unknown key would be accepted here, dropped there, and reported as
  // success -- the script would then read back the old value and have no way
  // to tell why.
  json existing;
  if (!GetValue(path, existing)) {
    error = "unknown configuration key \"" + path + "\"";
    return false;
  }

  // Same reasoning for types: ApplyJson's value<T>() calls throw on a
  // mismatch, which ApplyJson turns into a blanket failure for the whole
  // patch. Checking first gives the script the actual reason.
  auto category = [](const json& v) {
    if (v.is_boolean()) return "boolean";
    if (v.is_number()) return "number";
    if (v.is_string()) return "string";
    return "other";
  };
  if (std::string(category(existing)) != category(value)) {
    error = "configuration key \"" + path + "\" is a " + category(existing) + ", not a " + category(value);
    return false;
  }

  json patch;
  patch[section][key] = value;
  if (!ApplyJson(patch)) {
    error = "failed to apply " + path;
    return false;
  }
  if (!Save()) {
    error = "failed to persist " + path;
    return false;
  }
  return true;
}

}  // namespace hsf
