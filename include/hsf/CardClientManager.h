#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace hsf {

struct CardClient {
  int64_t id = 0;
  std::string name;
  std::string api_key;
  bool enabled = true;
  int64_t created_at = 0;   // unix seconds
  int64_t last_access = 0;  // unix seconds, 0 = never
  int64_t expires_at = 0;   // unix seconds, 0 = no expiration
};

// Manages API-Key-authenticated card reader clients (see
// request/Request/cardInputRESTAPI.md): stores the client table in its own
// SQLite database (default config/clients.db, a sibling of config.db) and
// authenticates incoming API-Keys for POST /api/card/input.
class CardClientManager {
 public:
  static CardClientManager& Instance();
  ~CardClientManager();

  bool Load(const std::string& path);

  // Generates a random API-Key and inserts a new, enabled client.
  std::optional<CardClient> CreateClient(const std::string& name, int64_t expiresAt);
  bool SetEnabled(int64_t id, bool enabled);
  bool DeleteClient(int64_t id);
  std::vector<CardClient> ListClients() const;

  // Looks up `apiKey`; returns the client if it exists, is enabled, and
  // hasn't expired, and records this call as its last access. Returns
  // std::nullopt for any other case (unknown, disabled, or expired key).
  std::optional<CardClient> Authenticate(const std::string& apiKey);

  static nlohmann::json ToJson(const CardClient& client);

 private:
  CardClientManager() = default;
  CardClientManager(const CardClientManager&) = delete;
  CardClientManager& operator=(const CardClientManager&) = delete;

  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
};

}  // namespace hsf
