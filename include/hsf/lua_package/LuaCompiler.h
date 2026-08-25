#pragma once

#include <string>
#include <vector>

namespace hsf {

// Compiles Lua source to bytecode with the gateway's OWN embedded interpreter
// (request/AdvanceUpdate.md sections 2.2 and 2.3).
//
// Not by shelling out to `luac`. Lua bytecode is not portable: it is tied to
// the interpreter's version, its number and pointer widths, and its endianness,
// and the header check on load is a hard reject rather than a best-effort
// conversion. A `luac` that happens to be on the build machine may be 5.3, or
// 64-bit where the gateway is 32-bit, and the failure would surface as
// "bad binary format" on the device at deploy time rather than at compile
// time. Dumping through the interpreter that will later load it makes that
// class of mistake impossible: the producer and the consumer are the same
// library, in the same process.
//
// The chunk is compiled in a bare lua_State with no libraries opened. Nothing
// here runs the script -- luaL_loadbuffer only parses -- but a state with no
// io, os or package loaded cannot be induced to do anything even if a future
// change accidentally executes something.
class LuaCompiler {
 public:
  struct Result {
    bool ok = false;
    std::vector<unsigned char> bytecode;
    // Lua's own message, e.g. `main.lua:14: '=' expected near 'x'`. Passed
    // through verbatim -- it is the single most useful thing the compile
    // button can show, and it names a line the author can go to.
    std::string error;
    int error_line = 0;
    size_t source_bytes = 0;
  };

  // `chunkName` becomes the name in runtime error messages, so pass the script
  // path. A leading '@' is added if absent, which is what tells Lua to print
  // it bare rather than as [string "..."].
  //
  // `strip` discards debug information: smaller output, and it removes local
  // variable names and the line table from the artifact. It also removes them
  // from crash reports, so it is off for the test build and on for the
  // production one.
  static Result Compile(const std::string& source, const std::string& chunkName, bool strip);

  // Syntax check only, no bytecode. What the editor's existing validate button
  // wants.
  static Result Validate(const std::string& source, const std::string& chunkName);

  // The bytecode signature this interpreter accepts: LUA_SIGNATURE plus the
  // version and format bytes. Stored in a package header so a mismatch is
  // caught when the package is opened, with an explanation, instead of as
  // "bad binary format" from deep inside the loader.
  static std::string RuntimeTag();

  // True if `data` starts with the Lua binary-chunk signature (ESC "Lua").
  // Used to keep a source file from being mistaken for a compiled one and the
  // other way round.
  static bool LooksLikeBytecode(const std::vector<unsigned char>& data);
};

}  // namespace hsf
