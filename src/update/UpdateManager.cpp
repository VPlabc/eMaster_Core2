#include "hsf/update/UpdateManager.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "hsf/Logger.h"
#include "hsf/RestClient.h"
#include "hsf/update/Downloader.h"
#include "hsf/update/Sha256.h"
#include "hsf/update/SignatureVerifier.h"
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

// Percent-encodes the few characters that could appear in a machine id or a
// channel name and change the meaning of the query string.
std::string UrlEncode(const std::string& text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// A manifest served as a static file can carry a relative download path, so
// one version.json works whatever host it ends up on.
std::string ResolveUrl(const std::string& base, const std::string& reference) {
  if (reference.empty()) return reference;
  if (reference.rfind("http://", 0) == 0 || reference.rfind("https://", 0) == 0) return reference;

  const size_t schemeEnd = base.find("://");
  if (schemeEnd == std::string::npos) return reference;
  if (reference.front() == '/') {
    const size_t hostEnd = base.find('/', schemeEnd + 3);
    return (hostEnd == std::string::npos ? base : base.substr(0, hostEnd)) + reference;
  }
  size_t lastSlash = base.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash < schemeEnd + 3) return base + "/" + reference;
  // Drop any query string before treating the tail as a directory.
  std::string directory = base.substr(0, lastSlash + 1);
  const size_t question = directory.find('?');
  if (question != std::string::npos) directory = directory.substr(0, question);
  return directory + reference;
}

std::vector<std::string> ReadNotes(const json& node) {
  std::vector<std::string> notes;
  if (node.is_array()) {
    for (const auto& item : node) {
      if (item.is_string()) notes.push_back(item.get<std::string>());
    }
  } else if (node.is_string()) {
    notes.push_back(node.get<std::string>());
  }
  return notes;
}

}  // namespace

UpdateManager::UpdateManager() = default;

UpdateManager::~UpdateManager() { Stop(); }

std::string UpdateManager::StateName(State state) {
  switch (state) {
    case State::kDisabled: return "disabled";
    case State::kIdle: return "idle";
    case State::kChecking: return "checking";
    case State::kAvailable: return "available";
    case State::kDownloading: return "downloading";
    case State::kVerifying: return "verifying";
    case State::kInstalling: return "installing";
    case State::kReady: return "ready";
    case State::kError: return "error";
  }
  return "unknown";
}

void UpdateManager::Configure(const UpdateConfig& config, const std::string& executableDir,
                              const std::string& platform, const std::string& currentVersion,
                              const std::string& dataDir) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  executableDir_ = executableDir;
  platform_ = platform;
  currentVersion_ = currentVersion;
  dataDir_ = dataDir;
  layout_ = UpdateInstaller::Detect(executableDir, config.install_root);
  state_ = config.enabled ? State::kIdle : State::kDisabled;

  // Surface a rollback that main()'s boot guard performed on the PREVIOUS run.
  // The operator clicked Update and then found the same version still running;
  // without this the status page has no explanation to offer.
  if (layout_.managed) {
    const UpdateInstaller::State bootState = UpdateInstaller::ReadState(layout_);
    const std::string prefix = "rolled-back:";
    if (bootState.last_result.rfind(prefix, 0) == 0) {
      rolledBackFrom_ = bootState.last_result.substr(prefix.size());
    }
  }
}

void UpdateManager::SetStateCallback(std::function<void(const json&)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  onStateChanged_ = std::move(callback);
}

void UpdateManager::SetRestartCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  onRestart_ = std::move(callback);
}

UpdateConfig UpdateManager::ConfigSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void UpdateManager::RefreshConfig() {
  const std::string previousRoot = config_.install_root;
  config_ = ConfigManager::Instance().GetUpdate();

  // Re-detecting the layout is only warranted when the thing it is derived
  // from changed; it touches the filesystem, and StatusJson() is polled.
  if (config_.install_root != previousRoot) {
    layout_ = UpdateInstaller::Detect(executableDir_, config_.install_root);
  }
  if (config_.enabled && state_ == State::kDisabled) {
    state_ = State::kIdle;
  } else if (!config_.enabled && state_ != State::kDisabled) {
    state_ = State::kDisabled;
  }
}

void UpdateManager::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { Worker(); });
}

void UpdateManager::Stop() {
  if (!running_.exchange(false)) return;
  cancelDownload_.store(true);
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void UpdateManager::CheckNow() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshConfig();
    if (!config_.enabled) return;
    command_ = Command::kCheck;
  }
  wake_.notify_all();
}

bool UpdateManager::Install(const std::string& version, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshConfig();
    if (!config_.enabled) {
      error = "over-the-air updates are switched off (update.enabled)";
      return false;
    }
    if (!layout_.managed) {
      error = layout_.reason;
      return false;
    }
    // The request must name the release the last check actually found. Without
    // this, an old browser tab -- or anything else that can POST -- could name
    // an arbitrary version and have the gateway go looking for it.
    if (available_.version.empty() || available_.version != version) {
      error = "no manifest for version " + version + "; run a check first";
      return false;
    }
    if (state_ == State::kDownloading || state_ == State::kVerifying || state_ == State::kInstalling ||
        state_ == State::kReady) {
      error = "an update is already in progress";
      return false;
    }
    command_ = Command::kInstall;
    commandVersion_ = version;
    cancelDownload_.store(false);
  }
  wake_.notify_all();
  return true;
}

bool UpdateManager::Dismiss(const std::string& version, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (available_.version != version) {
    error = "version " + version + " is not the one being offered";
    return false;
  }
  if (available_.mandatory) {
    error = "this is a mandatory update and cannot be postponed";
    return false;
  }
  dismissed_ = version;
  ++statusSeq_;
  return true;
}

void UpdateManager::SetState(State state, const std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    error_ = error;
    ++statusSeq_;
  }
  Publish();
}

void UpdateManager::SetProgress(int64_t received, int64_t total) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    progressBytes_ = received;
    progressTotal_ = total;
    ++statusSeq_;
  }
  Publish();
}

void UpdateManager::Publish() {
  // Called with mutex_ HELD, deliberately. The callback WebServer installs
  // does one thing -- park the frame under its own small mutex -- and it holds
  // a raw pointer to the web layer's pimpl. Copying the std::function out and
  // invoking it after unlocking would leave a window where WebServer::Stop()
  // has already cleared the slot and torn itself down while this thread is
  // still holding the copy. Nothing the callback touches can call back in
  // here, so there is no lock cycle to worry about.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!onStateChanged_) return;
  json payload = StatusJsonUnlocked();
  payload["type"] = "update";
  onStateChanged_(payload);
}

json UpdateManager::StatusJson() {
  std::lock_guard<std::mutex> lock(mutex_);
  RefreshConfig();
  return StatusJsonUnlocked();
}

json UpdateManager::StatusJsonUnlocked() const {
  json status = {{"state", StateName(state_)},
                 {"enabled", config_.enabled},
                 {"current_version", currentVersion_},
                 {"platform", platform_},
                 {"channel", config_.channel},
                 {"last_check", lastCheck_},
                 {"error", error_},
                 {"managed", layout_.managed},
                 {"signature_backend", SignatureVerifier::Backend()},
                 {"require_signature", config_.require_signature}};

  if (!layout_.managed) status["managed_reason"] = layout_.reason;
  if (!rolledBackFrom_.empty()) status["rolled_back_from"] = rolledBackFrom_;

  const bool haveOffer = !available_.version.empty() && IsNewer(available_.version, currentVersion_);
  status["available"] = haveOffer;
  if (haveOffer) {
    status["version"] = available_.version;
    status["mandatory"] = available_.mandatory;
    status["release_date"] = available_.release_date;
    status["release_notes"] = available_.release_notes;
    status["size_bytes"] = available_.size_bytes;
    // A dismissed update is still "available"; the UI just doesn't pop it up
    // again until a newer one arrives or the gateway restarts.
    status["dismissed"] = (dismissed_ == available_.version);
  }

  if (state_ == State::kDownloading) {
    status["progress_bytes"] = progressBytes_;
    status["progress_total"] = progressTotal_;
    status["progress_percent"] =
        progressTotal_ > 0 ? static_cast<int>((progressBytes_ * 100) / progressTotal_) : -1;
  }
  return status;
}

void UpdateManager::RunBootCheck() {
  // Only the HEALTHY half of the boot lifecycle lives here. The failure half
  // -- counting boots and rolling `current` back -- runs in main() before any
  // hardware is touched, and it has to: a release that dies while opening the
  // serial port or loading a script never reaches this thread, so a counter
  // incremented here would sit at zero forever and the rollback would never
  // fire. The rule is that the attempt is recorded as early as a boot can be
  // observed, and cleared as late as one can be trusted.
  UpdateConfig config;
  UpdateInstaller::Layout layout;
  std::string version;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config = config_;
    layout = layout_;
    version = currentVersion_;
  }
  if (!layout.managed) return;

  const UpdateInstaller::State state = UpdateInstaller::ReadState(layout);
  if (state.pending.empty() || state.pending != version) return;

  const int wait = config.health_confirm_sec > 0 ? config.health_confirm_sec : 120;
  Logger::Instance().Info(LogCategory::System, "Update: " + version + " is on trial; confirming in " +
                                                   std::to_string(wait) + "s if it is still running");
  {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::seconds(wait), [this] { return !running_.load(); });
  }
  if (!running_.load()) return;

  UpdateInstaller::ConfirmHealthy(layout, version);
  UpdateInstaller::PruneReleases(layout, config.keep_releases, UpdateInstaller::ReadState(layout));
}

bool UpdateManager::FetchManifest(Manifest& manifest, std::string& error) {
  UpdateConfig config;
  std::string platform;
  std::string current;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config = config_;
    platform = platform_;
    current = currentVersion_;
  }
  if (config.url.empty()) {
    error = "no update server configured (update.url)";
    return false;
  }

  RestRequestSpec spec;
  spec.method = "GET";
  spec.url = config.url + (config.url.find('?') == std::string::npos ? "?" : "&") + "platform=" +
             UrlEncode(platform) + "&channel=" + UrlEncode(config.channel) + "&version=" + UrlEncode(current) +
             "&device_id=" + UrlEncode(ConfigManager::Instance().GetSystem().machine_id);
  spec.timeout_ms = config.timeout_sec * 1000;
  spec.verify_ssl = config.ssl_verify;
  spec.headers["Accept"] = "application/json";

  const RestResponse response = RestClient::Send(spec);
  if (!response.ok) {
    error = response.error.empty() ? ("update server returned HTTP " + std::to_string(response.status_code))
                                   : response.error;
    return false;
  }

  json root;
  try {
    root = json::parse(response.body);
  } catch (const std::exception& e) {
    error = std::string("update server did not return JSON: ") + e.what();
    return false;
  }
  if (!root.is_object()) {
    error = "update manifest is not a JSON object";
    return false;
  }

  // Three shapes are accepted, because all three are things a release pipeline
  // reasonably produces: a per-device endpoint answering with one release, a
  // static version.json describing one release, and a static version.json with
  // a `platforms` map so a single file serves every build of a tag.
  json entry = root;
  if (root.contains("platforms") && root["platforms"].is_object()) {
    const auto it = root["platforms"].find(platform);
    if (it == root["platforms"].end()) {
      error = "the manifest lists no build for " + platform;
      return false;
    }
    entry = *it;
    // Fields the map entry doesn't override are inherited from the top level
    // (version, notes and the mandatory flag are normally per-release, not
    // per-platform).
    for (const auto& [key, value] : root.items()) {
      if (key == "platforms") continue;
      if (!entry.contains(key)) entry[key] = value;
    }
  }

  if (entry.contains("available") && entry["available"].is_boolean() && !entry["available"].get<bool>()) {
    manifest = Manifest();
    return true;  // server says nothing newer; not an error
  }

  manifest.version = entry.value("version", std::string());
  manifest.platform = entry.value("platform", platform);
  manifest.download_url = ResolveUrl(config.url, entry.contains("download_url")
                                                     ? entry.value("download_url", std::string())
                                                     : entry.value("url", std::string()));
  manifest.sha256 = entry.value("sha256", std::string());
  manifest.signature = entry.value("signature", std::string());
  manifest.release_date = entry.value("release_date", std::string());
  manifest.min_version = entry.value("min_version", std::string());
  manifest.mandatory = entry.value("mandatory", false);
  manifest.size_bytes = entry.value("size_bytes", entry.value("size", static_cast<int64_t>(0)));
  if (entry.contains("release_notes")) manifest.release_notes = ReadNotes(entry["release_notes"]);

  if (manifest.version.empty()) {
    error = "the manifest has no version field";
    return false;
  }
  // min_version is the pipeline saying "anything older than this must not stay
  // in the field" -- a security fix, normally. It escalates the offer to
  // mandatory; it never installs anything by itself.
  if (!manifest.min_version.empty() && IsNewer(manifest.min_version, current)) manifest.mandatory = true;
  return true;
}

bool UpdateManager::VerifyPackage(const Manifest& manifest, const std::string& archivePath, std::string& error) {
  const UpdateConfig config = ConfigSnapshot();

  if (manifest.sha256.empty()) {
    error = "the manifest carries no sha256, so the download cannot be checked";
    return false;
  }
  const std::string actual = Sha256::HexOfFile(archivePath);
  if (actual.empty()) {
    error = "could not hash the downloaded package";
    return false;
  }
  if (!Sha256::HexEquals(actual, manifest.sha256)) {
    error = "checksum mismatch: the package is not the one the manifest describes (expected " + manifest.sha256 +
            ", got " + actual + ")";
    return false;
  }

  if (!config.require_signature) {
    Logger::Instance().Warning(LogCategory::System,
                               "Update: installing " + manifest.version +
                                   " with signature verification disabled (update.require_signature is off)");
    return true;
  }

  if (manifest.signature.empty()) {
    error = "the manifest carries no signature and update.require_signature is on";
    return false;
  }

  fs::path keyPath = config.public_key_path;
  if (keyPath.is_relative()) {
    std::lock_guard<std::mutex> lock(mutex_);
    keyPath = fs::path(dataDir_) / keyPath;
  }

  // The digest is what is signed, not the archive -- and it is the digest we
  // just proved the bytes hash to, so this still authenticates the package
  // itself. See SignatureVerifier for why version and platform are in there.
  const std::string message = SignatureVerifier::BuildSignedMessage(manifest.version, manifest.platform, actual);
  std::string signatureError;
  if (!SignatureVerifier::Verify(keyPath.string(), message, manifest.signature, signatureError)) {
    error = "signature check failed: " + signatureError;
    return false;
  }
  Logger::Instance().Info(LogCategory::System, "Update: signature verified for " + manifest.version);
  return true;
}

bool UpdateManager::DownloadAndInstall(const Manifest& manifest, std::string& error) {
  UpdateConfig config;
  UpdateInstaller::Layout layout;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config = config_;
    layout = layout_;
  }
  if (manifest.download_url.empty()) {
    error = "the manifest has no download url";
    return false;
  }

  const fs::path archive = fs::path(layout.downloads_dir) / ("hsf-gateway-" + manifest.version + ".pkg");
  SetState(State::kDownloading);
  SetProgress(0, manifest.size_bytes);

  const DownloadResult download =
      Downloader::ToFile(manifest.download_url, archive.string(), config.timeout_sec, config.ssl_verify,
                         &cancelDownload_, [this](int64_t received, int64_t total) { SetProgress(received, total); });
  if (!download.ok) {
    error = "download failed: " + download.error;
    return false;
  }
  Logger::Instance().Info(LogCategory::System, "Update: downloaded " + std::to_string(download.bytes) +
                                                   " bytes for " + manifest.version);

  SetState(State::kVerifying);
  if (!VerifyPackage(manifest, archive.string(), error)) {
    // A package that fails verification is deleted, not kept for inspection:
    // leaving an unverified archive on the device is how it eventually gets
    // installed by someone in a hurry.
    std::error_code ec;
    fs::remove(archive, ec);
    return false;
  }

  SetState(State::kInstalling);
  if (!UpdateInstaller::Install(layout, manifest.version, archive.string(), error)) return false;

  std::error_code ec;
  fs::remove(archive, ec);
  return true;
}

void UpdateManager::Worker() {
  RunBootCheck();

  // The thread runs whether or not updates are switched on, and the loop below
  // re-reads the setting every pass -- so turning them on from the
  // Configuration page takes effect without a restart, like every other
  // section (see main()'s poll thread).
  UpdateConfig config = ConfigSnapshot();

  // The startup grace from request/CICD.md: let the serial ports open, the PLC
  // connect and the scripts settle before adding an HTTPS request to the mix.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::seconds(config.initial_delay_sec > 0 ? config.initial_delay_sec : 30),
                   [this] { return !running_.load() || command_ != Command::kNone; });
  }

  auto nextCheck = std::chrono::steady_clock::now();

  while (running_.load()) {
    Command command = Command::kNone;
    std::string version;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Re-read config every pass, same as the poll thread in main().
      const State before = state_;
      RefreshConfig();
      config = config_;
      if (state_ != before) ++statusSeq_;

      command = command_;
      version = commandVersion_;
      command_ = Command::kNone;
      commandVersion_.clear();

      if (command == Command::kNone) {
        const auto now = std::chrono::steady_clock::now();
        if (config.enabled && now >= nextCheck) {
          command = Command::kCheck;
        } else {
          // Wake for whichever comes first: the timer, a command, or shutdown.
          auto wait = config.enabled ? std::chrono::duration_cast<std::chrono::seconds>(nextCheck - now)
                                     : std::chrono::seconds(30);
          if (wait < std::chrono::seconds(1)) wait = std::chrono::seconds(1);
          if (wait > std::chrono::seconds(60)) wait = std::chrono::seconds(60);
          wake_.wait_for(lock, wait, [this] { return !running_.load() || command_ != Command::kNone; });
          continue;
        }
      }
    }
    if (!running_.load()) return;

    if (command == Command::kCheck) {
      const int hours = config.check_interval_hours > 0 ? config.check_interval_hours : 6;
      nextCheck = std::chrono::steady_clock::now() + std::chrono::hours(hours);

      SetState(State::kChecking);
      Manifest manifest;
      std::string error;
      if (!FetchManifest(manifest, error)) {
        Logger::Instance().Warning(LogCategory::System, "Update check failed: " + error);
        SetState(State::kError, error);
        continue;
      }

      std::string current;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        lastCheck_ = NowIso8601Utc();
        current = currentVersion_;
        if (!manifest.version.empty() && manifest.version != available_.version) {
          // A different release than the one that was postponed: offer it again.
          dismissed_.clear();
        }
        available_ = manifest;
      }

      if (!manifest.version.empty() && IsNewer(manifest.version, current)) {
        Logger::Instance().Info(LogCategory::System, "Update available: " + current + " -> " + manifest.version +
                                                         (manifest.mandatory ? " (mandatory)" : ""));
        SetState(State::kAvailable);
      } else {
        Logger::Instance().Info(LogCategory::System, "Update check: " + current + " is current");
        SetState(State::kIdle);
      }
      continue;
    }

    if (command == Command::kInstall) {
      Manifest manifest;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        manifest = available_;
      }
      if (manifest.version != version) {
        SetState(State::kError, "the offered version changed while the request was queued");
        continue;
      }

      std::string error;
      if (!DownloadAndInstall(manifest, error)) {
        Logger::Instance().Error(LogCategory::System, "Update to " + version + " failed: " + error);
        SetState(State::kError, error);
        continue;
      }

      SetState(State::kReady);
      Logger::Instance().Info(LogCategory::System, "Update: restarting into " + version);

      // Let the "restarting" frame reach the browser before the socket closes,
      // so the popup ends on "restarting" rather than on a dead progress bar.
      std::this_thread::sleep_for(std::chrono::seconds(2));

      std::function<void()> restart;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        restart = onRestart_;
      }
      if (!config.restart_command.empty()) {
        std::system(config.restart_command.c_str());
      } else if (restart) {
        restart();
      } else {
        Logger::Instance().Warning(LogCategory::System,
                                   "Update: installed, but nothing is wired up to restart the gateway. "
                                   "It will run the new release at the next restart.");
      }
      return;
    }
  }
}

}  // namespace hsf
