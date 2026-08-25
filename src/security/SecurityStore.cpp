#include "hsf/security/SecurityStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>

#include "hsf/Logger.h"
#include "hsf/security/PasswordHash.h"
#include "hsf/update/Sha256.h"

using nlohmann::json;

namespace hsf {
namespace {

int64_t NowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

// Tokens are looked up by hash, never stored in the clear. SHA-256 and not
// Argon2id: a token is 256 bits of libsodium randomness, so there is nothing
// for a slow KDF to defend -- and this runs on every authenticated request.
std::string TokenHash(const std::string& token) { return Sha256::HexOf(token); }

bool Exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                             std::string("SecurityStore: ") + (error ? error : "unknown error"));
    sqlite3_free(error);
    return false;
  }
  return true;
}

void BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  return text ? reinterpret_cast<const char*>(text) : "";
}

// Usernames are an identity, a filename-safe token in the audit log, and a
// lookup key. Keep them boring.
bool ValidUsername(const std::string& username, std::string& error) {
  if (username.size() < 3 || username.size() > 32) {
    error = "username must be 3-32 characters";
    return false;
  }
  for (char c : username) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
    if (!ok) {
      error = "username may contain only letters, digits, dot, underscore and hyphen";
      return false;
    }
  }
  return true;
}

}  // namespace

SecurityStore& SecurityStore::Instance() {
  static SecurityStore instance;
  return instance;
}

SecurityStore::~SecurityStore() {
  if (db_) sqlite3_close(db_);
}

bool SecurityStore::EnsureSchema() {
  static const char* kSchema = R"SQL(
    PRAGMA journal_mode=WAL;
    PRAGMA foreign_keys=ON;

    CREATE TABLE IF NOT EXISTS users (
      id                   INTEGER PRIMARY KEY AUTOINCREMENT,
      username             TEXT NOT NULL UNIQUE COLLATE NOCASE,
      display_name         TEXT NOT NULL DEFAULT '',
      password_hash        TEXT NOT NULL,
      role                 TEXT NOT NULL,
      enabled              INTEGER NOT NULL DEFAULT 1,
      created_at           INTEGER NOT NULL,
      last_login           INTEGER NOT NULL DEFAULT 0,
      password_changed     INTEGER NOT NULL DEFAULT 0,
      must_change_password INTEGER NOT NULL DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS sessions (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      token_hash TEXT NOT NULL UNIQUE,
      user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      issued_at  INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      source_ip  TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX IF NOT EXISTS sessions_user_idx ON sessions(user_id);
    CREATE INDEX IF NOT EXISTS sessions_expiry_idx ON sessions(expires_at);

    CREATE TABLE IF NOT EXISTS login_failures (
      id        INTEGER PRIMARY KEY AUTOINCREMENT,
      username  TEXT NOT NULL COLLATE NOCASE,
      source_ip TEXT NOT NULL DEFAULT '',
      at        INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS login_failures_idx ON login_failures(username, at);

    CREATE TABLE IF NOT EXISTS audit_log (
      id        INTEGER PRIMARY KEY AUTOINCREMENT,
      ts        INTEGER NOT NULL,
      event     TEXT NOT NULL,
      username  TEXT NOT NULL DEFAULT '',
      source_ip TEXT NOT NULL DEFAULT '',
      resource  TEXT NOT NULL DEFAULT '',
      result    TEXT NOT NULL DEFAULT '',
      reason    TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX IF NOT EXISTS audit_ts_idx ON audit_log(ts);
    CREATE INDEX IF NOT EXISTS audit_event_idx ON audit_log(event);
  )SQL";
  return Exec(db_, kSchema);
}

bool SecurityStore::Load(const std::string& path, std::string& seededPassword) {
  seededPassword.clear();
  if (!PasswordHash::Initialise()) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    Logger::Instance().Error(LogCategory::System,
                             std::string("SecurityStore: cannot open ") + path + ": " + sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return false;
  }
  if (db_) sqlite3_close(db_);
  db_ = db;

  if (!EnsureSchema()) return false;

  // Sessions do not survive a restart of the process that issued them. That is
  // a deliberate call, not laziness: the alternative is that a token stolen
  // from a machine keeps working across a reboot the operator performed
  // *because* something looked wrong.
  Exec(db_, "DELETE FROM sessions;");

  sqlite3_stmt* stmt = nullptr;
  int64_t userCount = 0;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) userCount = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }

  if (userCount == 0) {
    // Seed exactly one administrator with a generated password. No default
    // credential exists at any point -- "admin/admin" on a device reachable
    // from a plant network is the failure this whole plan is about.
    const std::string password = PasswordHash::RandomToken(16);
    const std::string hash = PasswordHash::Hash(password);
    if (hash.empty()) {
      Logger::Instance().Error(LogCategory::System, "SecurityStore: could not hash the seed password");
      return false;
    }
    const char* kInsert =
        "INSERT INTO users (username, display_name, password_hash, role, enabled, created_at, "
        "password_changed, must_change_password) VALUES (?,?,?,?,1,?,?,1);";
    if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, "admin");
    BindText(stmt, 2, "Administrator");
    BindText(stmt, 3, hash);
    BindText(stmt, 4, RoleName(Role::kAdmin));
    sqlite3_bind_int64(stmt, 5, NowUnix());
    sqlite3_bind_int64(stmt, 6, NowUnix());
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) {
      Logger::Instance().Error(LogCategory::System, "SecurityStore: failed to seed the admin account");
      return false;
    }
    seededPassword = password;
  }
  return true;
}

// --- accounts ---------------------------------------------------------------

std::optional<User> SecurityStore::ReadUserRow(sqlite3_stmt* stmt) const {
  User user;
  user.id = sqlite3_column_int64(stmt, 0);
  user.username = ColumnText(stmt, 1);
  user.display_name = ColumnText(stmt, 2);
  user.role = RoleFromName(ColumnText(stmt, 3));
  if (user.role == Role::kCount) user.role = Role::kUser;  // unknown role -> least privilege
  user.enabled = sqlite3_column_int(stmt, 4) != 0;
  user.created_at = sqlite3_column_int64(stmt, 5);
  user.last_login = sqlite3_column_int64(stmt, 6);
  user.password_changed = sqlite3_column_int64(stmt, 7);
  user.must_change_password = sqlite3_column_int(stmt, 8) != 0;
  return user;
}

static const char* kUserColumns =
    "id, username, display_name, role, enabled, created_at, last_login, password_changed, "
    "must_change_password";

std::optional<User> SecurityStore::FindUserUnlocked(const std::string& username) const {
  if (!db_) return std::nullopt;
  const std::string sql = std::string("SELECT ") + kUserColumns + " FROM users WHERE username = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
  BindText(stmt, 1, username);
  std::optional<User> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) result = ReadUserRow(stmt);
  sqlite3_finalize(stmt);
  return result;
}

std::optional<User> SecurityStore::FindUser(const std::string& username) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return FindUserUnlocked(username);
}

std::optional<User> SecurityStore::FindUserById(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return std::nullopt;
  const std::string sql = std::string("SELECT ") + kUserColumns + " FROM users WHERE id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_int64(stmt, 1, id);
  std::optional<User> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) result = ReadUserRow(stmt);
  sqlite3_finalize(stmt);
  return result;
}

std::vector<User> SecurityStore::ListUsers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<User> users;
  if (!db_) return users;
  const std::string sql = std::string("SELECT ") + kUserColumns + " FROM users ORDER BY username;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return users;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (auto user = ReadUserRow(stmt)) users.push_back(*user);
  }
  sqlite3_finalize(stmt);
  return users;
}

std::optional<User> SecurityStore::CreateUser(const std::string& username, const std::string& password,
                                              Role role, const std::string& displayName, std::string& error) {
  if (!ValidUsername(username, error)) return std::nullopt;
  if (role == Role::kCount) {
    error = "unknown role";
    return std::nullopt;
  }
  const std::string hash = PasswordHash::Hash(password);
  if (hash.empty()) {
    error = "could not hash the password";
    return std::nullopt;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) {
    error = "security database unavailable";
    return std::nullopt;
  }
  if (FindUserUnlocked(username)) {
    error = "a user with that name already exists";
    return std::nullopt;
  }

  const char* kInsert =
      "INSERT INTO users (username, display_name, password_hash, role, enabled, created_at, "
      "password_changed, must_change_password) VALUES (?,?,?,?,1,?,?,0);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
    error = "database error";
    return std::nullopt;
  }
  BindText(stmt, 1, username);
  BindText(stmt, 2, displayName.empty() ? username : displayName);
  BindText(stmt, 3, hash);
  BindText(stmt, 4, RoleName(role));
  sqlite3_bind_int64(stmt, 5, NowUnix());
  sqlite3_bind_int64(stmt, 6, NowUnix());
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!ok) {
    error = "database error";
    return std::nullopt;
  }
  return FindUserUnlocked(username);
}

bool SecurityStore::SetPassword(int64_t userId, const std::string& password, std::string& error) {
  const std::string hash = PasswordHash::Hash(password);
  if (hash.empty()) {
    error = "could not hash the password";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
      error = "security database unavailable";
      return false;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* kSql =
        "UPDATE users SET password_hash = ?, password_changed = ?, must_change_password = 0 WHERE id = ?;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
      error = "database error";
      return false;
    }
    BindText(stmt, 1, hash);
    sqlite3_bind_int64(stmt, 2, NowUnix());
    sqlite3_bind_int64(stmt, 3, userId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) {
      error = "database error";
      return false;
    }
  }
  // Outside the lock above only because RevokeAllForUser takes it again.
  // Section 1.1: a password change invalidates every existing session, so a
  // stolen token cannot outlive the response to the theft.
  RevokeAllForUser(userId);
  return true;
}

bool SecurityStore::SetEnabled(int64_t userId, bool enabled) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE users SET enabled = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, userId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return false;
  }
  if (!enabled) RevokeAllForUser(userId);
  return true;
}

bool SecurityStore::SetRole(int64_t userId, Role role) {
  if (role == Role::kCount) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE users SET role = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    BindText(stmt, 1, RoleName(role));
    sqlite3_bind_int64(stmt, 2, userId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return false;
  }
  // The session carries the role it was issued with, so an existing one would
  // keep the old privileges until it expired. Revoking makes a demotion take
  // effect immediately -- which is the direction that matters.
  RevokeAllForUser(userId);
  return true;
}

int64_t SecurityStore::CountAdmins() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  const char* kSql = "SELECT COUNT(*) FROM users WHERE role = ? AND enabled = 1;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
  BindText(stmt, 1, RoleName(Role::kAdmin));
  int64_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

bool SecurityStore::DeleteUser(int64_t userId, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) {
    error = "security database unavailable";
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM users WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    error = "database error";
    return false;
  }
  sqlite3_bind_int64(stmt, 1, userId);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!ok) error = "database error";
  return ok;
}

// --- login --------------------------------------------------------------------

int64_t SecurityStore::FailedAttemptsUnlocked(const std::string& username, int64_t since) const {
  sqlite3_stmt* stmt = nullptr;
  const char* kSql = "SELECT COUNT(*) FROM login_failures WHERE username = ? AND at >= ?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
  BindText(stmt, 1, username);
  sqlite3_bind_int64(stmt, 2, since);
  int64_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

void SecurityStore::RecordFailureUnlocked(const std::string& username, const std::string& sourceIp) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "INSERT INTO login_failures (username, source_ip, at) VALUES (?,?,?);", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    return;
  }
  BindText(stmt, 1, username);
  BindText(stmt, 2, sourceIp);
  sqlite3_bind_int64(stmt, 3, NowUnix());
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void SecurityStore::ClearFailuresUnlocked(const std::string& username) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM login_failures WHERE username = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return;
  }
  BindText(stmt, 1, username);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void SecurityStore::EnforceSessionLimitUnlocked(int64_t userId, int maxSessions) {
  if (maxSessions <= 0) return;  // unlimited
  sqlite3_stmt* stmt = nullptr;
  // Keep the newest (maxSessions - 1); the caller is about to insert one more.
  const char* kSql =
      "DELETE FROM sessions WHERE user_id = ? AND id NOT IN "
      "(SELECT id FROM sessions WHERE user_id = ? ORDER BY issued_at DESC LIMIT ?);";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, userId);
  sqlite3_bind_int64(stmt, 2, userId);
  sqlite3_bind_int(stmt, 3, maxSessions > 1 ? maxSessions - 1 : 0);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

SecurityStore::LoginResult SecurityStore::Login(const std::string& username, const std::string& password,
                                                const std::string& sourceIp, int maxFailedAttempts,
                                                int lockoutSeconds, int tokenLifetimeSeconds,
                                                int maxSessionsPerUser, std::string& tokenOut,
                                                Session& sessionOut, int64_t& lockoutRemaining) {
  tokenOut.clear();
  lockoutRemaining = 0;

  // The hash comparison happens outside the lock -- Argon2id takes ~100 ms and
  // 64 MB, and holding the store mutex for that would serialise every
  // authenticated request in the gateway behind one login attempt. That is not
  // a theoretical concern: it is exactly the amplification an attacker gets for
  // free from a brute-force attempt.
  std::string storedHash;
  User user;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return LoginResult::kInternalError;

    if (maxFailedAttempts > 0 && lockoutSeconds > 0) {
      const int64_t windowStart = NowUnix() - lockoutSeconds;
      if (FailedAttemptsUnlocked(username, windowStart) >= maxFailedAttempts) {
        sqlite3_stmt* stmt = nullptr;
        const char* kSql = "SELECT MAX(at) FROM login_failures WHERE username = ? AND at >= ?;";
        int64_t latest = NowUnix();
        if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK) {
          BindText(stmt, 1, username);
          sqlite3_bind_int64(stmt, 2, windowStart);
          if (sqlite3_step(stmt) == SQLITE_ROW) latest = sqlite3_column_int64(stmt, 0);
          sqlite3_finalize(stmt);
        }
        lockoutRemaining = (latest + lockoutSeconds) - NowUnix();
        if (lockoutRemaining < 1) lockoutRemaining = 1;
        return LoginResult::kLockedOut;
      }
    }

    const std::string sql = std::string("SELECT ") + kUserColumns + ", password_hash FROM users WHERE username = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return LoginResult::kInternalError;
    BindText(stmt, 1, username);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      if (auto row = ReadUserRow(stmt)) {
        user = *row;
        storedHash = ColumnText(stmt, 9);
        found = true;
      }
    }
    sqlite3_finalize(stmt);

    if (!found) {
      // Still record the failure, so enumerating usernames is rate limited the
      // same way guessing a password is.
      RecordFailureUnlocked(username, sourceIp);
      return LoginResult::kInvalidCredentials;
    }
  }

  const bool passwordOk = PasswordHash::Verify(storedHash, password);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return LoginResult::kInternalError;

  if (!passwordOk) {
    RecordFailureUnlocked(username, sourceIp);
    return LoginResult::kInvalidCredentials;
  }
  if (!user.enabled) {
    // Counted as a failure too: a disabled account being probed is exactly as
    // interesting as a wrong password.
    RecordFailureUnlocked(username, sourceIp);
    return LoginResult::kDisabled;
  }

  ClearFailuresUnlocked(username);
  EnforceSessionLimitUnlocked(user.id, maxSessionsPerUser);

  const std::string token = PasswordHash::RandomHex(32);  // 256 bits
  if (token.empty()) return LoginResult::kInternalError;

  const int64_t now = NowUnix();
  const int64_t expires = now + (tokenLifetimeSeconds > 0 ? tokenLifetimeSeconds : 1800);

  sqlite3_stmt* stmt = nullptr;
  const char* kInsert =
      "INSERT INTO sessions (token_hash, user_id, issued_at, expires_at, source_ip) VALUES (?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) return LoginResult::kInternalError;
  BindText(stmt, 1, TokenHash(token));
  sqlite3_bind_int64(stmt, 2, user.id);
  sqlite3_bind_int64(stmt, 3, now);
  sqlite3_bind_int64(stmt, 4, expires);
  BindText(stmt, 5, sourceIp);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  const int64_t sessionId = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(stmt);
  if (!ok) return LoginResult::kInternalError;

  if (sqlite3_prepare_v2(db_, "UPDATE users SET last_login = ? WHERE id = ?;", -1, &stmt, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, user.id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  tokenOut = token;
  sessionOut.id = sessionId;
  sessionOut.user_id = user.id;
  sessionOut.username = user.username;
  sessionOut.role = user.role;
  sessionOut.issued_at = now;
  sessionOut.expires_at = expires;
  sessionOut.source_ip = sourceIp;
  return LoginResult::kOk;
}

std::optional<Session> SecurityStore::Authenticate(const std::string& token) {
  if (token.empty()) return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return std::nullopt;

  const std::string hash = TokenHash(token);
  sqlite3_stmt* stmt = nullptr;
  const char* kSql =
      "SELECT s.id, s.user_id, s.issued_at, s.expires_at, s.source_ip, u.username, u.role, u.enabled "
      "FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.token_hash = ?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
  BindText(stmt, 1, hash);

  std::optional<Session> result;
  bool expired = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    Session session;
    session.id = sqlite3_column_int64(stmt, 0);
    session.user_id = sqlite3_column_int64(stmt, 1);
    session.issued_at = sqlite3_column_int64(stmt, 2);
    session.expires_at = sqlite3_column_int64(stmt, 3);
    session.source_ip = ColumnText(stmt, 4);
    session.username = ColumnText(stmt, 5);
    session.role = RoleFromName(ColumnText(stmt, 6));
    if (session.role == Role::kCount) session.role = Role::kUser;
    const bool enabled = sqlite3_column_int(stmt, 7) != 0;

    if (session.expires_at <= NowUnix()) {
      expired = true;
    } else if (enabled) {
      result = session;
    }
    // A disabled account falls through to nullopt without deleting the row;
    // re-enabling the user should not require them to have kept a session.
  }
  sqlite3_finalize(stmt);

  if (expired) {
    // Drop it as we go, so the table stays trimmed with no sweeper thread.
    if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE token_hash = ?;", -1, &stmt, nullptr) ==
        SQLITE_OK) {
      BindText(stmt, 1, hash);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  return result;
}

bool SecurityStore::Revoke(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || token.empty()) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE token_hash = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  BindText(stmt, 1, TokenHash(token));
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok && sqlite3_changes(db_) > 0;
}

int SecurityStore::RevokeAllForUser(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE user_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int64(stmt, 1, userId);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return sqlite3_changes(db_);
}

int SecurityStore::PurgeExpiredSessions() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE expires_at <= ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int64(stmt, 1, NowUnix());
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return sqlite3_changes(db_);
}

int64_t SecurityStore::CountActiveSessions(int64_t userId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM sessions WHERE user_id = ? AND expires_at > ?;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int64(stmt, 1, userId);
  sqlite3_bind_int64(stmt, 2, NowUnix());
  int64_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

// --- audit ---------------------------------------------------------------------

void SecurityStore::RecordAudit(const std::string& event, const std::string& username,
                                const std::string& sourceIp, const std::string& resource,
                                const std::string& result, const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return;
  sqlite3_stmt* stmt = nullptr;
  const char* kSql =
      "INSERT INTO audit_log (ts, event, username, source_ip, resource, result, reason) VALUES "
      "(?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, NowUnix());
  BindText(stmt, 2, event);
  BindText(stmt, 3, username);
  BindText(stmt, 4, sourceIp);
  BindText(stmt, 5, resource);
  BindText(stmt, 6, result);
  BindText(stmt, 7, reason);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::vector<SecurityStore::AuditEntry> SecurityStore::QueryAudit(const std::string& eventFilter,
                                                                 const std::string& usernameFilter, int limit,
                                                                 int offset) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AuditEntry> entries;
  if (!db_) return entries;

  // Filters are bound, never concatenated (section 1.6). The LIKE pattern is
  // built here but the value still travels as a parameter.
  std::string sql =
      "SELECT id, ts, event, username, source_ip, resource, result, reason FROM audit_log WHERE 1=1";
  if (!eventFilter.empty()) sql += " AND event = ?";
  if (!usernameFilter.empty()) sql += " AND username LIKE ?";
  sql += " ORDER BY id DESC LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return entries;
  int index = 1;
  if (!eventFilter.empty()) BindText(stmt, index++, eventFilter);
  if (!usernameFilter.empty()) BindText(stmt, index++, "%" + usernameFilter + "%");
  sqlite3_bind_int(stmt, index++, limit > 0 ? limit : 100);
  sqlite3_bind_int(stmt, index++, offset > 0 ? offset : 0);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    AuditEntry entry;
    entry.id = sqlite3_column_int64(stmt, 0);
    entry.timestamp = sqlite3_column_int64(stmt, 1);
    entry.event = ColumnText(stmt, 2);
    entry.username = ColumnText(stmt, 3);
    entry.source_ip = ColumnText(stmt, 4);
    entry.resource = ColumnText(stmt, 5);
    entry.result = ColumnText(stmt, 6);
    entry.reason = ColumnText(stmt, 7);
    entries.push_back(std::move(entry));
  }
  sqlite3_finalize(stmt);
  return entries;
}

int64_t SecurityStore::CountAudit(const std::string& eventFilter, const std::string& usernameFilter) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;
  std::string sql = "SELECT COUNT(*) FROM audit_log WHERE 1=1";
  if (!eventFilter.empty()) sql += " AND event = ?";
  if (!usernameFilter.empty()) sql += " AND username LIKE ?";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
  int index = 1;
  if (!eventFilter.empty()) BindText(stmt, index++, eventFilter);
  if (!usernameFilter.empty()) BindText(stmt, index++, "%" + usernameFilter + "%");
  int64_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

int64_t SecurityStore::PruneAudit(int days) {
  if (days <= 0) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM audit_log WHERE ts < ?;", -1, &stmt, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(stmt, 1, NowUnix() - static_cast<int64_t>(days) * 86400);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return sqlite3_changes(db_);
}

json SecurityStore::ToJson(const User& user) {
  return json{{"id", user.id},
              {"username", user.username},
              {"display_name", user.display_name},
              {"role", RoleName(user.role)},
              {"permissions", PermissionNamesFor(user.role)},
              {"enabled", user.enabled},
              {"created_at", user.created_at},
              {"last_login", user.last_login},
              {"must_change_password", user.must_change_password}};
}

json SecurityStore::ToJson(const AuditEntry& entry) {
  return json{{"id", entry.id},         {"timestamp", entry.timestamp}, {"event", entry.event},
              {"username", entry.username}, {"source_ip", entry.source_ip}, {"resource", entry.resource},
              {"result", entry.result}, {"reason", entry.reason}};
}

}  // namespace hsf
