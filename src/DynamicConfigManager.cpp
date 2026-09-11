#include "hsf/DynamicConfigManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <set>

namespace hsf {

using json = nlohmann::json;
namespace {
constexpr const char* kMasked = "********";

bool Exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

bool BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

}  // namespace

DynamicConfigManager& DynamicConfigManager::Instance() {
  static DynamicConfigManager manager;
  return manager;
}

DynamicConfigManager::~DynamicConfigManager() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) sqlite3_close(db_);
}

void DynamicConfigManager::SetDatabasePath(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (databasePath_ == path) return;
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
  databasePath_ = path;
  schemas_.clear();
  values_.clear();
}

bool DynamicConfigManager::EnsureDatabaseUnlocked() const {
  if (db_) return true;
  if (databasePath_.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(databasePath_).parent_path(), ec);
  if (sqlite3_open(databasePath_.c_str(), &db_) != SQLITE_OK) {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  return Exec(db_,
              "CREATE TABLE IF NOT EXISTS lua_dynamic_config ("
              "project_id TEXT PRIMARY KEY, schema_version INTEGER NOT NULL, "
              "schema_json TEXT NOT NULL, values_json TEXT NOT NULL, updated_at INTEGER NOT NULL);");
}

void DynamicConfigManager::CollectFields(const json& schema, std::vector<json>& fields) {
  if (schema.contains("fields") && schema["fields"].is_array())
    for (const auto& field : schema["fields"]) fields.push_back(field);
  if (schema.contains("groups") && schema["groups"].is_array())
    for (const auto& group : schema["groups"]) CollectFields(group, fields);
}

bool DynamicConfigManager::IsSecret(const json& field) {
  const std::string type = field.value("type", "string");
  return type == "password" || type == "secret" || field.value("secret", false);
}

void DynamicConfigManager::RemoveSecretDefaults(json& schema) {
  if (schema.contains("fields") && schema["fields"].is_array()) {
    for (auto& field : schema["fields"])
      if (IsSecret(field)) field.erase("default");
  }
  if (schema.contains("groups") && schema["groups"].is_array()) {
    for (auto& group : schema["groups"]) RemoveSecretDefaults(group);
  }
}

json DynamicConfigManager::DefaultsUnlocked(const json& schema) const {
  json result = json::object();
  std::function<json(const json&)> defaults = [&](const json& node) {
    json object = json::object();
    if (node.contains("fields") && node["fields"].is_array()) {
      for (const auto& field : node["fields"]) {
        const std::string key = field.value("key", "");
        if (key.empty()) continue;
        if (field.contains("default")) object[key] = field["default"];
        else if (field.value("type", "") == "array_group") {
          object[key] = json::array();
          const int minimum = std::max(0, field.value("min_items", 0));
          for (int i = 0; i < minimum; ++i) object[key].push_back(defaults(field.value("item_schema", json::object())));
        }
      }
    }
    if (node.contains("groups") && node["groups"].is_array())
      for (const auto& group : node["groups"]) {
        json nested = defaults(group);
        for (auto& item : nested.items()) object[item.key()] = item.value();
      }
    return object;
  };
  result = defaults(schema);
  return result;
}

bool DynamicConfigManager::ValidateUnlocked(const json& schema, const json& values, json& errors) const {
  errors = json::array();
  if (!values.is_object()) {
    errors.push_back({{"code", "INVALID_TYPE"}, {"message", "configuration must be an object"}});
    return false;
  }
  std::function<void(const json&, const json&, const std::string&)> validate =
      [&](const json& node, const json& object, const std::string& prefix) {
        if (!object.is_object()) {
          errors.push_back({{"field", prefix}, {"code", "INVALID_TYPE"}, {"message", "value must be an object"}});
          return;
        }
        std::set<std::string> knownKeys;
        std::vector<json> fields;
        if (node.contains("fields") && node["fields"].is_array())
          for (const auto& field : node["fields"]) fields.push_back(field);
        if (node.contains("groups") && node["groups"].is_array())
          for (const auto& group : node["groups"]) {
            std::vector<json> nested;
            if (group.contains("fields") && group["fields"].is_array())
              for (const auto& field : group["fields"]) fields.push_back(field);
          }
        for (const auto& field : fields) {
          const std::string key = field.value("key", "");
          if (key.empty()) continue;
          knownKeys.insert(key);
          const std::string path = prefix.empty() ? key : prefix + "." + key;
          auto it = object.find(key);
          if (it == object.end() || it->is_null() || (it->is_string() && it->get<std::string>().empty())) {
            if (field.value("required", false)) errors.push_back({{"field", path}, {"code", "REQUIRED"}, {"message", "field is required"}});
            continue;
          }
          const std::string type = field.value("type", "string");
          if (type == "array_group") {
            if (!it->is_array()) { errors.push_back({{"field", path}, {"code", "INVALID_TYPE"}, {"message", "value must be an array"}}); continue; }
            const int count = static_cast<int>(it->size());
            if (count < field.value("min_items", 0) || (field.contains("max_items") && count > field["max_items"].get<int>()))
              errors.push_back({{"field", path}, {"code", "ITEM_COUNT"}, {"message", "array item count is outside the allowed range"}});
            const json itemSchema = field.value("item_schema", json::object());
            for (size_t i = 0; i < it->size(); ++i) validate(itemSchema, (*it)[i], path + "[" + std::to_string(i) + "]");
            continue;
          }
          bool typeOk = (type == "string" || type == "password" || type == "secret" || type == "textarea" || type == "url") ? it->is_string() : type == "number" ? it->is_number() : type == "boolean" ? it->is_boolean() : type == "select" ? it->is_string() : true;
          if (!typeOk) { errors.push_back({{"field", path}, {"code", "INVALID_TYPE"}, {"message", "value has the wrong type"}}); continue; }
          if (it->is_number()) {
            if (field.contains("min") && *it < field["min"]) errors.push_back({{"field", path}, {"code", "OUT_OF_RANGE"}, {"message", "value is below the minimum"}});
            if (field.contains("max") && *it > field["max"]) errors.push_back({{"field", path}, {"code", "OUT_OF_RANGE"}, {"message", "value is above the maximum"}});
          }
          if (type == "url" && (it->get<std::string>().find("://") == std::string::npos || it->get<std::string>().find_first_of(" \t\r\n") != std::string::npos)) errors.push_back({{"field", path}, {"code", "INVALID_URL"}, {"message", "value must be a valid URL"}});
          if (type == "select" && field.contains("options") && field["options"].is_array()) {
            bool found = false; for (const auto& option : field["options"]) if (option.value("value", json()) == *it) found = true;
            if (!found) errors.push_back({{"field", path}, {"code", "NOT_ALLOWED"}, {"message", "value is not an allowed option"}});
          }
        }
        for (const auto& item : object.items()) if (!knownKeys.count(item.key())) errors.push_back({{"field", prefix.empty() ? item.key() : prefix + "." + item.key()}, {"code", "UNKNOWN_FIELD"}, {"message", "field is not present in the schema"}});
      };
  validate(schema, values, "");
  return errors.empty();
}

bool DynamicConfigManager::SaveUnlocked(const std::string& project, const json& schema,
                                        const json& values, int version) const {
  if (!EnsureDatabaseUnlocked()) return false;
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO lua_dynamic_config(project_id,schema_version,schema_json,values_json,updated_at) "
                    "VALUES(?,?,?,?,strftime('%s','now')) ON CONFLICT(project_id) DO UPDATE SET "
                    "schema_version=excluded.schema_version,schema_json=excluded.schema_json,values_json=excluded.values_json,updated_at=excluded.updated_at;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  BindText(stmt, 1, project); sqlite3_bind_int(stmt, 2, version);
  BindText(stmt, 3, schema.dump()); BindText(stmt, 4, values.dump());
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

void DynamicConfigManager::LoadProjectUnlocked(const std::string& project) const {
  if (schemas_.count(project) || !EnsureDatabaseUnlocked()) return;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT schema_json,values_json FROM lua_dynamic_config WHERE project_id=?", -1, &stmt, nullptr) != SQLITE_OK) return;
  BindText(stmt, 1, project);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    try {
      schemas_[project] = json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
      values_[project] = json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    } catch (...) {}
  }
  sqlite3_finalize(stmt);
}

bool DynamicConfigManager::RegisterSchema(const std::string& project, const json& schema, std::string& error) {
  if (project.empty() || !schema.is_object()) { error = "project and object schema are required"; return false; }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureDatabaseUnlocked()) { error = "dynamic configuration database unavailable"; return false; }
  LoadProjectUnlocked(project);
  json values = DefaultsUnlocked(schema);
  if (values_.count(project) && values_[project].is_object()) {
    std::vector<json> fields;
    CollectFields(schema, fields);
    std::set<std::string> knownKeys;
    for (const auto& field : fields) knownKeys.insert(field.value("key", ""));
    for (auto& item : values_[project].items())
      if (knownKeys.count(item.key())) values_[item.key()] = item.value();
  }
  json errors;
  if (!ValidateUnlocked(schema, values, errors)) { error = errors.dump(); return false; }
  const int version = schema.value("version", 1);
  json storedSchema = schema;
  RemoveSecretDefaults(storedSchema);
  if (!SaveUnlocked(project, storedSchema, values, version)) { error = "failed to persist schema"; return false; }
  schemas_[project] = storedSchema;
  values_[project] = values;
  return true;
}

bool DynamicConfigManager::HasProject(const std::string& project) const { return !Schema(project).is_null(); }

std::vector<std::string> DynamicConfigManager::Projects() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureDatabaseUnlocked()) return {};
  sqlite3_stmt* stmt = nullptr; std::vector<std::string> result;
  if (sqlite3_prepare_v2(db_, "SELECT project_id FROM lua_dynamic_config ORDER BY project_id", -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt); return result;
}

json DynamicConfigManager::Schema(const std::string& project) const { std::lock_guard<std::mutex> lock(mutex_); LoadProjectUnlocked(project); return schemas_.count(project) ? schemas_[project] : json(); }

json DynamicConfigManager::Values(const std::string& project, bool maskSecrets) const {
  std::lock_guard<std::mutex> lock(mutex_); LoadProjectUnlocked(project);
  if (!values_.count(project)) return json();
  json result = values_[project];
  if (maskSecrets) { std::vector<json> fields; CollectFields(schemas_[project], fields); for (const auto& f : fields) if (IsSecret(f) && result.contains(f.value("key", ""))) result[f.value("key", "")] = kMasked; }
  return result;
}

bool DynamicConfigManager::SetValues(const std::string& project, const json& incoming, json& errors) {
  std::lock_guard<std::mutex> lock(mutex_); LoadProjectUnlocked(project);
  if (!schemas_.count(project) || !incoming.is_object()) { errors = {{"code", "NOT_FOUND"}, {"message", "unknown project or invalid object"}}; return false; }
  // Accept both wire formats. New renderers send schema keys literally
  // (for example "plc.port"), while older cached pages sent nested objects
  // (for example {"plc":{"port":502}}). Normalize nested objects here so
  // a browser refresh is not required for an otherwise valid update.
  json flat = json::object();
  std::function<void(const json&, const std::string&)> flatten = [&](const json& node, const std::string& prefix) {
    for (const auto& item : node.items()) {
      const std::string key = prefix.empty() ? item.key() : prefix + "." + item.key();
      if (item.value().is_object()) flatten(item.value(), key);
      else flat[key] = item.value();
    }
  };
  flatten(incoming, "");
  json next = values_[project];
  std::vector<json> fields; CollectFields(schemas_[project], fields);
  for (auto& item : flat.items()) {
    bool secret = false; for (const auto& f : fields) if (f.value("key", "") == item.key()) secret = IsSecret(f);
    if (secret && item.value().is_string() && item.value() == kMasked) continue;
    next[item.key()] = item.value();
  }
  if (!ValidateUnlocked(schemas_[project], next, errors)) return false;
  if (!SaveUnlocked(project, schemas_[project], next, schemas_[project].value("version", 1))) { errors = {{"code", "PERSISTENCE"}, {"message", "failed to save configuration"}}; return false; }
  values_[project] = next; return true;
}

bool DynamicConfigManager::SetValue(const std::string& project, const std::string& key, const json& value, json& errors) {
  return SetValues(project, json{{key, value}}, errors);
}

}  // namespace hsf
