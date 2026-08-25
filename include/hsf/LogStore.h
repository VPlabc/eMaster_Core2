#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace hsf {

// One field of one log type, as declared in the definitions JSON
// (request/upgrade.md section 8).
struct LogFieldDef {
  std::string name;
  // "string" | "integer" | "number" | "boolean". Anything else is rejected
  // when the definition is loaded rather than when a script first writes a
  // row -- a typo in the definitions file should surface at startup, not
  // days later on the one code path that happens to use that field.
  std::string type = "string";
  // Optional by default. Section 11 asks for required-field validation, but
  // making every declared field mandatory would mean a log type could never
  // carry a field that only applies to some outcomes (an error string on a
  // failure, say), so the definition says which ones matter.
  bool required = false;
};

struct LogTypeDef {
  std::string type;
  std::string description;
  std::vector<LogFieldDef> fields;

  const LogFieldDef* Find(const std::string& fieldName) const;
};

// Filters for the Logs page's structured search (request/upgrade.md
// sections 14 and 16). Every field is optional; an all-defaults query returns
// the newest `limit` entries.
struct LogQuery {
  std::vector<std::string> types;  // empty = every type
  std::string level;               // "" or "all" = every level
  std::string script;              // "" = every script
  std::string keyword;             // matched against field values, script name and log type
  std::string from;                // "YYYY-MM-DD HH:MM[:SS]" inclusive, "" = unbounded
  std::string to;                  // same, inclusive
  int limit = 50;
  int offset = 0;
};

// Structured business/application logs in SQLite, in the three-table shape
// request/upgrade.md section 9 asks for:
//
//   log_definitions  one row per log type, with its field list as JSON
//   log_entries      one row per logged event (type, script, level, time)
//   log_values       one row per field value of an entry
//
// The point of the entry/value split is that a script can define a new log
// type with its own fields without anything creating a new SQLite table --
// see Write(), which validates the supplied fields against the type's
// definition before inserting.
//
// This is deliberately NOT the runtime Logger: Log.Debug/Info/Warning/Error
// stay console + file + ring buffer for debugging (section 12), while these
// rows are the durable, searchable record. Both are reachable from Lua, under
// the same `Log` table, because from a script author's point of view they are
// two ways of saying "record this".
//
// Thread-safe: one mutex guards the connection, and every public method takes
// it. Lua scripts write from their own threads and the web server reads from
// Crow's, concurrently.
class LogStore {
 public:
  static LogStore& Instance();
  ~LogStore();

  // Opens (creating if necessary) the database at `path` and ensures the
  // schema exists. Safe to call again to point at a different file.
  bool Load(const std::string& path);
  bool IsOpen() const;

  // Reads log type definitions from `jsonPath` (see section 8's example) and
  // upserts them into log_definitions. Types already in the database that
  // are absent from the file are left alone: a deployment that dropped a
  // definition still has entries referencing it, and the Logs page needs the
  // field list to display them.
  //
  // Returns false only if the file can't be read or parsed. Individual
  // malformed type entries are logged and skipped, so one bad type doesn't
  // cost the others.
  bool LoadDefinitions(const std::string& jsonPath);

  // Registers a type from code rather than from the definitions file. Used
  // for the built-in `script_error` type (section 23) so Lua runtime faults
  // are recorded even on an installation whose definitions file is missing.
  bool DefineType(const LogTypeDef& def);

  std::vector<LogTypeDef> Definitions() const;
  bool FindDefinition(const std::string& logType, LogTypeDef& out) const;

  // Validates `fields` against the definition of `logType` and inserts one
  // entry plus its values. Returns false with `error` set on an unknown log
  // type, a missing required field, an unknown field, or a value that doesn't
  // parse as the declared type -- section 11's step list, in order.
  //
  // `level` is stored as-is (upper-cased); "" becomes "INFO".
  bool Write(const std::string& logType, const std::string& scriptName, const std::string& level,
             const std::map<std::string, std::string>& fields, std::string& error);

  // { total, limit, offset, entries: [ { id, timestamp, log_type, script_name,
  //   level, fields: { name: value } } ] }, newest first. `total` is the
  // match count ignoring limit/offset, which is what the pager needs.
  nlohmann::json Query(const LogQuery& query) const;

  // One entry with all of its fields, for the detail view (section 15).
  // Null when there is no such id.
  nlohmann::json Entry(int64_t id) const;

  // Distinct script names that have written entries, for the Script filter.
  std::vector<std::string> Scripts() const;

  // Deletes entries (and their values) older than `days`. No-op when days
  // <= 0. Returns the number of entries removed.
  int64_t PruneOlderThan(int days);

  // Total entry count, for the Logs page header.
  int64_t Count() const;

 private:
  LogStore() = default;
  LogStore(const LogStore&) = delete;
  LogStore& operator=(const LogStore&) = delete;

  bool EnsureSchemaUnlocked();
  bool UpsertDefinitionUnlocked(const LogTypeDef& def);
  bool LoadDefinitionsFromDbUnlocked();

  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
  std::string path_;
  // Mirror of log_definitions, so Write() can validate without a query per
  // call -- a polling script writing a row per card would otherwise re-read
  // its own definition every time.
  std::map<std::string, LogTypeDef> definitions_;
};

}  // namespace hsf
