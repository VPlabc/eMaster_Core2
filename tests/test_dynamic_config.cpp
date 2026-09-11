#include "hsf/DynamicConfigManager.h"

#include <cassert>
#include <filesystem>

int main() {
  const auto path = std::filesystem::temp_directory_path() / "hsf_dynamic_config_test.db";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  auto& manager = hsf::DynamicConfigManager::Instance();
  manager.SetDatabasePath(path.string());
  const nlohmann::json schema = {
      {"version", 1},
      {"fields", {{{"key", "host"}, {"type", "string"}, {"default", "localhost"}, {"required", true}},
                  {{"key", "port"}, {"type", "number"}, {"default", 1883}, {"min", 1}, {"max", 65535}},
                  {{"key", "password"}, {"type", "password"}, {"default", "secret"}}}}};
  std::string error;
  assert(manager.RegisterSchema("test", schema, error));
  assert(manager.Values("test").at("password") == "********");
  assert(manager.Values("test", false).at("port") == 1883);
  assert(!manager.Schema("test")["fields"][2].contains("default"));

  nlohmann::json errors;
  assert(!manager.SetValues("test", {{"host", "localhost"}, {"port", 70000}}, errors));
  assert(!errors.empty());
  assert(manager.SetValues("test", {{"host", "localhost"}, {"port", 5672}, {"password", "********"}}, errors));
  assert(manager.Values("test", false).at("password") == "secret");
  assert(manager.SetValue("test", "password", "changed", errors));
  assert(manager.Values("test", false).at("password") == "changed");

  std::filesystem::remove(path, ec);
  return 0;
}
