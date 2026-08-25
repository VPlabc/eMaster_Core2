#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace hsf {

enum class LogLevel { Debug, Info, Warning, Error };
// kCount must stay last: it sizes the per-category ring-buffer array below, so
// a category added anywhere above it automatically gets a buffer. That array
// used to be a hardcoded [7], and adding `Mq` as an eighth category made the
// first Mq log line write one past the end -- an access violation that killed
// the gateway seconds after the broker was enabled, with no log line to show
// for it (the crash *was* the logging).
enum class LogCategory { System, Rest, Serial, Modbus, Rfid, Lua, Card, Mq, kCount };

struct LogEntry {
  // Monotonic across every category, assigned when the entry is created.
  // This is what lets the Log Viewer follow the WebSocket stream without
  // duplicating or missing lines: entries are merged by time for display,
  // but "have I already shown this one" can only be answered by identity,
  // and timestamps have 1ms resolution and can collide.
  uint64_t seq = 0;
  std::string timestamp;
  LogLevel level;
  LogCategory category;
  std::string message;
};

// Thread-safe logger: writes to console + a rotating-by-run log file, and
// keeps an in-memory ring buffer per category so the web Log Viewer page can
// query recent entries without re-reading the file from disk.
class Logger {
 public:
  static Logger& Instance();

  void Init(const std::string& logDirectory, size_t ringCapacityPerCategory = 2000);

  // Entries below this level are dropped before they reach the console, the
  // file or the ring buffer. Defaults to Info, i.e. DEBUG is OFF: the debug
  // lines are diagnostic detail (per-step serial open timings, released port
  // claims, raw traffic) and each one costs a file write, so they are opt-in
  // via logging.debug_enabled on the Configuration page.
  void SetMinLevel(LogLevel level) { minLevel_.store(level); }
  LogLevel MinLevel() const { return minLevel_.load(); }

  void Log(LogCategory category, LogLevel level, const std::string& message);

  void Debug(LogCategory category, const std::string& message) { Log(category, LogLevel::Debug, message); }
  void Info(LogCategory category, const std::string& message) { Log(category, LogLevel::Info, message); }
  void Warning(LogCategory category, const std::string& message) { Log(category, LogLevel::Warning, message); }
  void Error(LogCategory category, const std::string& message) { Log(category, LogLevel::Error, message); }

  // Returns up to `maxCount` most recent entries, newest last. If category
  // is nullptr, returns entries across all categories merged by time.
  std::vector<LogEntry> Recent(const LogCategory* category, size_t maxCount) const;

  // Entries newer than `afterSeq`, across all categories, oldest first.
  // Used to push only what's new over the WebSocket instead of resending
  // the whole buffer every tick.
  std::vector<LogEntry> Since(uint64_t afterSeq, size_t maxCount) const;

  // Highest sequence number issued so far; a client can start following the
  // stream from here without replaying history.
  uint64_t LatestSeq() const;

  // Empties the in-memory ring buffers that feed the Log Viewer. The log
  // FILE is deliberately left alone -- it is the durable record, and a UI
  // button should not be able to destroy an audit trail.
  void Clear();

  static std::string LevelToString(LogLevel level);
  static std::string CategoryToString(LogCategory category);
  static LogCategory CategoryFromString(const std::string& name);

 private:
  Logger() = default;

  mutable std::mutex mutex_;
  std::ofstream file_;
  // Atomic and outside mutex_: Log() checks it on every call from every thread,
  // and the check has to be cheaper than the lock it is there to avoid taking.
  std::atomic<LogLevel> minLevel_{LogLevel::Info};
  size_t ringCapacity_ = 2000;
  std::deque<LogEntry> ringByCategory_[static_cast<size_t>(LogCategory::kCount)];
  uint64_t nextSeq_ = 1;
};

}  // namespace hsf
