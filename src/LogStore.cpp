#include "hsf/LogStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "hsf/Logger.h"

namespace hsf {

using nlohmann::json;

namespace {

// Same local-time format the runtime Logger stamps its entries with, minus
// the milliseconds. It matters that this is local and not SQLite's
// CURRENT_TIMESTAMP (which is UTC): the Logs page's date pickers hand back
// local wall-clock strings, and the range filter compares them to this column
// lexicographically. Mixing the two would quietly shift every search by the
// machine's UTC offset.
std::string NowTimestamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &t);
#else
  localtime_r(&t, &tmBuf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string UpperCase(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

bool IsKnownFieldType(const std::string& type) {
  return type == "string" || type == "integer" || type == "number" || type == "boolean";
}

// Checks a value against its declared type. Values arrive as strings (Lua
// numbers and booleans are stringified at the binding), so this is a parse
// check rather than a type-tag comparison.
bool ValueMatchesType(const std::string& type, const std::string& value, std::string& error) {
  if (type == "string") return true;

  if (type == "boolean") {
    std::string v = UpperCase(value);
    if (v == "TRUE" || v == "FALSE" || v == "1" || v == "0") return true;
    error = "expected a boolean, got \"" + value + "\"";
    return false;
  }

  // Empty is not a number, and strtod/strtoll would happily accept "12abc"
  // by stopping at the first bad character -- require the whole string to be
  // consumed.
  if (value.empty()) {
    error = "expected " + type + ", got an empty value";
    return false;
  }
  const char* begin = value.c_str();
  char* end = nullptr;
  if (type == "integer") {
    std::strtoll(begin, &end, 10);
  } else {
    std::strtod(begin, &end);
  }
  if (end && *end == '\0') return true;
  error = "expected " + type + ", got \"" + value + "\"";
  return false;
}

bool Exec(sqlite3* db, const char* sql) {
  char* errMsg = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) == SQLITE_OK) return true;
  Logger::Instance().Error(LogCategory::System,
                            std::string("LogStore: ") + (errMsg ? errMsg : "unknown error"));
  sqlite3_free(errMsg);
  return false;
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
  return text ? text : "";
}

// Escapes the LIKE wildcards in a user-supplied keyword so searching for
// "100%" doesn't match everything. Paired with ESCAPE '\' in the SQL.
std::string EscapeLike(const std::string& term) {
  std::string out;
  out.reserve(term.size() + 8);
  for (char c : term) {
    if (c == '%' || c == '_' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

const LogFieldDef* LogTypeDef::Find(const std::string& fieldName) const {
  for (const auto& field : fields) {
    if (field.name == fieldName) return &field;
  }
  return nullptr;
}

LogStore& LogStore::Instance() {
  static LogStore instance;
  return instance;
}

LogStore::~LogStore() {
  if (db_) sqlite3_close(db_);
}

bool LogStore::Load(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System, "LogStore: failed to open " + path + ": " +
                                                      (db ? sqlite3_errmsg(db) : "unknown error"));
    if (db) sqlite3_close(db);
    return false;
  }

  if (db_) sqlite3_close(db_);
  db_ = db;
  path_ = path;

  // Writes come from Lua script threads while the web server reads on Crow's
  // threads; WAL is what keeps a search from blocking a card being logged.
  Exec(db_, "PRAGMA journal_mode=WAL;");
  Exec(db_, "PRAGMA foreign_keys=ON;");

  if (!EnsureSchemaUnlocked()) return false;
  LoadDefinitionsFromDbUnlocked();
  return true;
}

bool LogStore::IsOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return db_ != nullptr;
}

bool LogStore::EnsureSchemaUnlocked() {
  // Schema straight from request/upgrade.md section 9, plus the indexes the
  // section 14 filters need -- a keyword search over log_values on a gateway
  // with months of card history is a table scan otherwise.
  static const char* kSchema =
      "CREATE TABLE IF NOT EXISTS log_definitions ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  log_type TEXT UNIQUE NOT NULL,"
      "  description TEXT,"
      "  definition_json TEXT NOT NULL,"
      "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
      "CREATE TABLE IF NOT EXISTS log_entries ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  log_type TEXT NOT NULL,"
      "  script_name TEXT,"
      "  level TEXT,"
      "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);"
      "CREATE TABLE IF NOT EXISTS log_values ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  log_id INTEGER NOT NULL,"
      "  field_name TEXT NOT NULL,"
      "  field_value TEXT,"
      "  FOREIGN KEY(log_id) REFERENCES log_entries(id) ON DELETE CASCADE);"
      "CREATE INDEX IF NOT EXISTS idx_log_entries_time ON log_entries(timestamp);"
      "CREATE INDEX IF NOT EXISTS idx_log_entries_type ON log_entries(log_type);"
      "CREATE INDEX IF NOT EXISTS idx_log_entries_script ON log_entries(script_name);"
      "CREATE INDEX IF NOT EXISTS idx_log_values_log ON log_values(log_id);"
      "CREATE INDEX IF NOT EXISTS idx_log_values_value ON log_values(field_value);";
  return Exec(db_, kSchema);
}

bool LogStore::LoadDefinitionsFromDbUnlocked() {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT log_type, description, definition_json FROM log_definitions;", -1,
                          &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  definitions_.clear();
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    LogTypeDef def;
    def.type = ColumnText(stmt, 0);
    def.description = ColumnText(stmt, 1);
    try {
      json fields = json::parse(ColumnText(stmt, 2));
      if (fields.is_array()) {
        for (const auto& field : fields) {
          LogFieldDef f;
          f.name = field.value("name", "");
          f.type = field.value("type", std::string("string"));
          f.required = field.value("required", false);
          if (!f.name.empty()) def.fields.push_back(f);
        }
      }
    } catch (const std::exception& e) {
      Logger::Instance().Error(LogCategory::System,
                                "LogStore: stored definition for '" + def.type + "' is not valid JSON: " + e.what());
      continue;
    }
    definitions_[def.type] = def;
  }
  sqlite3_finalize(stmt);
  return true;
}

bool LogStore::UpsertDefinitionUnlocked(const LogTypeDef& def) {
  json fields = json::array();
  for (const auto& field : def.fields) {
    fields.push_back({{"name", field.name}, {"type", field.type}, {"required", field.required}});
  }
  std::string dump = fields.dump();

  static const char* kSql =
      "INSERT INTO log_definitions(log_type, description, definition_json) VALUES(?, ?, ?) "
      "ON CONFLICT(log_type) DO UPDATE SET description = excluded.description, "
      "definition_json = excluded.definition_json;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("LogStore: prepare failed: ") + sqlite3_errmsg(db_));
    return false;
  }
  sqlite3_bind_text(stmt, 1, def.type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, def.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, dump.c_str(), -1, SQLITE_TRANSIENT);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok) {
    Logger::Instance().Error(LogCategory::System, std::string("LogStore: failed to store definition '") +
                                                      def.type + "': " + sqlite3_errmsg(db_));
  }
  sqlite3_finalize(stmt);
  if (ok) definitions_[def.type] = def;
  return ok;
}

bool LogStore::DefineType(const LogTypeDef& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  return UpsertDefinitionUnlocked(def);
}

bool LogStore::LoadDefinitions(const std::string& jsonPath) {
  std::ifstream in(jsonPath);
  if (!in.is_open()) {
    Logger::Instance().Warning(LogCategory::System,
                                "LogStore: no log definitions file at " + jsonPath +
                                    "; Log.Write() will only accept built-in types");
    return false;
  }

  json root;
  try {
    in >> root;
  } catch (const std::exception& e) {
    Logger::Instance().Error(LogCategory::System,
                              "LogStore: failed to parse " + jsonPath + ": " + e.what());
    return false;
  }

  // Accepts both the section 8 shape ({"logs": [...]}) and a bare array, so a
  // hand-written file that skipped the wrapper still loads.
  const json* logs = nullptr;
  if (root.is_object() && root.contains("logs") && root["logs"].is_array()) {
    logs = &root["logs"];
  } else if (root.is_array()) {
    logs = &root;
  } else {
    Logger::Instance().Error(LogCategory::System,
                              "LogStore: " + jsonPath + " must contain a \"logs\" array");
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;

  int loaded = 0;
  for (const auto& item : *logs) {
    if (!item.is_object()) continue;

    LogTypeDef def;
    def.type = item.value("type", "");
    def.description = item.value("description", "");
    if (def.type.empty()) {
      Logger::Instance().Error(LogCategory::System, "LogStore: skipping a log definition with no \"type\"");
      continue;
    }

    bool valid = true;
    if (item.contains("fields") && item["fields"].is_array()) {
      for (const auto& field : item["fields"]) {
        LogFieldDef f;
        f.name = field.value("name", "");
        f.type = field.value("type", std::string("string"));
        f.required = field.value("required", false);
        if (f.name.empty()) {
          Logger::Instance().Error(LogCategory::System,
                                    "LogStore: log type '" + def.type + "' has a field with no name");
          valid = false;
          break;
        }
        if (!IsKnownFieldType(f.type)) {
          // Caught here rather than at Write() time: a typo like "str" would
          // otherwise sit in the file until the one script that writes that
          // field runs, and then fail as a validation error nobody expects.
          Logger::Instance().Error(LogCategory::System,
                                    "LogStore: log type '" + def.type + "' field '" + f.name +
                                        "' has unknown type '" + f.type +
                                        "' (use string, integer, number or boolean)");
          valid = false;
          break;
        }
        def.fields.push_back(f);
      }
    }
    if (!valid) continue;

    if (UpsertDefinitionUnlocked(def)) ++loaded;
  }

  Logger::Instance().Info(LogCategory::System, "LogStore: loaded " + std::to_string(loaded) +
                                                   " log definition(s) from " + jsonPath);
  return true;
}

std::vector<LogTypeDef> LogStore::Definitions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogTypeDef> out;
  out.reserve(definitions_.size());
  for (const auto& [type, def] : definitions_) out.push_back(def);
  return out;
}

bool LogStore::FindDefinition(const std::string& logType, LogTypeDef& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = definitions_.find(logType);
  if (it == definitions_.end()) return false;
  out = it->second;
  return true;
}

bool LogStore::Write(const std::string& logType, const std::string& scriptName, const std::string& level,
                     const std::map<std::string, std::string>& fields, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) {
    error = "log store is not open";
    return false;
  }

  // Section 11, steps 1-4: find the definition, then validate the type, the
  // required fields and the field types -- all before touching the database,
  // so a rejected write leaves nothing behind.
  auto defIt = definitions_.find(logType);
  if (defIt == definitions_.end()) {
    error = "unknown log type \"" + logType + "\"";
    return false;
  }
  const LogTypeDef& def = defIt->second;

  for (const auto& [name, value] : fields) {
    const LogFieldDef* field = def.Find(name);
    if (!field) {
      error = "log type \"" + logType + "\" has no field \"" + name + "\"";
      return false;
    }
    std::string typeError;
    if (!ValueMatchesType(field->type, value, typeError)) {
      error = "field \"" + name + "\": " + typeError;
      return false;
    }
  }
  for (const auto& field : def.fields) {
    if (!field.required) continue;
    auto it = fields.find(field.name);
    if (it == fields.end() || it->second.empty()) {
      error = "log type \"" + logType + "\" requires field \"" + field.name + "\"";
      return false;
    }
  }

  // Steps 5-6 in one transaction: an entry with only some of its values
  // written would show up in the Logs page as a row with holes in it, and
  // there would be no way to tell that from a script that genuinely omitted
  // those optional fields.
  if (!Exec(db_, "BEGIN IMMEDIATE;")) {
    error = "failed to begin transaction";
    return false;
  }

  std::string timestamp = NowTimestamp();
  std::string storedLevel = level.empty() ? "INFO" : UpperCase(level);

  sqlite3_stmt* entryStmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                          "INSERT INTO log_entries(log_type, script_name, level, timestamp) VALUES(?, ?, ?, ?);",
                          -1, &entryStmt, nullptr) != SQLITE_OK) {
    error = sqlite3_errmsg(db_);
    Exec(db_, "ROLLBACK;");
    return false;
  }
  sqlite3_bind_text(entryStmt, 1, logType.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(entryStmt, 2, scriptName.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(entryStmt, 3, storedLevel.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(entryStmt, 4, timestamp.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(entryStmt) == SQLITE_DONE;
  if (!ok) error = sqlite3_errmsg(db_);
  sqlite3_finalize(entryStmt);

  if (!ok) {
    Exec(db_, "ROLLBACK;");
    return false;
  }

  int64_t logId = sqlite3_last_insert_rowid(db_);

  sqlite3_stmt* valueStmt = nullptr;
  if (sqlite3_prepare_v2(db_, "INSERT INTO log_values(log_id, field_name, field_value) VALUES(?, ?, ?);", -1,
                          &valueStmt, nullptr) != SQLITE_OK) {
    error = sqlite3_errmsg(db_);
    Exec(db_, "ROLLBACK;");
    return false;
  }
  for (const auto& [name, value] : fields) {
    sqlite3_reset(valueStmt);
    sqlite3_bind_int64(valueStmt, 1, logId);
    sqlite3_bind_text(valueStmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(valueStmt, 3, value.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(valueStmt) != SQLITE_DONE) {
      error = sqlite3_errmsg(db_);
      ok = false;
      break;
    }
  }
  sqlite3_finalize(valueStmt);

  Exec(db_, ok ? "COMMIT;" : "ROLLBACK;");
  return ok;
}

nlohmann::json LogStore::Query(const LogQuery& query) const {
  std::lock_guard<std::mutex> lock(mutex_);
  json result = {{"total", 0}, {"limit", query.limit}, {"offset", query.offset}, {"entries", json::array()}};
  if (!db_) return result;

  // Built as a parameterised WHERE clause with a parallel list of bindings,
  // so nothing from a request URL is ever concatenated into SQL.
  std::string where = " WHERE 1=1";
  std::vector<std::string> bindings;

  if (!query.types.empty()) {
    where += " AND e.log_type IN (";
    for (size_t i = 0; i < query.types.size(); ++i) {
      where += i == 0 ? "?" : ",?";
      bindings.push_back(query.types[i]);
    }
    where += ")";
  }
  if (!query.level.empty() && query.level != "all") {
    where += " AND e.level = ?";
    bindings.push_back(UpperCase(query.level));
  }
  if (!query.script.empty()) {
    where += " AND e.script_name = ?";
    bindings.push_back(query.script);
  }
  if (!query.from.empty()) {
    where += " AND e.timestamp >= ?";
    bindings.push_back(query.from);
  }
  if (!query.to.empty()) {
    // The pickers hand back minute precision ("2026-08-11 23:59"), which as a
    // string compares BELOW every second within that minute -- ":59:30" would
    // fall outside a range the operator plainly meant to include. Widening a
    // minute-precision bound to the end of that minute is the fix.
    std::string upper = query.to;
    if (upper.size() == 16) upper += ":59";
    where += " AND e.timestamp <= ?";
    bindings.push_back(upper);
  }
  if (!query.keyword.empty()) {
    // Matches any field value, the script name or the log type: an operator
    // searching for "NGUYEN VAN A" (section 14's own example) has no reason to
    // know which field holds it.
    where +=
        " AND (e.script_name LIKE ? ESCAPE '\\' OR e.log_type LIKE ? ESCAPE '\\'"
        " OR EXISTS (SELECT 1 FROM log_values v WHERE v.log_id = e.id"
        "            AND v.field_value LIKE ? ESCAPE '\\'))";
    std::string pattern = "%" + EscapeLike(query.keyword) + "%";
    bindings.push_back(pattern);
    bindings.push_back(pattern);
    bindings.push_back(pattern);
  }

  auto bindAll = [&bindings](sqlite3_stmt* stmt) {
    for (size_t i = 0; i < bindings.size(); ++i) {
      sqlite3_bind_text(stmt, static_cast<int>(i + 1), bindings[i].c_str(), -1, SQLITE_TRANSIENT);
    }
  };

  // Total first: the pager needs the match count, not the page size.
  std::string countSql = "SELECT COUNT(*) FROM log_entries e" + where + ";";
  sqlite3_stmt* countStmt = nullptr;
  if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &countStmt, nullptr) == SQLITE_OK) {
    bindAll(countStmt);
    if (sqlite3_step(countStmt) == SQLITE_ROW) {
      result["total"] = sqlite3_column_int64(countStmt, 0);
    }
    sqlite3_finalize(countStmt);
  }

  int limit = query.limit > 0 ? query.limit : 50;
  if (limit > 500) limit = 500;  // section 16: never hand the whole table to a browser
  int offset = query.offset > 0 ? query.offset : 0;

  std::string sql = "SELECT e.id, e.timestamp, e.log_type, e.script_name, e.level FROM log_entries e" + where +
                     " ORDER BY e.id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("LogStore: query failed: ") + sqlite3_errmsg(db_));
    return result;
  }
  bindAll(stmt);
  sqlite3_bind_int(stmt, static_cast<int>(bindings.size() + 1), limit);
  sqlite3_bind_int(stmt, static_cast<int>(bindings.size() + 2), offset);

  std::vector<int64_t> ids;
  json entries = json::array();
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(stmt, 0);
    ids.push_back(id);
    entries.push_back({{"id", id},
                        {"timestamp", ColumnText(stmt, 1)},
                        {"log_type", ColumnText(stmt, 2)},
                        {"script_name", ColumnText(stmt, 3)},
                        {"level", ColumnText(stmt, 4)},
                        {"fields", json::object()}});
  }
  sqlite3_finalize(stmt);

  // Values for the whole page in one statement rather than one query per row:
  // the result table shows dynamic fields as columns (section 15), so every
  // row needs its values anyway.
  if (!ids.empty()) {
    std::string valueSql = "SELECT log_id, field_name, field_value FROM log_values WHERE log_id IN (";
    for (size_t i = 0; i < ids.size(); ++i) valueSql += i == 0 ? "?" : ",?";
    valueSql += ");";

    sqlite3_stmt* valueStmt = nullptr;
    if (sqlite3_prepare_v2(db_, valueSql.c_str(), -1, &valueStmt, nullptr) == SQLITE_OK) {
      for (size_t i = 0; i < ids.size(); ++i) {
        sqlite3_bind_int64(valueStmt, static_cast<int>(i + 1), ids[i]);
      }
      std::map<int64_t, json> byId;
      while (sqlite3_step(valueStmt) == SQLITE_ROW) {
        byId[sqlite3_column_int64(valueStmt, 0)][ColumnText(valueStmt, 1)] = ColumnText(valueStmt, 2);
      }
      sqlite3_finalize(valueStmt);

      for (auto& entry : entries) {
        auto it = byId.find(entry["id"].get<int64_t>());
        if (it != byId.end()) entry["fields"] = it->second;
      }
    }
  }

  result["entries"] = entries;
  result["limit"] = limit;
  result["offset"] = offset;
  return result;
}

nlohmann::json LogStore::Entry(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return json(nullptr);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT id, timestamp, log_type, script_name, level FROM log_entries WHERE id = ?;",
                          -1, &stmt, nullptr) != SQLITE_OK) {
    return json(nullptr);
  }
  sqlite3_bind_int64(stmt, 1, id);

  json entry(nullptr);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    entry = {{"id", sqlite3_column_int64(stmt, 0)},
              {"timestamp", ColumnText(stmt, 1)},
              {"log_type", ColumnText(stmt, 2)},
              {"script_name", ColumnText(stmt, 3)},
              {"level", ColumnText(stmt, 4)},
              {"fields", json::object()}};
  }
  sqlite3_finalize(stmt);
  if (entry.is_null()) return entry;

  // Field order follows the definition, not the database, so the detail view
  // reads in the order the definitions file declares (section 15's example
  // lists Citizen ID before Employee before Card UID -- alphabetical or
  // insertion order would scramble that).
  json values = json::object();
  sqlite3_stmt* valueStmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT field_name, field_value FROM log_values WHERE log_id = ?;", -1,
                          &valueStmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(valueStmt, 1, id);
    while (sqlite3_step(valueStmt) == SQLITE_ROW) {
      values[ColumnText(valueStmt, 0)] = ColumnText(valueStmt, 1);
    }
    sqlite3_finalize(valueStmt);
  }
  entry["fields"] = values;

  json order = json::array();
  auto defIt = definitions_.find(entry["log_type"].get<std::string>());
  if (defIt != definitions_.end()) {
    for (const auto& field : defIt->second.fields) order.push_back(field.name);
  }
  entry["field_order"] = order;
  return entry;
}

std::vector<std::string> LogStore::Scripts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  if (!db_) return out;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                          "SELECT DISTINCT script_name FROM log_entries WHERE script_name IS NOT NULL "
                          "AND script_name <> '' ORDER BY script_name;",
                          -1, &stmt, nullptr) != SQLITE_OK) {
    return out;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ColumnText(stmt, 0));
  sqlite3_finalize(stmt);
  return out;
}

int64_t LogStore::PruneOlderThan(int days) {
  if (days <= 0) return 0;

  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;

  // Cut-off computed here rather than with SQLite's date() so it lands in the
  // same local-time frame the timestamp column is written in.
  auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * days);
  std::time_t t = std::chrono::system_clock::to_time_t(cutoff);
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &t);
#else
  localtime_r(&t, &tmBuf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
  std::string cutoffText = oss.str();

  // log_values goes first and explicitly: ON DELETE CASCADE only fires with
  // foreign_keys=ON, and a database opened by anything that didn't set that
  // pragma would otherwise leak orphaned value rows.
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                          "DELETE FROM log_values WHERE log_id IN "
                          "(SELECT id FROM log_entries WHERE timestamp < ?);",
                          -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, cutoffText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  int64_t removed = 0;
  if (sqlite3_prepare_v2(db_, "DELETE FROM log_entries WHERE timestamp < ?;", -1, &stmt, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, cutoffText.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_DONE) removed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
  }
  return removed;
}

int64_t LogStore::Count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM log_entries;", -1, &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }
  int64_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

}  // namespace hsf
