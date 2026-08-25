#include "hsf/lua_package/LuaCompiler.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hsf {
namespace {

// lua_dump hands each block to this; append it to the vector behind `ud`.
int AppendWriter(lua_State*, const void* data, size_t size, void* ud) {
  auto* out = static_cast<std::vector<unsigned char>*>(ud);
  const auto* bytes = static_cast<const unsigned char*>(data);
  out->insert(out->end(), bytes, bytes + size);
  return 0;  // non-zero would abort the dump
}

// Lua prefixes syntax errors with "chunkname:LINE:". Pull the line out so the
// editor can jump to it, without disturbing the message itself.
int ParseErrorLine(const std::string& message, const std::string& chunkName) {
  std::string bare = chunkName;
  if (!bare.empty() && (bare[0] == '@' || bare[0] == '=')) bare.erase(0, 1);

  const size_t start = (bare.empty() || message.rfind(bare, 0) != 0) ? 0 : bare.size();
  if (start >= message.size() || message[start] != ':') return 0;

  size_t cursor = start + 1;
  int line = 0;
  while (cursor < message.size() && message[cursor] >= '0' && message[cursor] <= '9') {
    line = line * 10 + (message[cursor] - '0');
    ++cursor;
    if (line > 10000000) return 0;
  }
  return (cursor > start + 1) ? line : 0;
}

std::string NormaliseChunkName(const std::string& chunkName) {
  if (chunkName.empty()) return "=[buffer]";
  if (chunkName[0] == '@' || chunkName[0] == '=') return chunkName;
  return "@" + chunkName;
}

// A lua_State with no libraries. luaL_loadbuffer parses without executing, so
// nothing in the chunk can run here -- but a state that has no io, os or
// package to reach for cannot be talked into it either.
struct BareState {
  lua_State* L = nullptr;
  BareState() : L(luaL_newstate()) {}
  ~BareState() {
    if (L) lua_close(L);
  }
};

}  // namespace

LuaCompiler::Result LuaCompiler::Compile(const std::string& source, const std::string& chunkName,
                                         bool strip) {
  Result result;
  result.source_bytes = source.size();

  const std::string name = NormaliseChunkName(chunkName);

  // Before the load, not after. Round-tripping bytecode through the compiler
  // is legal in Lua and silently yields a smaller stripped copy, so "compile
  // the source" would quietly succeed on a file containing no source at all --
  // and it would mean handing the loader a binary chunk we have not
  // authenticated, which is the one input Lua's loader is not hardened
  // against.
  if (source.size() >= 4 && std::memcmp(source.data(), LUA_SIGNATURE, 4) == 0) {
    result.error = "this file is already compiled bytecode, not Lua source";
    return result;
  }

  BareState state;
  if (!state.L) {
    result.error = "out of memory creating a Lua state";
    return result;
  }

  if (luaL_loadbuffer(state.L, source.data(), source.size(), name.c_str()) != LUA_OK) {
    const char* message = lua_tostring(state.L, -1);
    result.error = message ? message : "unknown compile error";
    result.error_line = ParseErrorLine(result.error, name);
    return result;
  }

  if (lua_dump(state.L, AppendWriter, &result.bytecode, strip ? 1 : 0) != 0) {
    result.error = "failed to serialise the compiled chunk";
    result.bytecode.clear();
    return result;
  }
  if (result.bytecode.empty()) {
    result.error = "the compiler produced no output";
    return result;
  }

  result.ok = true;
  return result;
}

LuaCompiler::Result LuaCompiler::Validate(const std::string& source, const std::string& chunkName) {
  Result result;
  result.source_bytes = source.size();
  const std::string name = NormaliseChunkName(chunkName);

  BareState state;
  if (!state.L) {
    result.error = "out of memory creating a Lua state";
    return result;
  }
  if (luaL_loadbuffer(state.L, source.data(), source.size(), name.c_str()) != LUA_OK) {
    const char* message = lua_tostring(state.L, -1);
    result.error = message ? message : "unknown compile error";
    result.error_line = ParseErrorLine(result.error, name);
    return result;
  }
  result.ok = true;
  return result;
}

std::string LuaCompiler::RuntimeTag() {
  // LUA_SIGNATURE is "\x1bLua"; the two bytes after it are the version
  // (0x54 for 5.4) and the format. Rendered printable so it can live in a
  // JSON header and be compared as a string.
  //
  // The pointer width matters too and is NOT in those bytes -- 5.4 records the
  // sizes of instruction/integer/number in the header it checks at load time.
  // Including sizeof(void*) here makes a 32-bit-vs-64-bit mismatch a clear
  // message from us rather than "truncated precompiled chunk" from the loader.
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "lua%d.%d-i%zu-n%zu-p%zu", LUA_VERSION_NUM / 100,
                LUA_VERSION_NUM % 100, sizeof(lua_Integer), sizeof(lua_Number), sizeof(void*));
  return buffer;
}

bool LuaCompiler::LooksLikeBytecode(const std::vector<unsigned char>& data) {
  return data.size() >= 4 && std::memcmp(data.data(), LUA_SIGNATURE, 4) == 0;
}

}  // namespace hsf
