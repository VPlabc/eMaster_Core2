#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace hsf {

struct CardEntry {
  std::string uid;
  int position = 0;
  int64_t timestamp = 0;  // unix seconds
};

// Single-slot cache for the most recent card UID received via
// POST /api/card/input (see request/Request/cardInputRESTAPI.md), read by
// the Lua Card.* bindings. Deliberately a single slot rather than a queue:
// the spec's Lua workflow polls Card.Available() then Card.Get(), and calls
// Card.Clear() once consumed.
class CardCache {
 public:
  static CardCache& Instance();

  void Set(const std::string& uid, int position);
  std::optional<CardEntry> Get() const;
  void Clear();
  bool Available() const;

  static nlohmann::json ToJson(const CardEntry& entry);

 private:
  CardCache() = default;

  mutable std::mutex mutex_;
  std::optional<CardEntry> entry_;
};

}  // namespace hsf
