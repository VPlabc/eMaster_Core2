#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace hsf {

// Loads a config_schema.lua file in an isolated Lua state. The file must
// return a table; it cannot access the running project or gateway services.
bool LoadLuaConfigSchema(const std::string& path, nlohmann::json& schema, std::string& error);

}  // namespace hsf
