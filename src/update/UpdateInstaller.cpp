#include "hsf/update/UpdateInstaller.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "hsf/Logger.h"
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

// A version string reaches this file from a manifest served over the network
// and leaves it as a directory name and a shell argument. Whitelist, not
// blacklist.
bool IsSafeVersionString(const std::string& version) {
  if (version.empty() || version.size() > 64) return false;
  for (char c : version) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '-' || c == '+';
    if (!ok) return false;
  }
  // "." and ".." would escape or alias the releases directory; a leading dash
  // would be read by a shell as an option rather than a path.
  if (version == "." || version == "..") return false;
  if (version.front() == '-') return false;
  return true;
}

std::string Quote(const std::string& path) {
#if defined(_WIN32)
  return "\"" + path + "\"";
#else
  // Single quotes, with any embedded quote closed and re-opened. Paths here
  // are ours, but this is the function that would be reached if that ever
  // stopped being true.
  std::string out = "'";
  for (char c : path) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
#endif
}

bool RunCommand(const std::string& command, std::string& error) {
  Logger::Instance().Debug(LogCategory::System, "Update: running " + command);
#if defined(_WIN32)
  const int code = std::system(("cmd /c " + command).c_str());
#else
  const int code = std::system(command.c_str());
#endif
  if (code != 0) {
    error = "command failed with exit code " + std::to_string(code) + ": " + command;
    return false;
  }
  return true;
}

// The one file that proves an extracted directory is a gateway release.
bool LooksLikeRelease(const fs::path& dir) {
  std::error_code ec;
  return fs::exists(dir / "bin" / "hsf_gateway", ec) || fs::exists(dir / "bin" / "hsf_gateway.exe", ec);
}

}  // namespace

UpdateInstaller::Layout UpdateInstaller::Detect(const std::string& executableDir, const std::string& overrideRoot) {
  Layout layout;
  std::error_code ec;

  fs::path root;
  if (!overrideRoot.empty()) {
    root = fs::path(overrideRoot);
    if (!fs::is_directory(root, ec)) {
      layout.reason = "update.install_root is set to " + overrideRoot + ", which is not a directory";
      return layout;
    }
  } else {
    // The gateway is started through the `current` link, so what main() hands
    // us is one of two shapes depending on the platform:
    //
    //   <root>/releases/<version>/bin   Linux: /proc/self/exe is already
    //                                   resolved, the link is gone by now
    //   <root>/current/bin              Windows: GetModuleFileNameW reports the
    //                                   path the process was LAUNCHED with,
    //                                   junction and all
    //
    // Canonicalising turns the second into the first (both symlinks and
    // Windows junctions resolve), and the `current/bin` branch below is the
    // belt to that braces -- a filesystem that declines to resolve the link
    // should not cost the device its ability to update itself.
    fs::path exeDir = fs::weakly_canonical(fs::path(executableDir), ec);
    if (ec || exeDir.empty()) exeDir = fs::path(executableDir);

    const fs::path releaseDir = exeDir.parent_path();
    const fs::path parentName = releaseDir.parent_path();

    if (parentName.filename() == "releases") {
      root = parentName.parent_path();
      layout.active_release = fs::weakly_canonical(releaseDir, ec).string();
    } else if (releaseDir.filename() == "current") {
      root = releaseDir.parent_path();
      layout.active_release = fs::weakly_canonical(releaseDir, ec).string();
    } else {
      layout.reason =
          "not running from a managed install (expected <root>/releases/<version>/bin, got " + executableDir +
          "). Over-the-air updates need the layout scripts/install-ota.sh creates; set update.install_root to "
          "override.";
      return layout;
    }
  }

  layout.root = fs::weakly_canonical(root, ec).string();
  layout.releases_dir = (root / "releases").string();
  layout.current_link = (root / "current").string();
  layout.data_dir = (root / "data").string();
  layout.downloads_dir = (root / "downloads").string();
  layout.state_file = (root / "update-state.json").string();

  if (!fs::is_directory(layout.releases_dir, ec)) {
    layout.reason = layout.releases_dir + " does not exist";
    return layout;
  }
  if (layout.active_release.empty()) {
    layout.active_release = fs::weakly_canonical(layout.current_link, ec).string();
  }

  fs::create_directories(layout.downloads_dir, ec);
  fs::create_directories(layout.data_dir, ec);

  layout.managed = true;
  return layout;
}

std::vector<std::string> UpdateInstaller::InstalledVersions(const Layout& layout) {
  std::vector<std::string> versions;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(layout.releases_dir, ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    Version parsed;
    if (Version::Parse(name, parsed)) versions.push_back(name);
  }
  std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
    Version va, vb;
    Version::Parse(a, va);
    Version::Parse(b, vb);
    return va > vb;  // newest first
  });
  return versions;
}

bool UpdateInstaller::ExtractArchive(const std::string& archivePath, const std::string& destinationDir,
                                     std::string& error) {
  std::error_code ec;
  fs::remove_all(destinationDir, ec);
  if (!fs::create_directories(destinationDir, ec) && !fs::is_directory(destinationDir, ec)) {
    error = "cannot create staging directory " + destinationDir;
    return false;
  }

  // tar rather than a bundled decompressor: it is present on every target
  // (Windows 10 1803+ ships bsdtar as tar.exe, which also reads the .zip the
  // Windows packages use), and pulling in libarchive to unpack an archive we
  // produced ourselves is a dependency for one call site.
  const bool isZip = archivePath.size() > 4 && archivePath.substr(archivePath.size() - 4) == ".zip";
  const std::string command =
      "tar " + std::string(isZip ? "-xf" : "-xzf") + " " + Quote(archivePath) + " -C " + Quote(destinationDir);
  if (!RunCommand(command, error)) {
    error = "failed to unpack the package (" + error + ")";
    return false;
  }
  return true;
}

bool UpdateInstaller::SwapCurrent(const Layout& layout, const std::string& releaseDirName, std::string& error) {
  std::error_code ec;
  const fs::path link = layout.current_link;

#if defined(_WIN32)
  // Windows symlink creation needs either an elevated process or Developer
  // Mode; a directory junction needs neither, and behaves the same for the one
  // thing this is used for -- following a path into the release. The cost is
  // that there is no atomic replace, so there is a brief window where
  // `current` does not exist. A restart landing exactly there fails to start
  // and is retried by the service manager, which is survivable; requiring
  // Administrator to install an update is not.
  if (fs::exists(link, ec) || fs::is_symlink(link, ec)) {
    std::string ignored;
    RunCommand("rmdir " + Quote(layout.current_link), ignored);
  }
  const std::string absoluteTarget = (fs::path(layout.releases_dir) / releaseDirName).string();
  if (!RunCommand("mklink /J " + Quote(layout.current_link) + " " + Quote(absoluteTarget), error)) {
    error = "could not point current at " + releaseDirName + " (" + error + ")";
    return false;
  }
  return true;
#else
  // Relative target, so the whole tree can be moved or bind-mounted elsewhere
  // without every symlink still pointing at the old absolute path.
  const fs::path target = fs::path("releases") / releaseDirName;

  // Create the new link under a temporary name, then rename it over the old
  // one. rename(2) on a symlink is atomic, so `current` is never absent and
  // never half-written -- a reader sees either the old release or the new one.
  const fs::path staging = fs::path(layout.root) / ".current.new";
  fs::remove(staging, ec);
  ec.clear();
  fs::create_directory_symlink(target, staging, ec);
  if (ec) {
    error = "could not create symlink " + staging.string() + ": " + ec.message();
    return false;
  }
  fs::rename(staging, link, ec);
  if (ec) {
    // Cleanup after a failed operation: its own error_code, because `ec` still
    // holds the failure being reported and is read after this.
    std::error_code cleanupEc;
    fs::remove(staging, cleanupEc);
    error = "could not move " + staging.string() + " onto " + link.string() + ": " + ec.message();
    return false;
  }
  return true;
#endif
}

void UpdateInstaller::SeedDataDirectory(const Layout& layout, const std::string& releaseDir) {
  // Files a release ships that the *installation* owns once it exists: copied
  // into data/ only when they are not already there, so an operator's edited
  // log_definitions.json survives every future update while a newly introduced
  // file still appears.
  std::error_code ec;
  const fs::path shipped = fs::path(releaseDir) / "config";
  if (!fs::is_directory(shipped, ec)) return;

  for (const auto& entry : fs::directory_iterator(shipped, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const fs::path destination = fs::path(layout.data_dir) / entry.path().filename();
    if (fs::exists(destination, ec)) continue;
    std::error_code copyError;
    fs::copy_file(entry.path(), destination, copyError);
    if (!copyError) {
      Logger::Instance().Info(LogCategory::System,
                              "Update: seeded " + destination.string() + " from the new release");
    }
  }
}

bool UpdateInstaller::Install(const Layout& layout, const std::string& version, const std::string& archivePath,
                              std::string& error) {
  if (!layout.managed) {
    error = layout.reason.empty() ? "this gateway is not an OTA-managed install" : layout.reason;
    return false;
  }
  if (!IsSafeVersionString(version)) {
    error = "refusing to install a version string with unexpected characters: " + version;
    return false;
  }

  std::error_code ec;
  const fs::path staging = fs::path(layout.downloads_dir) / (version + ".stage");
  if (!ExtractArchive(archivePath, staging.string(), error)) return false;

  // A package stages as HSF-Gateway-v<version>-<platform>/, one directory at
  // the top. Find it rather than assuming the name: the platform suffix is
  // decided by whoever built it.
  fs::path unpacked;
  int topLevelEntries = 0;
  for (const auto& entry : fs::directory_iterator(staging, ec)) {
    ++topLevelEntries;
    if (entry.is_directory(ec) && LooksLikeRelease(entry.path())) unpacked = entry.path();
  }
  if (unpacked.empty() && LooksLikeRelease(staging)) unpacked = staging;  // archive without a wrapper directory
  if (unpacked.empty()) {
    fs::remove_all(staging, ec);
    error = "the package does not contain bin/hsf_gateway (found " + std::to_string(topLevelEntries) +
            " top-level entries)";
    return false;
  }

  const fs::path destination = fs::path(layout.releases_dir) / version;
  // A half-installed directory from an interrupted earlier attempt is the one
  // thing that can be here, and keeping it would mix two releases' files.
  fs::remove_all(destination, ec);
  ec.clear();
  fs::rename(unpacked, destination, ec);
  if (ec) {
    // Different filesystems (downloads/ on tmpfs, say) make rename fail; copy
    // then, rather than declaring the update impossible.
    ec.clear();
    fs::copy(unpacked, destination, fs::copy_options::recursive, ec);
    if (ec) {
      // Cleanup after a failed operation: its own error_code, because `ec` still
      // holds the failure being reported and is read after this.
      std::error_code cleanupEc;
      fs::remove_all(staging, cleanupEc);
      error = "could not move the unpacked release into " + destination.string() + ": " + ec.message();
      return false;
    }
  }
  fs::remove_all(staging, ec);

  // tar preserves the mode it was given, but a package built on a filesystem
  // that lost the bit, or unpacked under a umask that stripped it, leaves an
  // executable nobody can run -- and that only shows up at the restart, after
  // `current` has already moved.
#if !defined(_WIN32)
  ec.clear();
  fs::permissions(destination / "bin" / "hsf_gateway",
                  fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add,
                  ec);
#endif

  SeedDataDirectory(layout, destination.string());

  const State before = ReadState(layout);
  if (!SwapCurrent(layout, version, error)) return false;

  State state;
  state.pending = version;
  state.previous = fs::path(layout.active_release).filename().string();
  // Only a directory named as a version is a rollback target. On a filesystem
  // that would not resolve the `current` link, active_release is still the
  // link itself, and recording "current" as the version to fall back to would
  // point a future rollback at a directory that is not a release.
  {
    Version parsed;
    if (!Version::Parse(state.previous, parsed)) state.previous.clear();
  }
  if (state.previous.empty() || state.previous == version) {
    // Falling back to the release we just replaced only means something if it
    // is a different one; otherwise keep the last known-good already recorded.
    state.previous = before.previous;
  }
  state.attempts = 0;
  state.installed_at = NowIso8601Utc();
  WriteState(layout, state);

  Logger::Instance().Info(LogCategory::System, "Update: " + version + " installed and activated (previous: " +
                                                   (state.previous.empty() ? "none" : state.previous) + ")");
  return true;
}

bool UpdateInstaller::Activate(const Layout& layout, const std::string& version, std::string& error) {
  if (!layout.managed) {
    error = "this gateway is not an OTA-managed install";
    return false;
  }
  if (!IsSafeVersionString(version)) {
    error = "unexpected characters in version " + version;
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(fs::path(layout.releases_dir) / version, ec)) {
    error = "release " + version + " is not installed";
    return false;
  }
  return SwapCurrent(layout, version, error);
}

void UpdateInstaller::PruneReleases(const Layout& layout, int keep, const State& state) {
  if (keep <= 0) return;
  const std::string active = fs::path(layout.active_release).filename().string();
  const std::vector<std::string> versions = InstalledVersions(layout);

  int kept = 0;
  for (const std::string& version : versions) {
    // Never the running one, and never the rollback target -- pruning either is
    // how a device ends up with nothing to fall back to.
    if (version == active || version == state.previous || version == state.pending) continue;
    if (++kept <= keep) continue;
    std::error_code ec;
    fs::remove_all(fs::path(layout.releases_dir) / version, ec);
    if (!ec) Logger::Instance().Info(LogCategory::System, "Update: pruned old release " + version);
  }
}

UpdateInstaller::State UpdateInstaller::ReadState(const Layout& layout) {
  State state;
  std::ifstream in(layout.state_file);
  if (!in.is_open()) return state;
  try {
    json root;
    in >> root;
    state.pending = root.value("pending", std::string());
    state.previous = root.value("previous", std::string());
    state.attempts = root.value("attempts", 0);
    state.installed_at = root.value("installed_at", std::string());
    state.last_result = root.value("last_result", std::string());
  } catch (const std::exception& e) {
    Logger::Instance().Warning(LogCategory::System,
                               std::string("Update: ignoring unreadable ") + layout.state_file + ": " + e.what());
  }
  return state;
}

bool UpdateInstaller::WriteState(const Layout& layout, const State& state) {
  const json root = {{"pending", state.pending},
                     {"previous", state.previous},
                     {"attempts", state.attempts},
                     {"installed_at", state.installed_at},
                     {"last_result", state.last_result}};

  // Write-then-rename, because this file is read on the boot right after a
  // possible crash: a truncated one would read as "no update pending" and
  // silently disable the rollback that is the whole point of it.
  const fs::path temporary = fs::path(layout.state_file).string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open()) return false;
    out << root.dump(2) << "\n";
    if (!out.good()) return false;
  }
  std::error_code ec;
  fs::rename(temporary, layout.state_file, ec);
  if (ec) {
    // Cleanup after a failed operation: its own error_code, because `ec` still
    // holds the failure being reported and is read after this.
    std::error_code cleanupEc;
    fs::remove(temporary, cleanupEc);
    return false;
  }
  return true;
}

bool UpdateInstaller::CheckBoot(const Layout& layout, const std::string& runningVersion, int maxAttempts,
                                std::string& rolledBackTo) {
  rolledBackTo.clear();
  if (!layout.managed) return false;

  State state = ReadState(layout);
  if (state.pending.empty()) return false;

  if (state.pending != runningVersion) {
    // Either the rollback below already happened and this is the fallback
    // booting, or someone repointed `current` by hand. Either way the trial is
    // over and its state is stale.
    Logger::Instance().Warning(LogCategory::System, "Update: expected " + state.pending +
                                                        " to be running but this is " + runningVersion +
                                                        "; clearing the pending update state");
    state.pending.clear();
    state.last_result = "abandoned";
    WriteState(layout, state);
    return false;
  }

  ++state.attempts;
  if (state.attempts <= maxAttempts) {
    Logger::Instance().Info(LogCategory::System, "Update: " + runningVersion + " is on trial (boot " +
                                                     std::to_string(state.attempts) + " of " +
                                                     std::to_string(maxAttempts) + ")");
    WriteState(layout, state);
    return false;
  }

  if (state.previous.empty()) {
    Logger::Instance().Error(LogCategory::System, "Update: " + runningVersion + " has failed " +
                                                      std::to_string(state.attempts) +
                                                      " boots but there is no previous release to fall back to");
    state.pending.clear();
    state.last_result = "failed-no-rollback-target";
    WriteState(layout, state);
    return false;
  }

  std::string error;
  if (!Activate(layout, state.previous, error)) {
    Logger::Instance().Error(LogCategory::System, "Update: rollback to " + state.previous + " failed: " + error);
    return false;
  }

  Logger::Instance().Error(LogCategory::System, "Update: " + runningVersion + " failed to start " +
                                                    std::to_string(state.attempts) + " times; rolled back to " +
                                                    state.previous);
  rolledBackTo = state.previous;
  // Just the version after the prefix, no prose: UpdateManager::Configure
  // parses this back out to populate `rolled_back_from` in the status frame,
  // and the wording belongs to whatever renders it.
  state.last_result = "rolled-back:" + runningVersion;
  state.pending.clear();
  state.attempts = 0;
  WriteState(layout, state);
  return true;
}

void UpdateInstaller::ConfirmHealthy(const Layout& layout, const std::string& runningVersion) {
  if (!layout.managed) return;
  State state = ReadState(layout);
  if (state.pending.empty()) return;
  if (state.pending != runningVersion) return;

  Logger::Instance().Info(LogCategory::System, "Update: " + runningVersion + " confirmed healthy");
  state.pending.clear();
  state.attempts = 0;
  state.last_result = "confirmed";
  WriteState(layout, state);
}

}  // namespace hsf
