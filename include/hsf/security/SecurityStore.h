#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/security/Permissions.h"

// Both at global scope, and both needed. Writing `struct sqlite3_stmt*`
// inline in a member declaration below would instead declare a brand new
// hsf::sqlite3_stmt, and every call into the C API would then fail to convert.
struct sqlite3;
struct sqlite3_stmt;

namespace hsf {

struct User {
  int64_t id = 0;
  std::string username;
  std::string display_name;
  Role role = Role::kUser;
  bool enabled = true;
  int64_t created_at = 0;
  int64_t last_login = 0;    // 0 = never
  int64_t password_changed = 0;
  bool must_change_password = false;
};

// A validated bearer session. Returned by Authenticate() and carried through
// the request by the middleware.
struct Session {
  int64_t id = 0;
  int64_t user_id = 0;
  std::string username;
  Role role = Role::kUser;
  int64_t issued_at = 0;
  int64_t expires_at = 0;
  std::string source_ip;
};

// The security database (request/AdvanceUpdate.md sections 1.1, 1.3, 1.10):
// accounts, bearer sessions, failed-login tracking and the audit trail, in
// their own SQLite file (default config/security.db, a sibling of config.db,
// clients.db and logs.db).
//
// A FIFTH DATABASE, RATHER THAN TABLES IN AN EXISTING ONE. It follows the
// convention already in this codebase -- each store owns its schema -- but
// there is a sharper reason here: this file holds password hashes and live
// session tokens, and it is the one file whose backup, file permissions and
// disposal rules differ from everything else. Keeping it separate means those
// rules can be stated about a path rather than about a subset of tables.
//
// TOKENS ARE STORED HASHED. The `sessions` table keeps a SHA-256 of the token,
// never the token itself, for the same reason the `users` table keeps an
// Argon2id hash rather than a password: whoever reads this file at rest must
// not come away with credentials they can replay. Argon2id would be wrong here
// -- a token is 256 bits of real entropy, not a guessable human password, so
// there is nothing for a slow KDF to buy, and it runs on every single request.
class SecurityStore {
 public:
  static SecurityStore& Instance();
  ~SecurityStore();

  // Opens/creates the database and its schema. Seeds a single ADMIN account on
  // an empty database and returns its generated password through
  // `seededPassword` so the caller can print it once -- there is deliberately
  // no default password to look up later.
  bool Load(const std::string& path, std::string& seededPassword);

  // --- accounts -------------------------------------------------------------

  std::optional<User> FindUser(const std::string& username) const;
  std::optional<User> FindUserById(int64_t id) const;
  std::vector<User> ListUsers() const;
  std::optional<User> CreateUser(const std::string& username, const std::string& password, Role role,
                                 const std::string& displayName, std::string& error);
  bool SetPassword(int64_t userId, const std::string& password, std::string& error);
  bool SetEnabled(int64_t userId, bool enabled);
  bool SetRole(int64_t userId, Role role);
  bool DeleteUser(int64_t userId, std::string& error);
  int64_t CountAdmins() const;

  // --- login ------------------------------------------------------------------

  enum class LoginResult { kOk, kInvalidCredentials, kLockedOut, kDisabled, kInternalError };

  // Verifies the password and, on success, issues a session.
  //
  // `lockoutRemaining` is filled in for kLockedOut. Everything else collapses
  // to kInvalidCredentials on purpose: telling a caller "that user exists but
  // the password is wrong" hands them half the answer
  // (request/AdvanceUpdate.md section 1.1).
  LoginResult Login(const std::string& username, const std::string& password, const std::string& sourceIp,
                    int maxFailedAttempts, int lockoutSeconds, int tokenLifetimeSeconds,
                    int maxSessionsPerUser, std::string& tokenOut, Session& sessionOut,
                    int64_t& lockoutRemaining);

  // Looks up a bearer token. std::nullopt when unknown, expired, revoked, or
  // belonging to a disabled account. Expired rows are deleted as they are
  // encountered, which keeps the table trimmed without a sweeper thread.
  std::optional<Session> Authenticate(const std::string& token);

  bool Revoke(const std::string& token);
  // Used after a password change or a role change: every existing session for
  // that user stops working (request/AdvanceUpdate.md section 1.1).
  int RevokeAllForUser(int64_t userId);
  int PurgeExpiredSessions();
  int64_t CountActiveSessions(int64_t userId) const;

  // --- audit ---------------------------------------------------------------------

  struct AuditEntry {
    int64_t id = 0;
    int64_t timestamp = 0;
    std::string event;
    std::string username;
    std::string source_ip;
    std::string resource;
    std::string result;
    std::string reason;
  };

  // Never blocks the caller on anything but a local insert. `reason` should
  // never contain a password or a token -- see the note in AuditLog.h.
  void RecordAudit(const std::string& event, const std::string& username, const std::string& sourceIp,
                   const std::string& resource, const std::string& result, const std::string& reason);

  std::vector<AuditEntry> QueryAudit(const std::string& eventFilter, const std::string& usernameFilter,
                                     int limit, int offset) const;
  int64_t CountAudit(const std::string& eventFilter, const std::string& usernameFilter) const;
  // Deletes rows older than `days`. 0 keeps everything.
  int64_t PruneAudit(int days);

  static nlohmann::json ToJson(const User& user);
  static nlohmann::json ToJson(const AuditEntry& entry);

 private:
  SecurityStore() = default;
  SecurityStore(const SecurityStore&) = delete;
  SecurityStore& operator=(const SecurityStore&) = delete;

  bool EnsureSchema();
  std::optional<User> ReadUserRow(::sqlite3_stmt* stmt) const;
  // Callers already hold mutex_.
  std::optional<User> FindUserUnlocked(const std::string& username) const;
  int64_t FailedAttemptsUnlocked(const std::string& username, int64_t since) const;
  void RecordFailureUnlocked(const std::string& username, const std::string& sourceIp);
  void ClearFailuresUnlocked(const std::string& username);
  void EnforceSessionLimitUnlocked(int64_t userId, int maxSessions);

  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
};

}  // namespace hsf
