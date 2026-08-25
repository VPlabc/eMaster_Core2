#include "hsf/SqlDatabase.h"

#include <sqlite3.h>

#include <cctype>

#include "hsf/Logger.h"

namespace hsf {

using nlohmann::json;

namespace {

// How long a statement waits for another connection's write lock before giving
// up. The gateway's writers are short (one row per card swipe), so anything
// beyond a couple of seconds is a genuine problem rather than contention.
constexpr int kBusyTimeoutMs = 3000;

json ColumnValue(sqlite3_stmt* stmt, int index) {
  switch (sqlite3_column_type(stmt, index)) {
    case SQLITE_INTEGER:
      return json(static_cast<int64_t>(sqlite3_column_int64(stmt, index)));
    case SQLITE_FLOAT:
      return json(sqlite3_column_double(stmt, index));
    case SQLITE_NULL:
      return json(nullptr);
    case SQLITE_BLOB: {
      const void* blob = sqlite3_column_blob(stmt, index);
      int bytes = sqlite3_column_bytes(stmt, index);
      return json(std::string(static_cast<const char*>(blob), static_cast<size_t>(bytes)));
    }
    default: {
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
      return json(text ? std::string(text) : std::string());
    }
  }
}

}  // namespace

SqlDatabase::~SqlDatabase() { Close(); }

bool SqlDatabase::Open(const std::string& path, bool readOnly, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
    transactionDepth_ = 0;
  }

  int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    error = db_ ? sqlite3_errmsg(db_) : "cannot open database";
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }

  sqlite3_busy_timeout(db_, kBusyTimeoutMs);

  // WAL survives across connections (it is a property of the file, not of this
  // handle) and cannot be set on a read-only connection, so a reader simply
  // inherits whatever the writer established.
  if (!readOnly) {
    std::string ignored;
    ExecUnlocked("PRAGMA journal_mode=WAL;", ignored);
    ExecUnlocked("PRAGMA synchronous=NORMAL;", ignored);
    // Referential integrity is off by default in SQLite, and a locker row
    // pointing at a deleted employee is exactly the inconsistency the plan
    // asks transactions to prevent.
    ExecUnlocked("PRAGMA foreign_keys=ON;", ignored);
  }

  path_ = path;
  readOnly_ = readOnly;

  return true;
}

void SqlDatabase::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return;

  // An open transaction at close would be rolled back by SQLite anyway; doing
  // it explicitly keeps the WAL tidy and the intent obvious.
  if (transactionDepth_ > 0) {
    std::string ignored;
    ExecUnlocked("ROLLBACK;", ignored);
    transactionDepth_ = 0;
  }

  sqlite3_close(db_);
  db_ = nullptr;
  path_.clear();
}

bool SqlDatabase::IsOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return db_ != nullptr;
}

std::string SqlDatabase::Path() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return path_;
}

int SqlDatabase::TransactionDepth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return transactionDepth_;
}

bool SqlDatabase::ExecUnlocked(const std::string& sql, std::string& error) {
  if (!db_) {
    error = "database is not open";
    return false;
  }
  char* message = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK) return true;
  error = message ? message : "unknown SQLite error";
  sqlite3_free(message);
  return false;
}

SqlResult SqlDatabase::Run(const std::string& sql, const std::vector<SqlValue>& params, bool collectRows) {
  SqlResult result;

  if (!db_) {
    result.error = "database is not open";
    return result;
  }

  // A batch of statements is only meaningful without parameters (see the
  // header): sqlite3_prepare_v2 compiles the first statement and reports the
  // rest as a tail, and binding "?1" across a batch has no defined target.
  if (params.empty() && !collectRows) {
    if (!ExecUnlocked(sql, result.error)) return result;
    result.ok = true;
    result.changes = sqlite3_changes(db_);
    result.last_insert_id = sqlite3_last_insert_rowid(db_);
    return result;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    result.error = sqlite3_errmsg(db_);
    return result;
  }

  int expected = sqlite3_bind_parameter_count(stmt);
  if (static_cast<int>(params.size()) != expected) {
    // Reported rather than tolerated: a missing parameter binds as NULL in
    // SQLite, which silently turns "WHERE card_code = ?" into a query that
    // matches nothing -- an access check that fails open on a typo.
    result.error = "statement expects " + std::to_string(expected) + " parameter(s), got " +
                    std::to_string(params.size());
    sqlite3_finalize(stmt);
    return result;
  }

  for (size_t i = 0; i < params.size(); ++i) {
    int index = static_cast<int>(i) + 1;
    const SqlValue& value = params[i];
    int rc = SQLITE_OK;
    switch (value.kind) {
      case SqlValue::Kind::Integer:
        rc = sqlite3_bind_int64(stmt, index, value.integer);
        break;
      case SqlValue::Kind::Real:
        rc = sqlite3_bind_double(stmt, index, value.real);
        break;
      case SqlValue::Kind::Text:
        rc = sqlite3_bind_text(stmt, index, value.text.c_str(), static_cast<int>(value.text.size()),
                                SQLITE_TRANSIENT);
        break;
      case SqlValue::Kind::Blob:
        rc = sqlite3_bind_blob(stmt, index, value.text.data(), static_cast<int>(value.text.size()),
                                SQLITE_TRANSIENT);
        break;
      case SqlValue::Kind::Null:
      default:
        rc = sqlite3_bind_null(stmt, index);
        break;
    }
    if (rc != SQLITE_OK) {
      result.error = std::string("cannot bind parameter ") + std::to_string(index) + ": " + sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      return result;
    }
  }

  int columnCount = sqlite3_column_count(stmt);
  for (int i = 0; i < columnCount; ++i) {
    const char* name = sqlite3_column_name(stmt, i);
    result.columns.push_back(name ? name : ("column" + std::to_string(i + 1)));
  }

  while (true) {
    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
      if (collectRows) {
        json row = json::object();
        for (int i = 0; i < columnCount; ++i) {
          row[result.columns[static_cast<size_t>(i)]] = ColumnValue(stmt, i);
        }
        result.rows.push_back(std::move(row));
      }
      continue;
    }

    if (rc == SQLITE_DONE) {
      result.ok = true;
      break;
    }

    result.error = sqlite3_errmsg(db_);
    break;
  }

  sqlite3_finalize(stmt);

  if (result.ok) {
    result.changes = sqlite3_changes(db_);
    result.last_insert_id = sqlite3_last_insert_rowid(db_);
  }

  return result;
}

SqlResult SqlDatabase::Execute(const std::string& sql, const std::vector<SqlValue>& params) {
  std::lock_guard<std::mutex> lock(mutex_);
  return Run(sql, params, false);
}

SqlResult SqlDatabase::Query(const std::string& sql, const std::vector<SqlValue>& params) {
  std::lock_guard<std::mutex> lock(mutex_);
  return Run(sql, params, true);
}

bool SqlDatabase::Begin(std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    error = "database is not open";
    return false;
  }

  if (transactionDepth_ > 0) {
    ++transactionDepth_;
    return true;
  }

  // IMMEDIATE, not DEFERRED: the write lock is taken now rather than at the
  // first write, so two processes racing to assign the last free locker fail
  // fast at BEGIN instead of one of them failing at COMMIT with its work
  // already done.
  if (!ExecUnlocked("BEGIN IMMEDIATE;", error)) return false;

  transactionDepth_ = 1;
  return true;
}

bool SqlDatabase::Commit(std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (transactionDepth_ == 0) {
    error = "no transaction in progress";
    return false;
  }

  if (transactionDepth_ > 1) {
    --transactionDepth_;
    return true;
  }

  if (!ExecUnlocked("COMMIT;", error)) return false;

  transactionDepth_ = 0;
  return true;
}

bool SqlDatabase::Rollback(std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (transactionDepth_ == 0) {
    error = "no transaction in progress";
    return false;
  }

  // Unwinds the whole nesting, however deep -- see the header.
  transactionDepth_ = 0;
  return ExecUnlocked("ROLLBACK;", error);
}

std::string SqlDatabase::Quote(const std::string& text) {
  std::string out = "'";
  for (char c : text) {
    if (c == '\'') out += '\'';
    out += c;
  }
  out += "'";
  return out;
}

bool SqlDatabase::IsIdentifier(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  if (std::isdigit(static_cast<unsigned char>(name.front()))) return false;
  for (char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return true;
}

}  // namespace hsf
