#include "hsf/plugin_manager/PluginManifest.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "hsf/plugin.h"

namespace hsf {
namespace {

using json = nlohmann::json;

// "1.0" -> packed. Rejects anything that is not exactly two dot-separated
// non-negative integers: "1", "1.0.0" and "v1.0" are all mistakes worth
// reporting rather than guessing at, because guessing wrong here loads a
// binary against the wrong contract.
bool ParseApiVersion(const std::string& text, uint32_t* out) {
  if (text.empty()) return false;
  const size_t dot = text.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) return false;
  if (text.find('.', dot + 1) != std::string::npos) return false;

  const std::string major = text.substr(0, dot);
  const std::string minor = text.substr(dot + 1);
  for (const std::string* part : {&major, &minor}) {
    if (part->size() > 5) return false;
    for (char c : *part) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
  }
  const unsigned long ma = std::stoul(major);
  const unsigned long mi = std::stoul(minor);
  if (ma > 0xFFFF || mi > 0xFFFF) return false;
  if (out) *out = static_cast<uint32_t>((ma << 16) | mi);
  return true;
}

// Reverse-DNS-ish: lowercase alphanumerics, dots, dashes and underscores, at
// least one dot, no leading or trailing dot, no empty segment.
//
// Strict because the id keys the plugin's configuration, its install state and
// its rollback target. A near-miss id -- "HSF.Driver.Modbus" against
// "hsf.driver.modbus" -- would silently orphan the operator's settings, and it
// also has to be safe as a directory name and inside a URL.
bool ValidId(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  if (id.front() == '.' || id.back() == '.') return false;
  bool has_dot = false;
  bool prev_dot = true;  // catches a leading dot too
  for (char c : id) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '.') {
      if (prev_dot) return false;  // empty segment
      has_dot = true;
      prev_dot = true;
      continue;
    }
    if (!(std::islower(u) || std::isdigit(u) || c == '-' || c == '_')) return false;
    prev_dot = false;
  }
  return has_dot;
}

// Deliberately permissive: at least one number, and nothing that would be
// unsafe in a filename. Full semver enforcement would reject real-world
// versions like "2.1" or "1.0.0-rc1+build7" that work perfectly well.
bool PlausibleVersion(const std::string& v) {
  if (v.empty() || v.size() > 64) return false;
  bool digit = false;
  for (char c : v) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isdigit(u)) digit = true;
    if (!(std::isalnum(u) || c == '.' || c == '-' || c == '+' || c == '_')) return false;
  }
  return digit;
}

struct Named {
  const char* name;
  uint32_t    bit;
};

const Named kPermissions[] = {
    {"serial", HSF_PERM_SERIAL}, {"network", HSF_PERM_NETWORK},
    {"filesystem", HSF_PERM_FILESYS}, {"gpio", HSF_PERM_GPIO},
    {"exec", HSF_PERM_EXEC}, {"lua", HSF_PERM_LUA}, {"db", HSF_PERM_DB},
};

const char* const kTransports[] = {"serial", "tcp", "udp", "can", "spi", "gpio", "mock"};

std::string Join(const std::vector<std::string>& v) {
  std::ostringstream os;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) os << ", ";
    os << v[i];
  }
  return os.str();
}

ManifestResult Fail(ManifestError e, std::string message) {
  ManifestResult r;
  r.ok = false;
  r.error = e;
  r.message = std::move(message);
  return r;
}

std::vector<std::string> StringArray(const json& j, const char* key) {
  std::vector<std::string> out;
  if (!j.contains(key) || !j[key].is_array()) return out;
  for (const json& e : j[key]) {
    if (e.is_string()) out.push_back(e.get<std::string>());
  }
  return out;
}

}  // namespace

uint32_t PluginManifest::ApiVersionPacked() const {
  uint32_t packed = 0;
  return ParseApiVersion(api_version, &packed) ? packed : 0u;
}

uint32_t PluginManifest::PermissionBits() const {
  uint32_t bits = 0;
  for (const std::string& p : permissions) {
    for (const Named& n : kPermissions) {
      if (p == n.name) bits |= n.bit;
    }
  }
  return bits;
}

const char* ManifestErrorName(ManifestError e) {
  switch (e) {
    case ManifestError::kNone:                return "NONE";
    case ManifestError::kUnreadable:          return "UNREADABLE";
    case ManifestError::kMalformedJson:       return "MALFORMED_JSON";
    case ManifestError::kMissingField:        return "MISSING_FIELD";
    case ManifestError::kBadId:               return "BAD_ID";
    case ManifestError::kBadVersion:          return "BAD_VERSION";
    case ManifestError::kBadApiVersion:       return "BAD_API_VERSION";
    case ManifestError::kIncompatibleApi:     return "INCOMPATIBLE_API";
    case ManifestError::kUnsupportedPlatform: return "UNSUPPORTED_PLATFORM";
    case ManifestError::kBadExecution:        return "BAD_EXECUTION";
    case ManifestError::kUnknownPermission:   return "UNKNOWN_PERMISSION";
    case ManifestError::kUnknownTransport:    return "UNKNOWN_TRANSPORT";
    case ManifestError::kEntryMissing:        return "ENTRY_MISSING";
  }
  return "UNKNOWN";
}

ManifestResult ValidateManifest(PluginManifest m, uint32_t host_api_version) {
  // Failures carry the manifest AS PARSED, not a blank one.
  //
  // The caller needs the id and the path to say which plugin was rejected --
  // PluginManager keys its record by them, and a rejected plugin has to be
  // visible in the UI under a recognisable name rather than under "". Losing
  // them here made every rejected plugin land under an empty key, which then
  // collided with the next rejected one.
  auto reject = [&m](ManifestError e, std::string message) {
    ManifestResult r;
    r.ok = false;
    r.error = e;
    r.message = std::move(message);
    r.manifest = m;
    return r;
  };

  // --- required fields ---
  struct Req { const char* name; const std::string* value; };
  const Req required[] = {{"id", &m.id}, {"name", &m.name}, {"version", &m.version},
                          {"api_version", &m.api_version}, {"entry", &m.entry}};
  for (const Req& r : required) {
    if (r.value->empty()) {
      return reject(ManifestError::kMissingField,
                  std::string("manifest is missing the required field \"") + r.name + "\"");
    }
  }

  if (!ValidId(m.id)) {
    return reject(ManifestError::kBadId,
                "plugin id \"" + m.id +
                    "\" is not a valid reverse-DNS identifier (lowercase letters, "
                    "digits, dot, dash and underscore only, with at least one dot)");
  }
  if (!PlausibleVersion(m.version)) {
    return reject(ManifestError::kBadVersion, "plugin version \"" + m.version + "\" is not a version");
  }

  // --- API compatibility ---
  uint32_t plugin_api = 0;
  if (!ParseApiVersion(m.api_version, &plugin_api)) {
    return reject(ManifestError::kBadApiVersion,
                "api_version \"" + m.api_version + "\" is not MAJOR.MINOR");
  }
  if (!HSF_API_VERSION_COMPATIBLE(host_api_version, plugin_api)) {
    std::ostringstream os;
    os << "plugin \"" << m.id << "\" needs Plugin API "
       << HSF_API_VERSION_MAJOR_OF(plugin_api) << "." << HSF_API_VERSION_MINOR_OF(plugin_api)
       << ", this gateway provides " << HSF_API_VERSION_MAJOR_OF(host_api_version) << "."
       << HSF_API_VERSION_MINOR_OF(host_api_version);
    // Naming which direction failed turns "incompatible" into an action:
    // upgrade the gateway, or rebuild the plugin.
    if (HSF_API_VERSION_MAJOR_OF(plugin_api) != HSF_API_VERSION_MAJOR_OF(host_api_version)) {
      os << " (major versions differ; the plugin must be rebuilt against this gateway's SDK)";
    } else {
      os << " (the gateway is older than the plugin; upgrade the gateway)";
    }
    return reject(ManifestError::kIncompatibleApi, os.str());
  }

  // --- platform ---
  // An empty list means "unspecified", which is treated as portable rather
  // than as broken: a pure-logic service plugin genuinely has no architecture.
  if (!m.platforms.empty()) {
    const std::string here = HSF_PLATFORM_TRIPLE;
    if (std::find(m.platforms.begin(), m.platforms.end(), here) == m.platforms.end()) {
      return reject(ManifestError::kUnsupportedPlatform,
                  "plugin \"" + m.id + "\" is built for [" + Join(m.platforms) +
                      "], this gateway is " + here);
    }
  }

  // --- execution mode ---
  if (m.execution.empty()) m.execution = "in_process";  // plan §10.1 default
  if (m.execution != "in_process" && m.execution != "isolated") {
    return reject(ManifestError::kBadExecution,
                "execution must be \"in_process\" or \"isolated\", not \"" + m.execution + "\"");
  }

  // --- permissions ---
  // Unknown permissions are rejected rather than ignored. Ignoring one would
  // mean a plugin declaring a permission this gateway has never heard of gets
  // loaded as though it declared nothing -- exactly backwards for a security
  // control (plan §14).
  for (const std::string& p : m.permissions) {
    bool known = false;
    for (const Named& n : kPermissions) {
      if (p == n.name) { known = true; break; }
    }
    if (!known) {
      return reject(ManifestError::kUnknownPermission,
                  "plugin \"" + m.id + "\" declares the unknown permission \"" + p +
                      "\"; this gateway would not be able to enforce it");
    }
  }

  for (const std::string& t : m.transports) {
    bool known = false;
    for (const char* n : kTransports) {
      if (t == n) { known = true; break; }
    }
    if (!known) {
      return reject(ManifestError::kUnknownTransport,
                  "plugin \"" + m.id + "\" asks for the unknown transport \"" + t + "\"");
    }
  }

  ManifestResult r;
  r.ok = true;
  r.error = ManifestError::kNone;
  r.manifest = std::move(m);
  return r;
}

ManifestResult LoadManifest(const std::string& path, uint32_t host_api_version) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return Fail(ManifestError::kUnreadable, "no manifest at " + path);
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) return Fail(ManifestError::kUnreadable, "cannot read " + path);

  // Bounded read. A manifest is a few hundred bytes; anything above this is
  // either a mistake or an attempt to make the loader allocate.
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (!ec && size > 256u * 1024u) {
    return Fail(ManifestError::kMalformedJson,
                "manifest at " + path + " is implausibly large (" + std::to_string(size) +
                    " bytes)");
  }

  json j;
  try {
    in >> j;
  } catch (const std::exception& e) {
    return Fail(ManifestError::kMalformedJson,
                std::string("manifest at ") + path + " is not valid JSON: " + e.what());
  }
  if (!j.is_object()) {
    return Fail(ManifestError::kMalformedJson, "manifest at " + path + " is not a JSON object");
  }

  PluginManifest m;
  auto str = [&j](const char* key) -> std::string {
    return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : std::string();
  };
  m.id = str("id");
  m.name = str("name");
  m.version = str("version");
  m.api_version = str("api_version");
  m.description = str("description");
  m.author = str("author");
  m.entry = str("entry");
  m.execution = str("execution");
  m.platforms = StringArray(j, "platforms");
  m.transports = StringArray(j, "transports");
  m.permissions = StringArray(j, "permissions");

  m.manifest_path = path;
  m.directory = std::filesystem::path(path).parent_path().string();

  ManifestResult r = ValidateManifest(std::move(m), host_api_version);
  if (!r.ok) return r;

  // The entry must exist, and must be inside the plugin's own directory. The
  // containment check is not paranoia: an entry of "../../bin/hsf_gateway" or
  // an absolute path would make the manifest a way to load an arbitrary file,
  // and manifests arrive from outside.
  const std::filesystem::path dir = std::filesystem::path(r.manifest.directory);
  const std::filesystem::path entry = dir / r.manifest.entry;
  const std::filesystem::path dir_abs = std::filesystem::weakly_canonical(dir, ec);
  const std::filesystem::path entry_abs = std::filesystem::weakly_canonical(entry, ec);
  const std::string dir_s = dir_abs.string();
  const std::string entry_s = entry_abs.string();
  if (entry_s.size() <= dir_s.size() || entry_s.compare(0, dir_s.size(), dir_s) != 0) {
    return Fail(ManifestError::kEntryMissing,
                "entry \"" + r.manifest.entry + "\" escapes the plugin directory " + dir_s);
  }
  if (!std::filesystem::is_regular_file(entry_abs, ec)) {
    return Fail(ManifestError::kEntryMissing,
                "plugin \"" + r.manifest.id + "\" declares entry \"" + r.manifest.entry +
                    "\" but there is no such file in " + r.manifest.directory);
  }
  return r;
}

}  // namespace hsf
