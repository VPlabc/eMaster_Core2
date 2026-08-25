#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hsf {

// A plugin's manifest.json, parsed and validated (plan §9).
//
// VALIDATION FAILS CLOSED, and says why in terms of both sides. "requires
// Plugin API 2.0, this gateway provides 1.0" is actionable; "incompatible
// plugin" is not, and the operator holding a .hsfplugin that will not install
// has no other source of information.
//
// The manifest is checked BEFORE the shared object is opened. That ordering is
// the point: a wrong-architecture or wrong-API-version binary handed to
// dlopen produces a loader message that no operator can act on, and on some
// platforms a crash. Everything cheap and textual is verified first, so the
// only failures that reach the loader are the ones the manifest could not
// predict.
struct PluginManifest {
  std::string id;            // "hsf.driver.modbus" — reverse-DNS, stable forever
  std::string name;
  std::string version;       // the plugin's own semver
  std::string api_version;   // "1.0" — the Plugin API it was built against
  std::string description;
  std::string author;
  std::string entry;         // "plugin.so" / "plugin.dll"
  std::string execution;     // "in_process" | "isolated"

  std::vector<std::string> platforms;    // "linux-arm64", …
  std::vector<std::string> transports;   // "serial", "tcp", …
  std::vector<std::string> permissions;  // "serial", "network", …

  // Filled in by the loader, not by the file.
  std::string directory;     // where it was found
  std::string manifest_path;

  uint32_t ApiVersionPacked() const;   // 0 when api_version is unparseable
  uint32_t PermissionBits() const;     // HSF_PERM_* fold
  bool     IsIsolated() const { return execution == "isolated"; }
};

// Every way a manifest can be rejected. Kept as an enum rather than a bare
// string so the REST layer and the UI can branch on the reason — "wrong
// architecture" deserves a different message from "not signed".
enum class ManifestError {
  kNone = 0,
  kUnreadable,
  kMalformedJson,
  kMissingField,
  kBadId,
  kBadVersion,
  kBadApiVersion,
  kIncompatibleApi,
  kUnsupportedPlatform,
  kBadExecution,
  kUnknownPermission,
  kUnknownTransport,
  kEntryMissing
};

struct ManifestResult {
  bool           ok = false;
  ManifestError  error = ManifestError::kNone;
  std::string    message;      // operator-facing, names both sides
  PluginManifest manifest;
};

// Reads and validates `path`. `host_api_version` is the packed version the
// gateway provides; pass HSF_PLUGIN_API_VERSION.
ManifestResult LoadManifest(const std::string& path, uint32_t host_api_version);

// Validates an already-parsed manifest. Split out so a manifest embedded in a
// package can be checked before anything is written to disk.
ManifestResult ValidateManifest(PluginManifest manifest, uint32_t host_api_version);

const char* ManifestErrorName(ManifestError e);

}  // namespace hsf
