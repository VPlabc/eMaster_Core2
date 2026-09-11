#include "hsf/LuaConfigSchemaLoader.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>

namespace hsf {
namespace {

nlohmann::json ToJson(lua_State* lua, int index) {
  index = lua_absindex(lua, index);
  switch (lua_type(lua, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return static_cast<bool>(lua_toboolean(lua, index));
    case LUA_TNUMBER:
      return lua_isinteger(lua, index) ? nlohmann::json(static_cast<int64_t>(lua_tointeger(lua, index)))
                                      : nlohmann::json(lua_tonumber(lua, index));
    case LUA_TSTRING: return std::string(lua_tostring(lua, index));
    case LUA_TTABLE: {
      lua_Integer max = 0;
      bool array = true;
      lua_pushnil(lua);
      while (lua_next(lua, index) != 0) {
        if (!lua_isinteger(lua, -2) || lua_tointeger(lua, -2) < 1) array = false;
        else max = std::max(max, lua_tointeger(lua, -2));
        lua_pop(lua, 1);
      }
      if (array) {
        for (lua_Integer i = 1; i <= max; ++i) {
          lua_geti(lua, index, i);
          if (lua_isnil(lua, -1)) array = false;
          lua_pop(lua, 1);
          if (!array) break;
        }
      }
      nlohmann::json result = array ? nlohmann::json::array() : nlohmann::json::object();
      if (array) {
        for (lua_Integer i = 1; i <= max; ++i) {
          lua_geti(lua, index, i);
          result.push_back(ToJson(lua, -1));
          lua_pop(lua, 1);
        }
      } else {
        lua_pushnil(lua);
        while (lua_next(lua, index) != 0) {
          if (lua_isstring(lua, -2)) result[lua_tostring(lua, -2)] = ToJson(lua, -1);
          lua_pop(lua, 1);
        }
      }
      return result;
    }
    default: return nullptr;
  }
}

}  // namespace

bool LoadLuaConfigSchema(const std::string& path, nlohmann::json& schema, std::string& error) {
  lua_State* lua = luaL_newstate();
  if (!lua) { error = "failed to create Lua state"; return false; }
  luaL_openlibs(lua);
  if (luaL_loadfile(lua, path.c_str()) != LUA_OK || lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    const char* message = lua_tostring(lua, -1);
    error = message ? message : "failed to evaluate schema";
    lua_close(lua);
    return false;
  }
  if (!lua_istable(lua, -1)) {
    error = "schema must return a table";
    lua_close(lua);
    return false;
  }
  schema = ToJson(lua, -1);
  lua_close(lua);
  if (!schema.is_object()) { error = "schema must return an object table"; return false; }
  return true;
}

}  // namespace hsf
