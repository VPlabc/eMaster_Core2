#include "hsf/lua_package/LuaBundle.h"

#include <algorithm>
#include <cstring>

namespace hsf {
namespace {

void AppendU16(std::vector<unsigned char>& out, uint16_t value) {
  out.push_back(static_cast<unsigned char>(value & 0xFF));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<unsigned char>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xFF));
}

bool ReadU16(const std::vector<unsigned char>& in, size_t& offset, uint16_t& out) {
  if (offset + 2 > in.size()) return false;
  out = static_cast<uint16_t>(in[offset]) | static_cast<uint16_t>(in[offset + 1]) << 8;
  offset += 2;
  return true;
}

bool ReadU32(const std::vector<unsigned char>& in, size_t& offset, uint32_t& out) {
  if (offset + 4 > in.size()) return false;
  out = 0;
  for (int i = 0; i < 4; ++i) out |= static_cast<uint32_t>(in[offset + i]) << (i * 8);
  offset += 4;
  return true;
}

bool ReadBytes(const std::vector<unsigned char>& in, size_t& offset, size_t count,
               std::vector<unsigned char>& out) {
  if (count > in.size() || offset + count > in.size()) return false;
  out.assign(in.begin() + offset, in.begin() + offset + count);
  offset += count;
  return true;
}

constexpr uint32_t kMaxModules = 4096;
constexpr uint32_t kMaxModuleBytes = 16u * 1024 * 1024;

}  // namespace

void LuaBundle::AddModule(const std::string& name, const std::string& sourcePath,
                          std::vector<unsigned char> bytecode) {
  // Replace rather than append on a repeated name: two loaders for one module
  // name is ambiguous, and silently keeping the first would make a rebuild
  // that renamed a file behave differently from a clean build.
  for (Module& existing : modules_) {
    if (existing.name == name) {
      existing.source_path = sourcePath;
      existing.bytecode = std::move(bytecode);
      return;
    }
  }
  modules_.push_back(Module{name, sourcePath, std::move(bytecode)});
}

const std::vector<unsigned char>* LuaBundle::EntryBytecode() const {
  for (const Module& module : modules_) {
    if (module.name == entry_) return &module.bytecode;
  }
  return nullptr;
}

std::vector<unsigned char> LuaBundle::Serialise() const {
  std::vector<unsigned char> out;
  size_t estimate = 6;
  for (const Module& module : modules_) estimate += 6 + module.name.size() + module.bytecode.size();
  out.reserve(estimate + entry_.size());

  AppendU32(out, static_cast<uint32_t>(modules_.size()));
  for (const Module& module : modules_) {
    AppendU16(out, static_cast<uint16_t>(module.name.size()));
    out.insert(out.end(), module.name.begin(), module.name.end());
    AppendU32(out, static_cast<uint32_t>(module.bytecode.size()));
    out.insert(out.end(), module.bytecode.begin(), module.bytecode.end());
  }
  AppendU16(out, static_cast<uint16_t>(entry_.size()));
  out.insert(out.end(), entry_.begin(), entry_.end());
  return out;
}

bool LuaBundle::Parse(const std::vector<unsigned char>& data, LuaBundle& out, std::string& error) {
  out = LuaBundle();
  size_t offset = 0;

  uint32_t count = 0;
  if (!ReadU32(data, offset, count)) {
    error = "bundle is truncated (no module count)";
    return false;
  }
  if (count == 0 || count > kMaxModules) {
    error = "bundle declares an implausible module count";
    return false;
  }

  for (uint32_t i = 0; i < count; ++i) {
    uint16_t nameLength = 0;
    if (!ReadU16(data, offset, nameLength) || nameLength == 0) {
      error = "bundle is truncated (module name length)";
      return false;
    }
    std::vector<unsigned char> nameBytes;
    if (!ReadBytes(data, offset, nameLength, nameBytes)) {
      error = "bundle is truncated (module name)";
      return false;
    }
    uint32_t codeLength = 0;
    if (!ReadU32(data, offset, codeLength)) {
      error = "bundle is truncated (module length)";
      return false;
    }
    if (codeLength == 0 || codeLength > kMaxModuleBytes) {
      error = "bundle module has an implausible length";
      return false;
    }
    std::vector<unsigned char> code;
    if (!ReadBytes(data, offset, codeLength, code)) {
      error = "bundle is truncated (module body)";
      return false;
    }
    const std::string moduleName(nameBytes.begin(), nameBytes.end());
    // Every module must actually be a compiled chunk. Cheap, and it is the
    // check that stops a malformed bundle reaching luaL_loadbuffer -- Lua's
    // loader is not hardened against hostile bytecode, and "the signature
    // verified" is doing a lot of work to get us here.
    if (code.size() < 4 || std::memcmp(code.data(), "\x1bLua", 4) != 0) {
      error = "bundle module \"" + moduleName + "\" is not compiled Lua bytecode";
      return false;
    }
    out.modules_.push_back(Module{moduleName, std::string(), std::move(code)});
  }

  uint16_t entryLength = 0;
  if (!ReadU16(data, offset, entryLength) || entryLength == 0) {
    error = "bundle is truncated (entry name)";
    return false;
  }
  std::vector<unsigned char> entryBytes;
  if (!ReadBytes(data, offset, entryLength, entryBytes)) {
    error = "bundle is truncated (entry name)";
    return false;
  }
  out.entry_ = std::string(entryBytes.begin(), entryBytes.end());

  if (offset != data.size()) {
    // Trailing bytes mean the payload is not what it claims. Nothing legitimate
    // produces them.
    error = "bundle has " + std::to_string(data.size() - offset) + " unexpected trailing bytes";
    return false;
  }
  if (out.EntryBytecode() == nullptr) {
    error = "bundle entry \"" + out.entry_ + "\" is not one of its modules";
    return false;
  }
  return true;
}

std::string LuaBundle::ModuleNameForPath(const std::string& relativePath) {
  std::string name = relativePath;
  std::replace(name.begin(), name.end(), '\\', '/');

  // Strip the extension.
  const size_t dot = name.rfind(".lua");
  if (dot != std::string::npos && dot + 4 == name.size()) name.erase(dot);

  // "pkg/init.lua" is how package.path's "?/init.lua" spells the module "pkg".
  const std::string initSuffix = "/init";
  if (name.size() > initSuffix.size() && name.compare(name.size() - initSuffix.size(), initSuffix.size(),
                                                      initSuffix) == 0) {
    name.erase(name.size() - initSuffix.size());
  } else if (name == "init") {
    // A bare init.lua at the root has no package to name; leave it as "init"
    // rather than producing an empty module name.
    return "init";
  }

  std::replace(name.begin(), name.end(), '/', '.');
  return name;
}

}  // namespace hsf
