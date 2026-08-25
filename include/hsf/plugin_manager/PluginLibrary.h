#pragma once

#include <string>

#include "hsf/plugin.h"

namespace hsf {

// One loaded shared object, and nothing else.
//
// Deliberately narrow: it opens a file, resolves the four ABI symbols, checks
// the version, and closes. It knows nothing about manifests, lifecycle or
// configuration, which is what makes it the only place in the gateway that has
// to be correct about dlopen/LoadLibrary differences.
//
// UNLOAD ORDERING IS THE WHOLE POINT OF THIS CLASS. The library must not be
// closed while any of the plugin's code can still run: a thread still executing
// in unmapped memory is not a diagnosable crash, it is a stack trace pointing
// at nothing. So Close() is private to the destructor's ordering, and the owner
// must have destroyed the plugin instance first. PluginManager enforces that.
class PluginLibrary {
 public:
  PluginLibrary() = default;
  ~PluginLibrary();

  PluginLibrary(const PluginLibrary&) = delete;
  PluginLibrary& operator=(const PluginLibrary&) = delete;
  PluginLibrary(PluginLibrary&& other) noexcept;
  PluginLibrary& operator=(PluginLibrary&& other) noexcept;

  // Opens `path` and resolves the ABI. On failure returns false and fills
  // `error` with the platform's own message, which is the only thing that
  // distinguishes "file not found" from "undefined symbol" from "wrong
  // architecture" -- and an operator needs to know which.
  bool Open(const std::string& path, std::string& error);

  // Releases the handle. Safe to call twice. The caller MUST already have
  // destroyed the plugin instance.
  void Close();

  bool IsOpen() const { return handle_ != nullptr; }

  // The ABI version the binary reports, read via the exported symbol rather
  // than trusted from the manifest. The manifest is a text file that anyone can
  // edit; this is what the code was actually compiled against, and when the two
  // disagree the binary wins.
  uint32_t AbiVersion() const;

  const HSFPluginInfo* Info() const;

  HSFStatus Create(const HSFPluginContext* ctx, HSFPlugin** out_plugin,
                   const HSFPluginVTable** out_vtable) const;
  void Destroy(HSFPlugin* plugin) const;

  const std::string& Path() const { return path_; }

 private:
  void* Symbol(const char* name) const;

  void*       handle_ = nullptr;
  std::string path_;

  uint32_t (*abi_version_)() = nullptr;
  const HSFPluginInfo* (*get_info_)() = nullptr;
  HSFStatus (*create_)(const HSFPluginContext*, HSFPlugin**, const HSFPluginVTable**) = nullptr;
  void (*destroy_)(HSFPlugin*) = nullptr;
};

}  // namespace hsf
