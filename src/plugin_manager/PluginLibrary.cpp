#include "hsf/plugin_manager/PluginLibrary.h"

#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace hsf {
namespace {

#if defined(_WIN32)
std::string LastOsError() {
  const DWORD code = GetLastError();
  if (code == 0) return "no error";
  char* buffer = nullptr;
  const DWORD n = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string message = (n && buffer) ? std::string(buffer, n) : "error " + std::to_string(code);
  if (buffer) LocalFree(buffer);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  return message;
}
#else
std::string LastOsError() {
  const char* e = dlerror();
  return e ? std::string(e) : std::string("unknown dynamic-loader error");
}
#endif

}  // namespace

PluginLibrary::~PluginLibrary() { Close(); }

PluginLibrary::PluginLibrary(PluginLibrary&& other) noexcept { *this = std::move(other); }

PluginLibrary& PluginLibrary::operator=(PluginLibrary&& other) noexcept {
  if (this == &other) return *this;
  Close();
  handle_ = other.handle_;
  path_ = std::move(other.path_);
  abi_version_ = other.abi_version_;
  get_info_ = other.get_info_;
  create_ = other.create_;
  destroy_ = other.destroy_;
  // Cleared so the moved-from object's destructor does not close a handle this
  // one now owns.
  other.handle_ = nullptr;
  other.abi_version_ = nullptr;
  other.get_info_ = nullptr;
  other.create_ = nullptr;
  other.destroy_ = nullptr;
  return *this;
}

void* PluginLibrary::Symbol(const char* name) const {
  if (!handle_) return nullptr;
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
  return dlsym(handle_, name);
#endif
}

bool PluginLibrary::Open(const std::string& path, std::string& error) {
  Close();
  path_ = path;

#if defined(_WIN32)
  handle_ = static_cast<void*>(LoadLibraryA(path.c_str()));
#else
  // RTLD_LOCAL, not RTLD_GLOBAL: a plugin's symbols must not join the global
  // namespace, or two plugins carrying differently-built copies of the same
  // helper resolve to whichever loaded first. That is a real crash that only
  // appears once a second plugin is installed, and it is very hard to diagnose.
  //
  // RTLD_NOW rather than RTLD_LAZY so an unresolved symbol is an error here,
  // with a message, instead of a SIGSEGV on the first call to it months later.
  dlerror();  // clear any stale error before we attribute one to this call
  handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

  if (!handle_) {
    error = "cannot load " + path + ": " + LastOsError();
    return false;
  }

  abi_version_ = reinterpret_cast<uint32_t (*)()>(Symbol("hsf_plugin_abi_version"));
  get_info_ = reinterpret_cast<const HSFPluginInfo* (*)()>(Symbol("hsf_plugin_get_info"));
  create_ = reinterpret_cast<HSFStatus (*)(const HSFPluginContext*, HSFPlugin**,
                                           const HSFPluginVTable**)>(Symbol("hsf_plugin_create"));
  destroy_ = reinterpret_cast<void (*)(HSFPlugin*)>(Symbol("hsf_plugin_destroy"));

  // Named individually, because "missing hsf_plugin_create" tells the author
  // they forgot HSF_PLUGIN_DEFINE, while "not a plugin" tells them nothing.
  const char* missing = nullptr;
  if (!abi_version_) missing = "hsf_plugin_abi_version";
  else if (!get_info_) missing = "hsf_plugin_get_info";
  else if (!create_) missing = "hsf_plugin_create";
  else if (!destroy_) missing = "hsf_plugin_destroy";
  if (missing) {
    error = std::string(path) + " does not export " + missing +
            " (is it built with HSF_PLUGIN_DEFINE and default visibility on the entry points?)";
    Close();
    return false;
  }
  return true;
}

void PluginLibrary::Close() {
  if (!handle_) return;
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle_));
#else
  dlclose(handle_);
#endif
  handle_ = nullptr;
  abi_version_ = nullptr;
  get_info_ = nullptr;
  create_ = nullptr;
  destroy_ = nullptr;
}

uint32_t PluginLibrary::AbiVersion() const { return abi_version_ ? abi_version_() : 0u; }

const HSFPluginInfo* PluginLibrary::Info() const { return get_info_ ? get_info_() : nullptr; }

HSFStatus PluginLibrary::Create(const HSFPluginContext* ctx, HSFPlugin** out_plugin,
                                const HSFPluginVTable** out_vtable) const {
  if (!create_) return HSF_ERR_STATE;
  return create_(ctx, out_plugin, out_vtable);
}

void PluginLibrary::Destroy(HSFPlugin* plugin) const {
  if (destroy_ && plugin) destroy_(plugin);
}

}  // namespace hsf
