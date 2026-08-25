#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/ConfigManager.h"
#include "hsf/update/UpdateInstaller.h"

namespace hsf {

// Over-the-air updates: asks the release server what the newest build is,
// and -- only when a human says yes -- downloads it, verifies it, unpacks it
// into its own release directory and restarts into it.
//
// NOTHING HERE INSTALLS ON ITS OWN. The check runs on a timer, but the result
// is a notification; installing needs an explicit Install() call, which comes
// from the Update button in the web UI. The single exception is a manifest
// marked `mandatory`, and even that only removes the "Later" button from the
// popup -- it still waits for the click, because this gateway may be holding a
// locker door open for someone standing in front of it.
//
// One background thread does all the work, driven by a command queue of
// exactly one slot. The web thread never blocks on a download: it posts a
// command, gets the current status back, and watches the rest arrive over the
// WebSocket. State transitions push a frame out through the OnStateChanged
// callback WebServer installs.
class UpdateManager {
 public:
  enum class State {
    kDisabled,     // update.enabled is off
    kIdle,         // nothing to do; last check found nothing newer
    kChecking,     // manifest request in flight
    kAvailable,    // a newer version exists and is waiting for a decision
    kDownloading,  // fetching the package
    kVerifying,    // checksum + signature
    kInstalling,   // unpacking and repointing `current`
    kReady,        // installed; restarting into it
    kError         // last operation failed; `error` says why
  };

  // One release, as the manifest describes it.
  struct Manifest {
    std::string version;
    std::string platform;
    std::string download_url;
    std::string sha256;
    std::string signature;      // base64, detached; see SignatureVerifier
    std::string release_date;
    std::string min_version;    // below this, the update is forced
    bool mandatory = false;
    std::vector<std::string> release_notes;
    int64_t size_bytes = 0;
  };

  UpdateManager();
  ~UpdateManager();

  // `executableDir` is what main() already computed for resource lookup; it is
  // how the managed-install layout is found. `currentVersion` is HSF_VERSION.
  void Configure(const UpdateConfig& config, const std::string& executableDir, const std::string& platform,
                 const std::string& currentVersion, const std::string& dataDir);

  // Called on every status change with the frame to broadcast. Installed by
  // WebServer; may be empty.
  void SetStateCallback(std::function<void(const nlohmann::json&)> callback);

  // Invoked when the gateway must restart into a freshly installed release.
  // main() wires this to the same flag SIGTERM sets, so shutdown runs its
  // normal course -- scripts stopped, doors left in a defined state, databases
  // closed -- instead of the process vanishing mid-transaction.
  void SetRestartCallback(std::function<void()> callback);

  // Starts the background thread: the boot check first (which may roll back
  // and ask for a restart before anything else runs), then the periodic
  // version check.
  void Start();
  void Stop();

  // Asks for a check now, out of band with the timer. Returns immediately.
  void CheckNow();

  // Downloads, verifies and installs `version`, then restarts. `version` must
  // match what the last check found -- a request naming anything else is
  // refused, so a stale browser tab cannot install a release this gateway has
  // never seen a manifest for.
  bool Install(const std::string& version, std::string& error);

  // Hides the popup for this version until the gateway restarts or a newer
  // version appears. Refused for a mandatory update.
  bool Dismiss(const std::string& version, std::string& error);

  // Not const: it re-reads the `update` config section first, so a setting
  // saved on the Configuration page is reflected on the very next poll rather
  // than whenever the worker thread next wakes.
  nlohmann::json StatusJson();

  // True when the status has changed since `lastSeq`; the broadcast loop uses
  // it to avoid pushing an identical frame every second.
  uint64_t StatusSeq() const { return statusSeq_.load(); }

  static std::string StateName(State state);

 private:
  enum class Command { kNone, kCheck, kInstall };

  void Worker();
  void RunBootCheck();
  bool FetchManifest(Manifest& manifest, std::string& error);
  bool DownloadAndInstall(const Manifest& manifest, std::string& error);
  bool VerifyPackage(const Manifest& manifest, const std::string& archivePath, std::string& error);

  // Pulls the `update` section back out of ConfigManager. Called with mutex_
  // held, from every path a browser can reach: the worker only re-reads
  // config on its own tick, so without this, enabling updates on the
  // Configuration page left the status endpoint reporting "disabled" -- and
  // Install() refusing -- for up to half a minute after the operator saved.
  void RefreshConfig();

  void SetState(State state, const std::string& error = std::string());
  void SetProgress(int64_t received, int64_t total);
  void Publish();
  nlohmann::json StatusJsonUnlocked() const;
  UpdateConfig ConfigSnapshot() const;

  mutable std::mutex mutex_;
  UpdateConfig config_;
  std::string executableDir_;
  std::string platform_;
  std::string currentVersion_;
  std::string dataDir_;
  UpdateInstaller::Layout layout_;

  State state_ = State::kDisabled;
  std::string error_;
  std::string lastCheck_;      // ISO-8601 UTC, "" when never
  std::string dismissed_;      // version the operator postponed
  Manifest available_;         // valid when state_ is kAvailable or later
  int64_t progressBytes_ = 0;
  int64_t progressTotal_ = 0;
  std::string rolledBackFrom_; // set when this boot followed a failed update

  std::atomic<uint64_t> statusSeq_{0};
  std::function<void(const nlohmann::json&)> onStateChanged_;
  std::function<void()> onRestart_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancelDownload_{false};
  std::condition_variable wake_;
  Command command_ = Command::kNone;
  std::string commandVersion_;
};

}  // namespace hsf
