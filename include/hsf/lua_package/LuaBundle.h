#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hsf {

// A whole Lua application as one payload: the entry chunk plus every module it
// require()s, each compiled separately.
//
// WHY THIS EXISTS. A package holding a single compiled chunk would be useless
// for the application this gateway actually runs. config/scripts/smartlocker/
// is a dozen files wired together with require(), and require() resolves
// through package.path against the FILESYSTEM -- so a deployed single-chunk
// package would compile cleanly, verify cleanly, load cleanly, and then fail
// on its first require() with "module 'locker' not found", having shipped
// exactly the plaintext source it was supposed to replace as the only way to
// make it work.
//
// So the bundle carries the modules, and LuaEngine installs them into
// package.preload before the entry chunk runs. require("locker") then finds a
// preloaded loader and never touches the disk -- which is also what keeps the
// promise in section 2.7 that nothing is written back to persistent storage.
//
// WIRE FORMAT (this is the plaintext that gets encrypted, so it is never seen
// outside the gateway; it is little-endian and deliberately dull):
//
//   u32  module count
//   repeated:
//     u16 name length, name bytes        module name as require() spells it
//     u32 code length, bytecode bytes
//   u16  entry name length, entry name
//
// Module names are the require() name, not a path: "smartlocker.locker", not
// "smartlocker/locker.lua". The packager does that translation once, so the
// device never has to guess how a path maps to a module.
class LuaBundle {
 public:
  struct Module {
    std::string name;      // as require() spells it
    std::string source_path;  // where it came from, for diagnostics only
    std::vector<unsigned char> bytecode;
  };

  void SetEntry(const std::string& name) { entry_ = name; }
  const std::string& Entry() const { return entry_; }

  void AddModule(const std::string& name, const std::string& sourcePath,
                 std::vector<unsigned char> bytecode);

  const std::vector<Module>& Modules() const { return modules_; }
  size_t Count() const { return modules_.size(); }

  // The entry module's bytecode, or empty when the entry is not among the
  // modules (which Parse rejects, so this only bites a caller building one by
  // hand).
  const std::vector<unsigned char>* EntryBytecode() const;

  std::vector<unsigned char> Serialise() const;

  // Every length is checked against the buffer before it is used. This runs on
  // data that has already had its signature verified, so it is not a hostile
  // input path -- but "already verified" is exactly the assumption that turns
  // out to be wrong later, and the checks cost nothing.
  static bool Parse(const std::vector<unsigned char>& data, LuaBundle& out, std::string& error);

  // Turns a path relative to the application root into a require() name:
  // "smartlocker/locker.lua" -> "smartlocker.locker", and
  // "smartlocker/init.lua"   -> "smartlocker". Mirrors what package.path's
  // "?.lua;?/init.lua" patterns would have resolved.
  static std::string ModuleNameForPath(const std::string& relativePath);

 private:
  std::vector<Module> modules_;
  std::string entry_;
};

}  // namespace hsf
