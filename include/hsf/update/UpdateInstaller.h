#pragma once

#include <string>
#include <vector>

namespace hsf {

// The on-disk half of OTA updating: unpacking a verified package into its own
// release directory, pointing `current` at it, and putting `current` back if
// the new build cannot get through a boot.
//
// THE LAYOUT. Everything here assumes the managed tree that
// scripts/install-ota.sh creates:
//
//     <root>/
//     |-- current -> releases/1.2.3     symlink (POSIX) / junction (Windows)
//     |-- releases/
//     |   |-- 1.2.2/  bin/ web/ config/ docs/ ...
//     |   `-- 1.2.3/
//     |-- data/        config.db, clients.db, logs.db, logs/, the public key
//     |-- downloads/   archives and extraction staging, safe to delete
//     `-- update-state.json
//
// The split between `releases/` and `data/` is the load-bearing part: a
// release directory is disposable and is replaced wholesale, so anything the
// installation owns -- its databases, its keys, its logs -- has to live
// outside one. That is why the service runs the gateway as
// `current/bin/hsf_gateway <root>/data/config.db`: main() derives every other
// state path from the config file's directory, so one argument keeps all of it
// out of the release tree.
//
// A gateway NOT running from this layout (a development build, an unzipped
// package run in place) reports Detect() == false and refuses to install
// anything. Rewriting a directory the operator laid out by hand, on a guess
// about what it means, is worse than declining.
class UpdateInstaller {
 public:
  struct Layout {
    bool managed = false;      // false: this gateway cannot self-update
    std::string root;          // <root>
    std::string releases_dir;  // <root>/releases
    std::string current_link;  // <root>/current
    std::string data_dir;      // <root>/data
    std::string downloads_dir; // <root>/downloads
    std::string state_file;    // <root>/update-state.json
    std::string active_release;// resolved target of `current`, e.g. ".../releases/1.2.3"
    std::string reason;        // when !managed, why
  };

  // Boot state, persisted across restarts in update-state.json. This is what
  // makes rollback work with no agent outside the process: the installer
  // writes "1.2.3 is on trial" before restarting, every boot of 1.2.3
  // increments a counter, and a boot that survives long enough clears it. A
  // build that dies during startup therefore never clears it, and the boot
  // after the limit puts `current` back.
  struct State {
    std::string pending;      // version on trial, "" when nothing is
    std::string previous;     // version to fall back to
    int attempts = 0;         // boots of `pending` so far
    std::string installed_at; // ISO-8601 UTC
    std::string last_result;  // "confirmed" | "rolled-back:<reason>" | ""
  };

  // Works out where this gateway is installed. `overrideRoot` (config
  // update.install_root) wins when set; otherwise the root is inferred from
  // the running executable's own path -- <root>/releases/<version>/bin, since
  // /proc/self/exe has already resolved the `current` symlink by the time
  // main() sees it.
  static Layout Detect(const std::string& executableDir, const std::string& overrideRoot);

  // Unpacks `archivePath` into <root>/releases/<version> and repoints
  // `current`. Returns false with `error` set and the previous `current`
  // untouched on any failure before the swap.
  //
  // Refuses a version string containing anything outside [A-Za-z0-9.+-]: it
  // becomes a directory name and a shell argument, and a manifest is remote
  // input.
  static bool Install(const Layout& layout, const std::string& version, const std::string& archivePath,
                       std::string& error);

  // Points `current` back at `version` (used both by the boot-failure path and
  // by an operator asking for it).
  static bool Activate(const Layout& layout, const std::string& version, std::string& error);

  static std::vector<std::string> InstalledVersions(const Layout& layout);

  // Deletes all but the newest `keep` releases, never touching the active one
  // or the rollback target.
  static void PruneReleases(const Layout& layout, int keep, const State& state);

  static State ReadState(const Layout& layout);
  static bool WriteState(const Layout& layout, const State& state);

  // --- boot lifecycle -----------------------------------------------------

  // Called once at startup, before anything else can crash. Increments the
  // trial counter, and rolls `current` back when `runningVersion` has now
  // failed to settle `maxAttempts` times. `rolledBackTo` names the version it
  // reverted to, and the caller is expected to restart the process.
  static bool CheckBoot(const Layout& layout, const std::string& runningVersion, int maxAttempts,
                         std::string& rolledBackTo);

  // Called once the gateway has been up long enough to count as working.
  // Clears the trial state so the next boot is an ordinary one.
  static void ConfirmHealthy(const Layout& layout, const std::string& runningVersion);

 private:
  static bool ExtractArchive(const std::string& archivePath, const std::string& destinationDir, std::string& error);
  static bool SwapCurrent(const Layout& layout, const std::string& releaseDirName, std::string& error);
  static void SeedDataDirectory(const Layout& layout, const std::string& releaseDir);
};

}  // namespace hsf
