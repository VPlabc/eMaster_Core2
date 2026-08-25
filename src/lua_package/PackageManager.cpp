#include "hsf/lua_package/PackageManager.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "hsf/Logger.h"
#include "hsf/lua_package/LuaCompiler.h"
#include "hsf/security/Validation.h"
#include "hsf/update/Sha256.h"
#include "hsf/update/Version.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace hsf {
namespace {

std::string NowIso8601Utc() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

bool WriteWholeFile(const std::string& path, const std::vector<unsigned char>& data) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  // Written to a temporary and renamed: a package half-written when the power
  // fails must not be left where the deploy list will offer it.
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out.good()) return false;
  }
  fs::rename(temporary, path, ec);
  if (ec) {
    // Cleanup after a failed operation: its own error_code, because `ec` still
    // holds the failure being reported and is read after this.
    std::error_code cleanupEc;
    fs::remove(temporary, cleanupEc);
    return false;
  }
  return true;
}

// A package filename is derived from app id and version, both of which are
// operator input that becomes a path. Same whitelist discipline as the OTA
// installer's version strings.
bool SafeComponent(const std::string& text) {
  if (text.empty() || text.size() > 64) return false;
  for (char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '-' || c == '_';
    if (!ok) return false;
  }
  return text != "." && text != ".." && text.front() != '-' && text.front() != '.';
}

}  // namespace

PackageManager::PackageManager(std::string configDir)
    : configDir_(std::move(configDir)), keys_(configDir_) {
  packageDir_ = (fs::path(configDir_) / "lua_packages").string();
  statePath_ = (fs::path(packageDir_) / "state.json").string();
}

bool PackageManager::Initialise(bool wantSigningKey, std::string& error) {
  std::error_code ec;
  fs::create_directories(packageDir_, ec);
  if (ec) {
    error = "cannot create " + packageDir_;
    return false;
  }
  bool generated = false;
  if (!keys_.EnsureExists(wantSigningKey, generated, error)) return false;
  if (generated) {
    Logger::Instance().Warning(LogCategory::Lua,
                               "Generated a NEW Lua package signing keypair (" +
                                   keys_.PublicKeyFingerprint() +
                                   "). Packages signed with any previous key will no longer be accepted.");
  }
  return true;
}

std::string PackageManager::PackagePath(const std::string& file) const {
  return (fs::path(packageDir_) / file).string();
}

// --- build -------------------------------------------------------------------

PackageManager::BuildReport PackageManager::BuildFromDirectory(
    const std::string& sourceDir, const std::string& moduleRoot, const std::string& appId,
    const std::string& version, const std::string& entryRelativePath, const std::string& builtBy,
    const std::string& notes, bool writeArtifact) {
  BuildReport report;

  auto fail = [&report](const std::string& stage, const std::string& message) {
    report.ok = false;
    report.failed_stage = stage;
    report.error = message;
    return report;
  };

  if (!SafeComponent(appId)) return fail("validate", "application id may contain only letters, digits, dot, dash and underscore");
  if (!SafeComponent(version)) return fail("validate", "version may contain only letters, digits, dot, dash and underscore");
  {
    Version parsed;
    if (!Version::Parse(version, parsed)) {
      // Rollback ordering and "is this newer" both compare versions, so a
      // version that cannot be parsed would sort unpredictably against the
      // ones that can.
      return fail("validate", "version must be a semantic version like 1.4.0");
    }
  }

  std::error_code ec;
  if (!fs::is_directory(sourceDir, ec)) return fail("validate", "no such application directory");

  const fs::path root = fs::weakly_canonical(sourceDir, ec);
  const fs::path entryAbsolute = fs::weakly_canonical(root / entryRelativePath, ec);
  if (ec || !fs::is_regular_file(entryAbsolute, ec)) {
    // Named relative to the APPLICATION directory, not the scripts directory:
    // the app root is the bundle root, so an app in scripts/demoapp/ has entry
    // "main.lua", not "demoapp/main.lua". Easy to get backwards, so the
    // message says which.
    return fail("validate", "entry script \"" + entryRelativePath +
                                "\" not found -- it is relative to the application directory (" +
                                sourceDir + "), so for scripts/demoapp/main.lua the entry is \"main.lua\"");
  }

  // --- stage 1: compile every module ---------------------------------------
  //
  // Module names are relative to `moduleRoot` (the scripts directory), not to
  // the application directory -- see the note on the declaration. `nameRoot`
  // falls back to the application directory only when no module root was
  // given, which is the single-application-at-the-root case.
  const fs::path nameRoot = moduleRoot.empty() ? root : fs::weakly_canonical(moduleRoot, ec);

  LuaBundle bundle;
  std::vector<std::string> sourceHashes;
  const std::string entryModule = LuaBundle::ModuleNameForPath(
      fs::relative(entryAbsolute, nameRoot, ec).generic_string());

  for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() != ".lua") continue;

    const std::string relative = fs::relative(entry.path(), root, ec).generic_string();
    // The application's own test scripts are not part of the product. Shipping
    // them would put test fixtures -- and whatever credentials they hard-code
    // for a test rig -- inside the production artifact.
    if (relative.rfind("test", 0) == 0 || relative.find("/test") != std::string::npos ||
        relative.find("_test.lua") != std::string::npos) {
      continue;
    }

    std::ifstream in(entry.path(), std::ios::binary);
    if (!in.is_open()) return fail("compile", "cannot read " + relative);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Stripped: the production artifact carries no local names and no line
    // table. That is the "raises the reverse-engineering cost" part of section
    // 2.2 -- and the cost is real, so it is stated in docs/lua-packaging.md:
    // a runtime error from a deployed package reports no line number.
    LuaCompiler::Result compiled = LuaCompiler::Compile(source, relative, true);
    if (!compiled.ok) {
      report.error_line = compiled.error_line;
      report.error_module = relative;
      return fail("compile", compiled.error);
    }

    const std::string moduleName =
        LuaBundle::ModuleNameForPath(fs::relative(entry.path(), nameRoot, ec).generic_string());
    bundle.AddModule(moduleName, relative, std::move(compiled.bytecode));
    report.modules_compiled++;
    report.source_bytes += static_cast<int64_t>(source.size());
    sourceHashes.push_back(relative + ":" + Sha256::HexOf(source));
  }

  if (report.modules_compiled == 0) return fail("compile", "the application directory contains no .lua files");
  bundle.SetEntry(entryModule);
  if (bundle.EntryBytecode() == nullptr) {
    return fail("compile", "entry module \"" + entryModule + "\" was not among the compiled files");
  }
  report.stages_passed.push_back("compile");

  const std::vector<unsigned char> payload = bundle.Serialise();
  report.bytecode_bytes = static_cast<int64_t>(payload.size());

  // --- stage 2: encrypt and sign -------------------------------------------
  std::vector<unsigned char> encryptionKey;
  std::vector<unsigned char> secretKey;
  std::string keyError;
  if (!keys_.LoadEncryptionKey(encryptionKey, keyError)) return fail("sign", keyError);
  if (!keys_.LoadSecretKey(secretKey, keyError)) {
    return fail("sign", keyError + " -- this gateway has no signing key, so it cannot build packages. "
                                   "Build them where the key lives, or generate one (docs/lua-packaging.md).");
  }

  LuaPackage::Metadata metadata;
  metadata.app_id = appId;
  metadata.version = version;
  metadata.entry = entryRelativePath;
  metadata.runtime_tag = LuaCompiler::RuntimeTag();
  metadata.built_at = NowIso8601Utc();
  metadata.built_by = builtBy;
  metadata.notes = notes;
  {
    // One hash over every source file's own hash, so the artifact can be
    // traced back to an exact tree without listing every file in the header.
    std::sort(sourceHashes.begin(), sourceHashes.end());
    std::string combined;
    for (const std::string& line : sourceHashes) combined += line + "\n";
    metadata.source_sha256 = Sha256::HexOf(combined);
  }

  LuaPackage::BuildResult built = LuaPackage::Build(payload, metadata, encryptionKey, secretKey);
  if (!built.ok) return fail("sign", built.error);
  report.package_bytes = static_cast<int64_t>(built.package.size());
  report.stages_passed.push_back("sign");

  // --- stage 3: open the artifact back up ----------------------------------
  std::vector<unsigned char> publicKey;
  if (!keys_.LoadPublicKey(publicKey, keyError)) return fail("verify", keyError);

  LuaPackage::OpenResult opened = LuaPackage::Open(built.package, encryptionKey, publicKey);
  if (!opened.ok) return fail("verify", opened.error);
  report.metadata = opened.metadata;
  report.stages_passed.push_back("verify");

  // --- stage 4: load it, with the real interpreter -------------------------
  LuaBundle roundTripped;
  std::string bundleError;
  if (!LuaBundle::Parse(opened.bytecode, roundTripped, bundleError)) {
    return fail("load", bundleError);
  }
  for (const LuaBundle::Module& module : roundTripped.Modules()) {
    // Parses each chunk in a throwaway state. This is what catches a
    // bytecode/runtime mismatch here rather than on the device, and it is the
    // reason the compiler runs in-process instead of shelling out to luac.
    LuaCompiler::Result check = LuaCompiler::Validate(
        std::string(reinterpret_cast<const char*>(module.bytecode.data()), module.bytecode.size()),
        module.name);
    if (!check.ok) {
      report.error_module = module.name;
      return fail("load", "compiled module \"" + module.name + "\" will not load: " + check.error);
    }
  }
  report.stages_passed.push_back("load");

  // --- stage 5: write it ----------------------------------------------------
  if (writeArtifact) {
    const std::string file = appId + "-" + version + ".pkg";
    if (!WriteWholeFile(PackagePath(file), built.package)) {
      return fail("write", "could not write " + PackagePath(file));
    }
    report.package_path = file;
    report.stages_passed.push_back("write");
    Logger::Instance().Info(LogCategory::Lua, "Built Lua package " + file + " (" +
                                                   std::to_string(report.modules_compiled) + " modules, " +
                                                   std::to_string(report.package_bytes) + " bytes)");
  }

  report.ok = true;
  return report;
}

// --- store ---------------------------------------------------------------------

std::vector<PackageManager::Entry> PackageManager::List() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Entry> entries;
  const State state = ReadState();

  std::error_code ec;
  for (const auto& item : fs::directory_iterator(packageDir_, ec)) {
    if (!item.is_regular_file(ec)) continue;
    if (item.path().extension() != ".pkg") continue;

    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(item.path().string(), bytes)) continue;

    Entry entry;
    entry.file = item.path().filename().string();
    entry.size_bytes = static_cast<int64_t>(bytes.size());
    std::string error;
    // Header only: listing must not need the encryption key, and must not cost
    // a decrypt per row.
    LuaPackage::PeekMetadata(bytes, entry.metadata, error);
    entry.active = (entry.file == state.active);
    entry.previous = (entry.file == state.previous);
    entry.run_on_startup =
        std::find(state.running.begin(), state.running.end(), entry.file) != state.running.end();
    entries.push_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    Version va, vb;
    const bool pa = Version::Parse(a.metadata.version, va);
    const bool pb = Version::Parse(b.metadata.version, vb);
    if (pa && pb && !(va == vb)) return vb < va;  // newest first
    return a.file > b.file;
  });
  return entries;
}

bool PackageManager::Import(const std::string& file, const std::vector<unsigned char>& bytes,
                            std::string& error) {
  if (!SafeComponent(fs::path(file).stem().string()) || fs::path(file).extension() != ".pkg") {
    error = "not a package filename";
    return false;
  }
  std::vector<unsigned char> encryptionKey;
  std::vector<unsigned char> publicKey;
  if (!keys_.LoadEncryptionKey(encryptionKey, error) || !keys_.LoadPublicKey(publicKey, error)) return false;
  LuaPackage::OpenResult opened = LuaPackage::Open(bytes, encryptionKey, publicKey);
  if (!opened.ok) { error = opened.error; return false; }
  if (!WriteWholeFile(PackagePath(file), bytes)) { error = "could not save imported package"; return false; }
  return true;
}

bool PackageManager::ImportFromPath(const std::string& sourcePath, std::string& importedFile,
                                    std::string& error) {
  importedFile.clear();
  std::error_code ec;
  const fs::path source = fs::weakly_canonical(sourcePath, ec);
  if (ec || !fs::is_regular_file(source, ec)) {
    error = "package path does not point to a regular file";
    return false;
  }
  const std::string file = source.filename().string();
  if (source.extension() != ".pkg" || !SafeComponent(source.stem().string())) {
    error = "package path must name a safe .pkg file";
    return false;
  }
  constexpr uintmax_t kMaxPackageBytes = 64u * 1024u * 1024u;
  const uintmax_t size = fs::file_size(source, ec);
  if (ec || size > kMaxPackageBytes) {
    error = "package is missing or larger than 64 MB";
    return false;
  }
  std::ifstream in(source, std::ios::binary);
  if (!in.is_open()) {
    error = "cannot read package path";
    return false;
  }
  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (!in.good() && !in.eof()) {
    error = "could not read package path";
    return false;
  }
  if (!Import(file, bytes, error)) return false;
  importedFile = file;
  return true;
}

bool PackageManager::Remove(const std::string& file, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!SafeComponent(fs::path(file).stem().string()) || fs::path(file).extension() != ".pkg") {
    error = "not a package filename";
    return false;
  }
  const State state = ReadState();
  if (file == state.active) {
    error = "that package is currently deployed";
    return false;
  }
  if (file == state.previous) {
    error = "that package is the rollback target";
    return false;
  }
  if (std::find(state.running.begin(), state.running.end(), file) != state.running.end()) {
    error = "that package is configured to run on startup; stop it first";
    return false;
  }
  std::error_code ec;
  if (!fs::remove(PackagePath(file), ec) || ec) {
    error = "could not delete the package";
    return false;
  }
  return true;
}

bool PackageManager::OpenBundle(const std::string& file, LuaBundle& bundle,
                                LuaPackage::Metadata& metadata, std::string& error) const {
  if (file.empty()) {
    error = "no package specified";
    return false;
  }
  if (fs::path(file).filename().string() != file) {
    error = "not a package filename";
    return false;
  }

  std::vector<unsigned char> bytes;
  if (!ReadWholeFile(PackagePath(file), bytes)) {
    error = "cannot read " + file;
    return false;
  }
  std::vector<unsigned char> encryptionKey;
  std::vector<unsigned char> publicKey;
  if (!keys_.LoadEncryptionKey(encryptionKey, error)) return false;
  if (!keys_.LoadPublicKey(publicKey, error)) return false;

  LuaPackage::OpenResult opened = LuaPackage::Open(bytes, encryptionKey, publicKey);
  if (!opened.ok) {
    error = opened.error;
    // A signature failure is not a configuration problem; it is either a
    // corrupted file or someone's attempt. It gets its own log line at ERROR
    // so it is visible in the runtime log without reading the audit trail.
    if (opened.signature_failed) {
      Logger::Instance().Error(LogCategory::Lua,
                               "REFUSED Lua package " + Validate::SanitiseForLog(file, 128) +
                                   ": signature does not verify");
    }
    return false;
  }
  metadata = opened.metadata;
  return LuaBundle::Parse(opened.bytecode, bundle, error);
}

// --- deployment -------------------------------------------------------------------

PackageManager::State PackageManager::ReadState() const {
  State state;
  std::ifstream in(statePath_);
  if (!in.is_open()) return state;
  try {
    json root;
    in >> root;
    state.active = root.value("active", std::string());
    state.previous = root.value("previous", std::string());
    if (root.contains("running") && root["running"].is_array()) {
      for (const auto& item : root["running"]) {
        if (item.is_string()) state.running.push_back(item.get<std::string>());
      }
    } else if (!state.active.empty()) {
      // Backward compatibility: before per-package desired state existed, the
      // active package always started at boot.
      state.running.push_back(state.active);
    }
    if (root.contains("history") && root["history"].is_array()) state.history = root["history"];
  } catch (const std::exception&) {
    Logger::Instance().Warning(LogCategory::Lua, "Ignoring unreadable " + statePath_);
  }
  return state;
}

bool PackageManager::WriteState(const State& state) const {
  json root = {{"active", state.active},
               {"previous", state.previous},
               {"running", state.running},
               {"history", state.history}};
  std::error_code ec;
  const std::string temporary = statePath_ + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open()) return false;
    out << root.dump(2) << "\n";
    if (!out.good()) return false;
  }
  fs::rename(temporary, statePath_, ec);
  if (ec) {
    // Cleanup after a failed operation: its own error_code, because `ec` still
    // holds the failure being reported and is read after this.
    std::error_code cleanupEc;
    fs::remove(temporary, cleanupEc);
    return false;
  }
  return true;
}

void PackageManager::RecordHistory(State& state, const std::string& action, const std::string& file,
                                   const std::string& byUser) const {
  state.history.push_back(json{{"at", NowIso8601Utc()},
                               {"action", action},
                               {"package", file},
                               {"by", Validate::SanitiseForLog(byUser, 64)}});
  // Bounded: this file is read on every status poll.
  while (state.history.size() > 100) state.history.erase(state.history.begin());
}

bool PackageManager::Deploy(const std::string& file, const std::string& byUser, std::string& error) {
  LuaBundle bundle;
  LuaPackage::Metadata metadata;
  // Verified BEFORE it is recorded as active. Pointing the gateway at a
  // package that cannot be opened would mean it fails at every boot with
  // nothing obviously wrong in the configuration.
  if (!OpenBundle(file, bundle, metadata, error)) return false;

  std::string outgoingAppId;
  const std::string currentActive = ActiveFile();
  if (!currentActive.empty() && currentActive != file) {
    LuaBundle outgoingBundle;
    LuaPackage::Metadata outgoingMetadata;
    std::string ignored;
    if (OpenBundle(currentActive, outgoingBundle, outgoingMetadata, ignored))
      outgoingAppId = outgoingMetadata.app_id;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  State state = ReadState();
  const std::string outgoing = state.active;
  const bool sameApplication = !outgoing.empty() && outgoingAppId == metadata.app_id;
  const bool replaceRunning =
      sameApplication &&
      std::find(state.running.begin(), state.running.end(), outgoing) != state.running.end();
  if (outgoing != file) state.previous = sameApplication ? outgoing : std::string();
  state.active = file;
  if (outgoing.empty() || !sameApplication || replaceRunning) {
    if (sameApplication) {
      state.running.erase(std::remove(state.running.begin(), state.running.end(), outgoing),
                          state.running.end());
    }
    if (std::find(state.running.begin(), state.running.end(), file) == state.running.end())
      state.running.push_back(file);
  }
  RecordHistory(state, "deploy", file, byUser);
  if (!WriteState(state)) {
    error = "could not record the deployment";
    return false;
  }
  Logger::Instance().Info(LogCategory::Lua, "Deployed Lua package " + file + " (version " +
                                                 metadata.version + ", " +
                                                 std::to_string(bundle.Count()) + " modules)");
  return true;
}

bool PackageManager::Rollback(const std::string& byUser, std::string& rolledBackTo, std::string& error) {
  std::string target;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const State state = ReadState();
    if (state.previous.empty()) {
      error = "there is no previous package to roll back to";
      return false;
    }
    target = state.previous;
  }

  LuaBundle bundle;
  LuaPackage::Metadata metadata;
  if (!OpenBundle(target, bundle, metadata, error)) {
    error = "the previous package cannot be opened: " + error;
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  State state = ReadState();
  // The lock was released to verify `target`, so a deploy may have moved the
  // rollback target since. Swapping blindly would make active a package this
  // call never opened -- the failure Deploy's verify-first exists to prevent.
  if (state.previous != target) {
    error = "the rollback target changed while it was being verified; try again";
    return false;
  }
  const std::string outgoing = state.active;
  const bool replaceRunning =
      std::find(state.running.begin(), state.running.end(), outgoing) != state.running.end();
  std::swap(state.active, state.previous);
  if (replaceRunning) {
    state.running.erase(std::remove(state.running.begin(), state.running.end(), outgoing),
                        state.running.end());
    if (std::find(state.running.begin(), state.running.end(), state.active) == state.running.end())
      state.running.push_back(state.active);
  }
  RecordHistory(state, "rollback", state.active, byUser);
  if (!WriteState(state)) {
    error = "could not record the rollback";
    return false;
  }
  rolledBackTo = state.active;
  Logger::Instance().Warning(LogCategory::Lua, "Rolled Lua application back to " + state.active);
  return true;
}

std::string PackageManager::ActiveFile() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadState().active;
}

std::string PackageManager::PreviousFile() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadState().previous;
}

bool PackageManager::HasActive() const { return !ActiveFile().empty(); }

std::vector<std::string> PackageManager::RunningFiles() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadState().running;
}

bool PackageManager::SetRunOnStartup(const std::string& file, bool enabled,
                                     const std::string& byUser, std::string& error) {
  std::string appId;
  std::vector<Entry> known;
  if (enabled) {
    LuaBundle bundle;
    LuaPackage::Metadata metadata;
    if (!OpenBundle(file, bundle, metadata, error)) return false;
    appId = metadata.app_id;
    known = List();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  State state = ReadState();
  if (enabled) {
    for (const Entry& entry : known) {
      if (entry.file != file && entry.metadata.app_id == appId) {
        state.running.erase(std::remove(state.running.begin(), state.running.end(), entry.file),
                            state.running.end());
      }
    }
  }
  auto it = std::find(state.running.begin(), state.running.end(), file);
  if (enabled && it == state.running.end()) state.running.push_back(file);
  if (!enabled && it != state.running.end()) state.running.erase(it);
  RecordHistory(state, enabled ? "run" : "stop", file, byUser);
  if (!WriteState(state)) {
    error = "could not save package startup state";
    return false;
  }
  return true;
}

bool PackageManager::ClearActive(const std::string& byUser, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  State state = ReadState();
  if (state.active.empty()) {
    error = "nothing is deployed";
    return false;
  }
  state.previous = state.active;
  state.active.clear();
  RecordHistory(state, "undeploy", state.previous, byUser);
  if (!WriteState(state)) {
    error = "could not record the change";
    return false;
  }
  return true;
}

json PackageManager::StatusJson() const {
  const std::vector<Entry> entries = List();
  std::lock_guard<std::mutex> lock(mutex_);
  const State state = ReadState();

  json packages = json::array();
  for (const Entry& entry : entries) {
    json item = LuaPackage::ToJson(entry.metadata);
    item["file"] = entry.file;
    item["size_bytes"] = entry.size_bytes;
    item["active"] = entry.active;
    item["previous"] = entry.previous;
    item["run_on_startup"] = entry.run_on_startup;
    packages.push_back(std::move(item));
  }

  return json{{"packages", packages},
              {"active", state.active},
              {"previous", state.previous},
              {"running", state.running},
              {"history", state.history},
              {"runtime_tag", LuaCompiler::RuntimeTag()},
              {"can_build", keys_.HasSigningKey()},
              {"signing_key", keys_.PublicKeyFingerprint()},
              {"package_dir", packageDir_}};
}

}  // namespace hsf
