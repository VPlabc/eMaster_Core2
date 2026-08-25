#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/lua_package/LuaBundle.h"
#include "hsf/lua_package/LuaPackage.h"
#include "hsf/lua_package/PackageKeys.h"

namespace hsf {

// Builds, stores, deploys and rolls back production Lua packages
// (request/AdvanceUpdate.md sections 2.5, 2.10 and 2.11).
//
// LAYOUT, under the config directory:
//
//   lua_packages/
//   |-- smartlocker-1.4.0.pkg
//   |-- smartlocker-1.3.0.pkg
//   `-- state.json      { active, previous, history[] }
//
// Deliberately the same shape as the OTA installer's releases/ + current: a
// directory of immutable versioned artifacts and a small file naming which one
// is live. Rollback is then repointing a name, not reconstructing anything,
// and the previous version is still on disk to point back at.
class PackageManager {
 public:
  // Per-stage outcome of Build & Test (section 2.5), so a failure can say
  // WHICH stage failed rather than just "build failed".
  struct BuildReport {
    bool ok = false;
    std::string failed_stage;   // "" when ok
    std::string error;
    int error_line = 0;
    std::string error_module;

    int modules_compiled = 0;
    int64_t source_bytes = 0;
    int64_t bytecode_bytes = 0;
    int64_t package_bytes = 0;
    std::vector<std::string> stages_passed;
    LuaPackage::Metadata metadata;
    std::string package_path;   // set only when the artifact was written
  };

  struct Entry {
    std::string file;           // filename inside lua_packages/
    LuaPackage::Metadata metadata;
    int64_t size_bytes = 0;
    int64_t modified_at = 0;
    bool active = false;
    bool previous = false;
    bool run_on_startup = false;
  };

  PackageManager(std::string configDir);

  // Creates the package directory and any missing keys. `wantSigningKey` is
  // true on a machine that builds packages.
  bool Initialise(bool wantSigningKey, std::string& error);

  const PackageKeys& Keys() const { return keys_; }

  // --- build ---------------------------------------------------------------

  // Compiles every .lua under `sourceDir`, bundles them, encrypts, signs, and
  // -- crucially -- opens the result back up and loads it, before it is
  // written anywhere.
  //
  // The verify-and-load stage is section 2.4's requirement and the reason this
  // is "Build & Test" rather than "Build": a package that compiles but cannot
  // be loaded by this runtime is exactly the failure that would otherwise be
  // discovered at deploy time, on the machine, with the doors locked. Testing
  // the ARTIFACT rather than the source is the whole point.
  //
  // `writeArtifact` false does everything except save the .pkg -- the Compile
  // button, which wants the diagnostics without producing a deployable file.
  // `moduleRoot` is what module names are computed relative to, and it is NOT
  // the same as `sourceDir`. It has to be the scripts directory, because that
  // is what LuaEngine puts on package.path: an application in
  // scripts/smartlocker/ spells its own modules require("smartlocker.locker"),
  // so naming them relative to the application directory would produce
  // "locker" and every require() would miss.
  //
  // Getting this wrong is invisible while the plaintext sources are still on
  // disk -- require() simply falls through to package.path and loads the .lua,
  // and the package appears to work. It only fails once the source is removed,
  // which is the one moment you were relying on it.
  BuildReport BuildFromDirectory(const std::string& sourceDir, const std::string& moduleRoot,
                                 const std::string& appId, const std::string& version,
                                 const std::string& entryRelativePath, const std::string& builtBy,
                                 const std::string& notes, bool writeArtifact);

  // --- store ---------------------------------------------------------------

  std::vector<Entry> List() const;
  bool Import(const std::string& file, const std::vector<unsigned char>& bytes, std::string& error);
  // Imports a package already present on the gateway host, copying it into
  // the managed package directory after the same signature/decryption checks
  // used by browser uploads.
  bool ImportFromPath(const std::string& sourcePath, std::string& importedFile, std::string& error);
  bool Remove(const std::string& file, std::string& error);

  // Reads, verifies and decrypts `file`, returning its modules ready for
  // LuaEngine::RunBundle. Never writes the plaintext anywhere.
  bool OpenBundle(const std::string& file, LuaBundle& bundle, LuaPackage::Metadata& metadata,
                  std::string& error) const;

  // --- deployment ----------------------------------------------------------

  // Marks `file` active, remembering the outgoing one as the rollback target.
  // Verifies the package opens before recording it: activating something that
  // cannot be loaded would leave the gateway pointing at an artifact that
  // fails at every boot.
  bool Deploy(const std::string& file, const std::string& byUser, std::string& error);

  // Swaps back to the recorded previous version (section 2.11).
  bool Rollback(const std::string& byUser, std::string& rolledBackTo, std::string& error);

  std::string ActiveFile() const;
  std::string PreviousFile() const;
  // Nothing deployed, or the operator explicitly cleared it.
  bool HasActive() const;
  bool ClearActive(const std::string& byUser, std::string& error);
  std::vector<std::string> RunningFiles() const;
  bool SetRunOnStartup(const std::string& file, bool enabled, const std::string& byUser,
                       std::string& error);

  nlohmann::json StatusJson() const;

 private:
  struct State {
    std::string active;
    std::string previous;
    std::vector<std::string> running;
    nlohmann::json history = nlohmann::json::array();
  };

  State ReadState() const;
  bool WriteState(const State& state) const;
  void RecordHistory(State& state, const std::string& action, const std::string& file,
                     const std::string& byUser) const;
  std::string PackagePath(const std::string& file) const;

  mutable std::mutex mutex_;
  std::string configDir_;
  std::string packageDir_;
  std::string statePath_;
  PackageKeys keys_;
};

}  // namespace hsf
