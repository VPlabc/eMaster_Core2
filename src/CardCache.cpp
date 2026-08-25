#include "hsf/CardCache.h"

#include <chrono>

namespace hsf {

using nlohmann::json;

namespace {
int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

CardCache& CardCache::Instance() {
  static CardCache instance;
  return instance;
}

void CardCache::Set(const std::string& uid, int position) {
  std::lock_guard<std::mutex> lock(mutex_);
  entry_ = CardEntry{uid, position, NowUnixSeconds()};
}

std::optional<CardEntry> CardCache::Get() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entry_;
}

void CardCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entry_.reset();
}

bool CardCache::Available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entry_.has_value();
}

json CardCache::ToJson(const CardEntry& entry) {
  return json{{"uid", entry.uid}, {"position", entry.position}, {"timestamp", entry.timestamp}};
}

}  // namespace hsf
