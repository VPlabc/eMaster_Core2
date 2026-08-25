#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace hsf {

// One bound parameter of a statement. Lua has no distinction between an
// integer and a float at the type level and no NULL of its own, so the binding
// layer decides which of these a value becomes and this type carries that
// decision down to sqlite3_bind_*.
struct SqlValue {
  enum class Kind { Null, Integer, Real, Text, Blob } kind = Kind::Null;
  int64_t integer = 0;
  double real = 0.0;
  std::string text;  // also holds blob bytes

  static SqlValue Null() { return SqlValue{}; }
  static SqlValue Int(int64_t v) {
    SqlValue value;
    value.kind = Kind::Integer;
    value.integer = v;
    return value;
  }
  static SqlValue Real(double v) {
    SqlValue value;
    value.kind = Kind::Real;
    value.real = v;
    return value;
  }
  static SqlValue Text(std::string v) {
    SqlValue value;
    value.kind = Kind::Text;
    value.text = std::move(v);
    return value;
  }
};

// Result of a statement that returns rows. `columns` is the column order as
// declared by the query, which the Lua binding also exposes so a caller can
// iterate a row deterministically instead of relying on pairs() order.
struct SqlResult {
  bool ok = false;
  std::string error;
  std::vector<std::string> columns;
  // One object per row, values typed as SQLite reported them (integer, real,
  // string, or null) rather than everything stringified -- a locker_number
  // that came back as "5" would break every numeric comparison in a script.
  nlohmann::json rows = nlohmann::json::array();
  int64_t changes = 0;
  int64_t last_insert_id = 0;
};

// A general-purpose SQLite handle for application data, as opposed to the
// three purpose-built stores the gateway already has (ConfigManager's
// config.db, LogStore's logs.db, CardClientManager's clients.db). This one
// owns no schema of its own: whoever opens it writes their own DDL. It exists
// because request/SmartLocker/SmartLockerPlan.md section 15 requires the
// application's own tables in SQLite, and until now Lua had no way to reach
// SQL at all -- see the Db.* bindings in LuaEngine.
//
// WAL is enabled on open, with a busy timeout. That combination is what lets
// the web server read the same file (its own connection, opened read-only)
// while a script writes to it: under the default rollback journal a reader and
// a writer lock each other out, and the dashboard would intermittently 500
// while a card was being processed.
//
// Thread-safe: one mutex guards the connection and every public method takes
// it. Statements are prepared per call and finalized before returning -- no
// statement outlives the lock, so there is no cursor to invalidate.
class SqlDatabase {
 public:
  SqlDatabase() = default;
  ~SqlDatabase();
  SqlDatabase(const SqlDatabase&) = delete;
  SqlDatabase& operator=(const SqlDatabase&) = delete;

  // `path` is a filesystem path; the directory must exist. Opening again
  // closes the previous handle first, so a script may re-open a different
  // file. `readOnly` opens with SQLITE_OPEN_READONLY and does NOT create the
  // file -- that is how the web server attaches to a database a script owns.
  bool Open(const std::string& path, bool readOnly, std::string& error);
  void Close();
  bool IsOpen() const;
  std::string Path() const;

  // Statements that return no rows (DDL, INSERT/UPDATE/DELETE). `changes` and
  // `last_insert_id` in the result are filled from the connection afterwards.
  // Multiple statements separated by ';' are allowed only when `params` is
  // empty -- sqlite3_prepare_v2 compiles one statement at a time, and binding
  // parameters across a batch has no meaning.
  SqlResult Execute(const std::string& sql, const std::vector<SqlValue>& params);

  // Statements that return rows. A non-SELECT is not rejected: `INSERT ...
  // RETURNING id` legitimately returns one.
  SqlResult Query(const std::string& sql, const std::vector<SqlValue>& params);

  // BEGIN / COMMIT / ROLLBACK. Kept as named methods rather than left to
  // Execute("BEGIN") so the nesting counter below can be honest: SQLite has
  // no nested transactions, and a script whose release() runs inside its own
  // assign() must not commit the outer one. Nested Begin() increments a depth
  // counter and issues no SQL; Rollback() at any depth unwinds everything,
  // because an inner step that failed makes the outer one meaningless.
  bool Begin(std::string& error);
  bool Commit(std::string& error);
  bool Rollback(std::string& error);
  int TransactionDepth() const;

  // Escapes a string for embedding in SQL as a quoted literal, quotes
  // included. Provided for the cases parameter binding cannot cover (a table
  // name, an ORDER BY column); it is NOT the way to pass values -- those are
  // bound.
  static std::string Quote(const std::string& text);

  // True when `name` is a plain identifier (letters, digits, underscore, not
  // starting with a digit). The one guard that makes an interpolated table or
  // column name safe.
  static bool IsIdentifier(const std::string& name);

 private:
  SqlResult Run(const std::string& sql, const std::vector<SqlValue>& params, bool collectRows);
  bool ExecUnlocked(const std::string& sql, std::string& error);

  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
  std::string path_;
  bool readOnly_ = false;
  int transactionDepth_ = 0;
};

}  // namespace hsf
