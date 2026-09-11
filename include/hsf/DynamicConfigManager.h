#pragma once

#include <mutex>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace hsf {

// Project-scoped, schema-driven configuration shared by Lua projects and the
// web API. Gateway configuration remains owned by ConfigManager; this class
// stores only Lua project schemas and values in the same SQLite database.
class DynamicConfigManager {
 public:
  static DynamicConfigManager& Instance();

  bool RegisterSchema(const std::string& project, const nlohmann::json& schema, std::string& error);
  bool HasProject(const std::string& project) const;
  std::vector<std::string> Projects() const;
  nlohmann::json Schema(const std::string& project) const;
  nlohmann::json Values(const std::string& project, bool maskSecrets = true) const;

  bool SetValues(const std::string& project, const nlohmann::json& values,
                nlohmann::json& errors);
  bool SetValue(const std::string& project, const std::string& key,
                const nlohmann::json& value, nlohmann::json& errors);

  void SetDatabasePath(const std::string& path);

 private:
  DynamicConfigManager() = default;
  ~DynamicConfigManager();
  DynamicConfigManager(const DynamicConfigManager&) = delete;
  DynamicConfigManager& operator=(const DynamicConfigManager&) = delete;

  bool EnsureDatabaseUnlocked() const;
  bool SaveUnlocked(const std::string& project, const nlohmann::json& schema,
                    const nlohmann::json& values, int version) const;
  bool ValidateUnlocked(const nlohmann::json& schema, const nlohmann::json& values,
                        nlohmann::json& errors) const;
  nlohmann::json DefaultsUnlocked(const nlohmann::json& schema) const;
  void LoadProjectUnlocked(const std::string& project) const;
  static void CollectFields(const nlohmann::json& schema, std::vector<nlohmann::json>& fields);
  static bool IsSecret(const nlohmann::json& field);
  static void RemoveSecretDefaults(nlohmann::json& schema);

  mutable std::mutex mutex_;
  mutable sqlite3* db_ = nullptr;
  mutable std::string databasePath_;
  mutable std::map<std::string, nlohmann::json> schemas_;
  mutable std::map<std::string, nlohmann::json> values_;
};

}  // namespace hsf
