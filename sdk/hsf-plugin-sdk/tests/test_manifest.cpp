/* Manifest validation tests.
 *
 * The manifest is the gateway's first and cheapest gate: everything textual is
 * checked before dlopen is allowed near the file. So the interesting cases here
 * are all the ways a manifest should be REFUSED, and whether the refusal says
 * something an operator can act on.
 *
 * These live in the SDK's test directory but exercise gateway code
 * (src/plugin_manager/PluginManifest.cpp), because that is where the C++ test
 * harness is. The gateway has no test framework of its own.
 */
#include "hsf/plugin_manager/PluginManifest.h"

#include "hsf/plugin.h"   /* HSF_PERM_*, which PluginManifest.h does not expose */
#include "hsf/testing.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using hsf::LoadManifest;
using hsf::ManifestError;
using hsf::ManifestResult;

namespace {

/* A scratch plugin directory that cleans up after itself. */
class Scratch {
 public:
  Scratch() {
    // No Date/random available in the SDK tests, so uniqueness comes from the
    // address of this object, which is unique among live instances.
    char name[64];
    std::snprintf(name, sizeof(name), "hsf_manifest_test_%p", static_cast<void*>(this));
    dir_ = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_, ec);
  }
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string WriteManifest(const std::string& json) const {
    const std::filesystem::path p = dir_ / "manifest.json";
    std::ofstream out(p, std::ios::binary);
    out << json;
    out.close();
    return p.string();
  }

  void WriteEntry(const std::string& name = "plugin.so") const {
    std::ofstream out(dir_ / name, std::ios::binary);
    out << "not a real shared object";
  }

  const std::filesystem::path& Dir() const { return dir_; }

 private:
  std::filesystem::path dir_;
};

/* A manifest that passes, for tests to mutate one field of. */
std::string GoodManifest(const char* extra = "") {
  std::string j =
      "{\"id\":\"hsf.driver.test\",\"name\":\"Test\",\"version\":\"1.0.0\","
      "\"api_version\":\"1.0\",\"entry\":\"plugin.so\",\"execution\":\"in_process\","
      "\"platforms\":[\"" HSF_PLATFORM_TRIPLE "\"],\"transports\":[\"tcp\"],"
      "\"permissions\":[\"network\"]";
  j += extra;
  j += "}";
  return j;
}

ManifestResult Check(const Scratch& s, const std::string& json, bool with_entry = true) {
  const std::string path = s.WriteManifest(json);
  if (with_entry) s.WriteEntry();
  return LoadManifest(path, HSF_PLUGIN_API_VERSION);
}

}  // namespace

HSF_TEST("manifest: a well-formed manifest is accepted and fully parsed") {
  Scratch s;
  ManifestResult r = Check(s, GoodManifest());
  HSF_REQUIRE(r.ok);
  HSF_CHECK_EQ(r.manifest.id, std::string("hsf.driver.test"));
  HSF_CHECK_EQ(r.manifest.version, std::string("1.0.0"));
  HSF_CHECK_EQ(r.manifest.entry, std::string("plugin.so"));
  HSF_CHECK_EQ(r.manifest.execution, std::string("in_process"));
  HSF_CHECK_EQ(r.manifest.permissions.size(), (size_t)1);
  HSF_CHECK(!r.manifest.directory.empty());
  HSF_CHECK_EQ(r.manifest.PermissionBits(), (uint32_t)HSF_PERM_NETWORK);
}

HSF_TEST("manifest: execution defaults to in_process when omitted") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"hsf.driver.t\",\"name\":\"T\",\"version\":\"1\","
      "\"api_version\":\"1.0\",\"entry\":\"plugin.so\"}");
  HSF_REQUIRE(r.ok);
  HSF_CHECK_EQ(r.manifest.execution, std::string("in_process"));
  HSF_CHECK(!r.manifest.IsIsolated());
}

HSF_TEST("manifest: a missing file and malformed JSON are distinguished") {
  ManifestResult missing = LoadManifest("/nonexistent/manifest.json", HSF_PLUGIN_API_VERSION);
  HSF_CHECK(!missing.ok);
  HSF_CHECK_EQ((int)missing.error, (int)ManifestError::kUnreadable);

  Scratch s;
  ManifestResult bad = Check(s, "{ this is not json ");
  HSF_CHECK(!bad.ok);
  HSF_CHECK_EQ((int)bad.error, (int)ManifestError::kMalformedJson);
  HSF_CHECK(!bad.message.empty());
}

HSF_TEST("manifest: each required field is named when it is missing") {
  /* Naming the field is the whole value of this check: "missing required field
   * api_version" is fixable, "invalid manifest" is not. */
  Scratch s;
  const char* without[] = {
      "{\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\",\"entry\":\"plugin.so\"}",
      "{\"id\":\"a.b\",\"version\":\"1\",\"api_version\":\"1.0\",\"entry\":\"plugin.so\"}",
      "{\"id\":\"a.b\",\"name\":\"T\",\"api_version\":\"1.0\",\"entry\":\"plugin.so\"}",
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"entry\":\"plugin.so\"}",
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\"}"};
  const char* field[] = {"id", "name", "version", "api_version", "entry"};
  for (size_t i = 0; i < sizeof(without) / sizeof(without[0]); ++i) {
    ManifestResult r = Check(s, without[i]);
    HSF_CHECK(!r.ok);
    HSF_CHECK_EQ((int)r.error, (int)ManifestError::kMissingField);
    HSF_CHECK(r.message.find(field[i]) != std::string::npos);
  }
}

HSF_TEST("manifest: ids that would orphan configuration are refused") {
  /* The id keys config, install state and rollback. A near-miss silently
   * detaches the operator's settings, so near-misses are rejected. */
  Scratch s;
  const char* bad_ids[] = {"\"nodot\"", "\"Has.Capitals\"", "\"has space.x\"",
                           "\"trailing.\"", "\".leading\"", "\"double..dot\"",
                           "\"bad/slash.x\"", "\"\""};
  for (const char* id : bad_ids) {
    const std::string j = std::string("{\"id\":") + id +
                          ",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\","
                          "\"entry\":\"plugin.so\"}";
    ManifestResult r = Check(s, j);
    HSF_CHECK(!r.ok);
    HSF_CHECK(r.error == ManifestError::kBadId || r.error == ManifestError::kMissingField);
  }
}

HSF_TEST("manifest: a major version mismatch says the plugin must be rebuilt") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"2.0\","
      "\"entry\":\"plugin.so\"}");
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kIncompatibleApi);
  /* Both sides named, and the remedy stated. */
  HSF_CHECK(r.message.find("2.0") != std::string::npos);
  HSF_CHECK(r.message.find("1.0") != std::string::npos);
  HSF_CHECK(r.message.find("rebuilt") != std::string::npos);
}

HSF_TEST("manifest: a newer minor says upgrade the gateway instead") {
  /* The asymmetric half of the compatibility rule: the plugin may call a slot
   * this host does not have, so it is refused -- but the fix is the opposite
   * one, and saying which saves a wasted rebuild. */
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.99\","
      "\"entry\":\"plugin.so\"}");
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kIncompatibleApi);
  HSF_CHECK(r.message.find("upgrade the gateway") != std::string::npos);
}

HSF_TEST("manifest: an older minor is accepted") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\","
      "\"entry\":\"plugin.so\"}");
  HSF_CHECK(r.ok);
}

HSF_TEST("manifest: api_version must be MAJOR.MINOR exactly") {
  Scratch s;
  const char* bad[] = {"\"1\"", "\"1.0.0\"", "\"v1.0\"", "\"1.x\"", "\"\"", "\"1.\""};
  for (const char* v : bad) {
    const std::string j = std::string("{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\","
                                      "\"api_version\":") + v + ",\"entry\":\"plugin.so\"}";
    ManifestResult r = Check(s, j);
    HSF_CHECK(!r.ok);
    HSF_CHECK(r.error == ManifestError::kBadApiVersion ||
              r.error == ManifestError::kMissingField);
  }
}

HSF_TEST("manifest: a wrong-architecture plugin is refused before dlopen") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\","
      "\"entry\":\"plugin.so\",\"platforms\":[\"solaris-sparc\"]}");
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kUnsupportedPlatform);
  /* Names what it is and what we are, so the operator knows they downloaded
   * the wrong artifact rather than that the plugin is broken. */
  HSF_CHECK(r.message.find("solaris-sparc") != std::string::npos);
  HSF_CHECK(r.message.find(HSF_PLATFORM_TRIPLE) != std::string::npos);
}

HSF_TEST("manifest: an empty platform list means portable, not broken") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\","
      "\"entry\":\"plugin.so\",\"platforms\":[]}");
  HSF_CHECK(r.ok);
}

HSF_TEST("manifest: an unknown permission is refused, not ignored") {
  /* Ignoring it would load a plugin asking for something this gateway cannot
   * enforce as though it had asked for nothing -- backwards for a security
   * control. */
  Scratch s;
  ManifestResult r = Check(s, GoodManifest(",\"permissions\":[\"network\",\"root\"]"));
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kUnknownPermission);
  HSF_CHECK(r.message.find("root") != std::string::npos);
}

HSF_TEST("manifest: every documented permission name is accepted") {
  Scratch s;
  ManifestResult r = Check(s, GoodManifest(
      ",\"permissions\":[\"serial\",\"network\",\"filesystem\",\"gpio\",\"exec\",\"lua\",\"db\"]"));
  HSF_REQUIRE(r.ok);
  const uint32_t all = HSF_PERM_SERIAL | HSF_PERM_NETWORK | HSF_PERM_FILESYS |
                       HSF_PERM_GPIO | HSF_PERM_EXEC | HSF_PERM_LUA | HSF_PERM_DB;
  HSF_CHECK_EQ(r.manifest.PermissionBits(), all);
}

HSF_TEST("manifest: an unknown transport is refused") {
  Scratch s;
  ManifestResult r = Check(s, GoodManifest(",\"transports\":[\"tcp\",\"carrier-pigeon\"]"));
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kUnknownTransport);
}

HSF_TEST("manifest: execution must be one of the two documented values") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\","
      "\"entry\":\"plugin.so\",\"execution\":\"sandboxed\"}");
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kBadExecution);
}

HSF_TEST("manifest: isolated is recognised") {
  Scratch s;
  ManifestResult r = Check(s,
      "{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\",\"api_version\":\"1.0\","
      "\"entry\":\"plugin.so\",\"execution\":\"isolated\"}");
  HSF_REQUIRE(r.ok);
  HSF_CHECK(r.manifest.IsIsolated());
}

HSF_TEST("manifest: a declared entry that does not exist is refused") {
  Scratch s;
  ManifestResult r = Check(s, GoodManifest(), /*with_entry=*/false);
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kEntryMissing);
  HSF_CHECK(r.message.find("plugin.so") != std::string::npos);
}

HSF_TEST("manifest: an entry escaping the plugin directory is refused") {
  /* Manifests arrive from outside. Without this, "entry" is a way to make the
   * gateway dlopen an arbitrary file on the box. */
  Scratch s;
  const char* escapes[] = {"\"../../plugin.so\"", "\"/usr/lib/libc.so.6\"",
                           "\"sub/../../plugin.so\""};
  for (const char* e : escapes) {
    const std::string j = std::string("{\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1\","
                                      "\"api_version\":\"1.0\",\"entry\":") + e + "}";
    ManifestResult r = Check(s, j);
    HSF_CHECK(!r.ok);
    HSF_CHECK_EQ((int)r.error, (int)ManifestError::kEntryMissing);
  }
}

HSF_TEST("manifest: an implausibly large file is refused without parsing") {
  Scratch s;
  std::string big = "{\"id\":\"a.b\",\"pad\":\"";
  big.append(300u * 1024u, 'x');
  big += "\"}";
  ManifestResult r = Check(s, big);
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kMalformedJson);
  HSF_CHECK(r.message.find("large") != std::string::npos);
}

HSF_TEST("manifest: a JSON array is not a manifest") {
  Scratch s;
  ManifestResult r = Check(s, "[1,2,3]");
  HSF_CHECK(!r.ok);
  HSF_CHECK_EQ((int)r.error, (int)ManifestError::kMalformedJson);
}

HSF_TEST("manifest: every error code has a name") {
  const ManifestError all[] = {
      ManifestError::kNone, ManifestError::kUnreadable, ManifestError::kMalformedJson,
      ManifestError::kMissingField, ManifestError::kBadId, ManifestError::kBadVersion,
      ManifestError::kBadApiVersion, ManifestError::kIncompatibleApi,
      ManifestError::kUnsupportedPlatform, ManifestError::kBadExecution,
      ManifestError::kUnknownPermission, ManifestError::kUnknownTransport,
      ManifestError::kEntryMissing};
  for (ManifestError e : all) {
    HSF_CHECK(std::string(hsf::ManifestErrorName(e)) != std::string("UNKNOWN"));
  }
}

HSF_TEST_MAIN()
