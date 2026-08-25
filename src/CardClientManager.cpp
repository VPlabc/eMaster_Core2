#include "hsf/CardClientManager.h"

#include <sqlite3.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

#include "hsf/Logger.h"

namespace hsf {

using nlohmann::json;

namespace {

constexpr const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS card_clients ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL,"
    "api_key TEXT NOT NULL UNIQUE,"
    "enabled INTEGER NOT NULL DEFAULT 1,"
    "created_at INTEGER NOT NULL,"
    "last_access INTEGER NOT NULL DEFAULT 0,"
    "expires_at INTEGER NOT NULL DEFAULT 0);";

int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string GenerateApiKey() {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 4; ++i) oss << std::setw(16) << rng();
  return oss.str();
}

CardClient RowToClient(sqlite3_stmt* stmt) {
  CardClient c;
  c.id = sqlite3_column_int64(stmt, 0);
  const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  c.name = name ? name : "";
  const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  c.api_key = key ? key : "";
  c.enabled = sqlite3_column_int(stmt, 3) != 0;
  c.created_at = sqlite3_column_int64(stmt, 4);
  c.last_access = sqlite3_column_int64(stmt, 5);
  c.expires_at = sqlite3_column_int64(stmt, 6);
  return c;
}

}  // namespace

CardClientManager& CardClientManager::Instance() {
  static CardClientManager instance;
  return instance;
}

CardClientManager::~CardClientManager() {
  if (db_) sqlite3_close(db_);
}

bool CardClientManager::Load(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              "CardClientManager: failed to open " + path + ": " +
                                  (db ? sqlite3_errmsg(db) : "unknown error"));
    if (db) sqlite3_close(db);
    return false;
  }

  char* errMsg = nullptr;
  if (sqlite3_exec(db, kCreateTableSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("CardClientManager: failed to create schema: ") +
                                  (errMsg ? errMsg : "unknown error"));
    sqlite3_free(errMsg);
    sqlite3_close(db);
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) sqlite3_close(db_);
  db_ = db;
  return true;
}

std::optional<CardClient> CardClientManager::CreateClient(const std::string& name, int64_t expiresAt) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return std::nullopt;

  std::string apiKey = GenerateApiKey();
  int64_t now = NowUnixSeconds();

  static const char* kInsertSql =
      "INSERT INTO card_clients(name, api_key, enabled, created_at, last_access, expires_at) "
      "VALUES(?, ?, 1, ?, 0, ?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("CardClientManager: prepare insert failed: ") + sqlite3_errmsg(db_));
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, apiKey.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, now);
  sqlite3_bind_int64(stmt, 4, expiresAt);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!ok) {
    Logger::Instance().Error(LogCategory::System,
                              std::string("CardClientManager: insert failed: ") + sqlite3_errmsg(db_));
    return std::nullopt;
  }

  CardClient client;
  client.id = sqlite3_last_insert_rowid(db_);
  client.name = name;
  client.api_key = apiKey;
  client.enabled = true;
  client.created_at = now;
  client.last_access = 0;
  client.expires_at = expiresAt;
  return client;
}

bool CardClientManager::SetEnabled(int64_t id, bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "UPDATE card_clients SET enabled = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
  sqlite3_bind_int64(stmt, 2, id);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok && sqlite3_changes(db_) > 0;
}

bool CardClientManager::DeleteClient(int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM card_clients WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, id);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok && sqlite3_changes(db_) > 0;
}

std::vector<CardClient> CardClientManager::ListClients() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<CardClient> result;
  if (!db_) return result;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                          "SELECT id, name, api_key, enabled, created_at, last_access, expires_at "
                          "FROM card_clients ORDER BY id;",
                          -1, &stmt, nullptr) != SQLITE_OK) {
    return result;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    result.push_back(RowToClient(stmt));
  }
  sqlite3_finalize(stmt);
  return result;
}

std::optional<CardClient> CardClientManager::Authenticate(const std::string& apiKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || apiKey.empty()) return std::nullopt;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                          "SELECT id, name, api_key, enabled, created_at, last_access, expires_at "
                          "FROM card_clients WHERE api_key = ?;",
                          -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, apiKey.c_str(), -1, SQLITE_TRANSIENT);

  std::optional<CardClient> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    result = RowToClient(stmt);
  }
  sqlite3_finalize(stmt);

  if (!result) return std::nullopt;
  int64_t now = NowUnixSeconds();
  if (!result->enabled) return std::nullopt;
  if (result->expires_at != 0 && result->expires_at <= now) return std::nullopt;

  sqlite3_stmt* updateStmt = nullptr;
  if (sqlite3_prepare_v2(db_, "UPDATE card_clients SET last_access = ? WHERE id = ?;", -1, &updateStmt, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_int64(updateStmt, 1, now);
    sqlite3_bind_int64(updateStmt, 2, result->id);
    sqlite3_step(updateStmt);
    sqlite3_finalize(updateStmt);
  }
  result->last_access = now;
  return result;
}

json CardClientManager::ToJson(const CardClient& c) {
  return json{{"id", c.id},
              {"name", c.name},
              {"api_key", c.api_key},
              {"enabled", c.enabled},
              {"created_at", c.created_at},
              {"last_access", c.last_access},
              {"expires_at", c.expires_at}};
}

}  // namespace hsf
