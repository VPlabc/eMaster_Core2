#include "hsf/Logger.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace hsf {

namespace {
std::string NowTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &t);
#else
  localtime_r(&t, &tmBuf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms.count();
  return oss.str();
}

// Clamped rather than trusted. The array is sized from LogCategory::kCount, so
// this can only trip on a value cast in from outside the enum (CategoryFromString
// never produces one) -- and the cost of being wrong here is memory corruption
// in a logger, which is the worst possible place to look for it.
size_t CategoryIndex(LogCategory category) {
  const size_t index = static_cast<size_t>(category);
  return index < static_cast<size_t>(LogCategory::kCount) ? index : 0;
}
}  // namespace

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

void Logger::Init(const std::string& logDirectory, size_t ringCapacityPerCategory) {
  std::lock_guard<std::mutex> lock(mutex_);
  ringCapacity_ = ringCapacityPerCategory;
  std::filesystem::create_directories(logDirectory);
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &t);
#else
  localtime_r(&t, &tmBuf);
#endif
  std::ostringstream name;
  name << logDirectory << "/gateway_" << std::put_time(&tmBuf, "%Y%m%d_%H%M%S") << ".log";
  file_.open(name.str(), std::ios::out | std::ios::app);
}

void Logger::Log(LogCategory category, LogLevel level, const std::string& message) {
  // Dropped before the timestamp is even formatted: with DEBUG off this is the
  // whole cost of a Log.Debug() call, which matters because some of them sit in
  // per-iteration paths (serial reads, PLC input polls).
  if (level < minLevel_.load()) return;

  LogEntry entry{0, NowTimestamp(), level, category, message};

  std::lock_guard<std::mutex> lock(mutex_);
  // Assigned under the lock so concurrent loggers can't be handed the same
  // number -- the viewer relies on seq being unique to avoid duplicates.
  entry.seq = nextSeq_++;
  auto& ring = ringByCategory_[CategoryIndex(category)];
  ring.push_back(entry);
  while (ring.size() > ringCapacity_) ring.pop_front();

  std::ostringstream line;
  line << '[' << entry.timestamp << "] [" << LevelToString(level) << "] [" << CategoryToString(category) << "] "
       << message;

  std::ostream& out = (level == LogLevel::Error) ? std::cerr : std::cout;
  out << line.str() << std::endl;
  if (file_.is_open()) {
    file_ << line.str() << std::endl;
  }
}

std::vector<LogEntry> Logger::Recent(const LogCategory* category, size_t maxCount) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogEntry> result;
  if (category) {
    const auto& ring = ringByCategory_[CategoryIndex(*category)];
    size_t start = ring.size() > maxCount ? ring.size() - maxCount : 0;
    result.assign(ring.begin() + static_cast<long>(start), ring.end());
    return result;
  }

  for (const auto& ring : ringByCategory_) {
    result.insert(result.end(), ring.begin(), ring.end());
  }
  std::sort(result.begin(), result.end(),
            [](const LogEntry& a, const LogEntry& b) { return a.timestamp < b.timestamp; });
  if (result.size() > maxCount) {
    result.erase(result.begin(), result.end() - static_cast<long>(maxCount));
  }
  return result;
}

std::vector<LogEntry> Logger::Since(uint64_t afterSeq, size_t maxCount) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogEntry> result;
  for (const auto& ring : ringByCategory_) {
    for (const auto& entry : ring) {
      if (entry.seq > afterSeq) result.push_back(entry);
    }
  }
  // By seq, not timestamp: seq is the true order entries were created, and
  // timestamps only have millisecond resolution so several can tie.
  std::sort(result.begin(), result.end(),
            [](const LogEntry& a, const LogEntry& b) { return a.seq < b.seq; });
  if (result.size() > maxCount) {
    // Keep the NEWEST on overflow -- a client that fell far behind wants
    // current state, not an ancient prefix.
    result.erase(result.begin(), result.end() - static_cast<long>(maxCount));
  }
  return result;
}

uint64_t Logger::LatestSeq() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nextSeq_ - 1;
}

void Logger::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& ring : ringByCategory_) ring.clear();
  // nextSeq_ deliberately keeps counting: restarting it would make old
  // sequence numbers reappear, and a viewer still holding pre-clear entries
  // would treat new lines as already-seen and silently drop them.
}

std::string Logger::LevelToString(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
  }
  return "UNKNOWN";
}

std::string Logger::CategoryToString(LogCategory category) {
  switch (category) {
    case LogCategory::System: return "System";
    case LogCategory::Rest: return "Rest";
    case LogCategory::Serial: return "Serial";
    case LogCategory::Modbus: return "Modbus";
    case LogCategory::Rfid: return "Rfid";
    case LogCategory::Lua: return "Lua";
    case LogCategory::Card: return "Card";
    case LogCategory::Mq: return "Mq";
  }
  return "Unknown";
}

LogCategory Logger::CategoryFromString(const std::string& name) {
  if (name == "Rest") return LogCategory::Rest;
  if (name == "Serial") return LogCategory::Serial;
  if (name == "Modbus") return LogCategory::Modbus;
  if (name == "Rfid") return LogCategory::Rfid;
  if (name == "Lua") return LogCategory::Lua;
  if (name == "Card") return LogCategory::Card;
  if (name == "Mq") return LogCategory::Mq;
  return LogCategory::System;
}

}  // namespace hsf
