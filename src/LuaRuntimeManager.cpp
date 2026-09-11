#include "hsf/LuaRuntimeManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

#include "hsf/CitizenIdParser.h"
#include "hsf/ConfigManager.h"
#include "hsf/LogStore.h"
#include "hsf/Logger.h"
#include "hsf/ModbusRegistry.h"
#include "hsf/MqClient.h"
#include "hsf/RuntimeVariables.h"
#include "hsf/SerialPort.h"
#include "hsf/zk_controller/ZkController.h"

namespace hsf {

namespace {

// How long StartScript waits for a script's top-level code to settle before
// reporting "started". Long enough that a syntax error, a failed require or a
// script that returns straight away is reported inline to whoever pressed Run;
// short enough that an intentional polling loop doesn't hold the HTTP request
// open. A loop script simply reports success and keeps running.
constexpr int kStartDecisionTimeoutMs = 1500;

constexpr const char* kEditorBufferName = "(editor buffer)";

// Built-in log type for section 23's script error record. Registered from code
// rather than from log_definitions.json so a runtime fault is still recorded
// on an installation whose definitions file was deleted or never shipped.
LogTypeDef ScriptErrorDefinition() {
  LogTypeDef def;
  def.type = "script_error";
  def.description = "Lua script runtime error (gateway built-in)";
  def.fields = {{"script", "string", true},   {"path", "string", false},
                {"error", "string", true},    {"error_type", "string", false},
                {"line", "integer", false},   {"runtime_id", "integer", false}};
  return def;
}

// Classifies an error message for the `error_type` field. Lua does not tag its
// errors, so this reads the text -- crude, but the alternative is a column
// that always says "runtime".
std::string ClassifyError(const std::string& message) {
  auto has = [&message](const char* needle) { return message.find(needle) != std::string::npos; };
  if (has("syntax error") || has("unexpected symbol") || has("'<eof>'")) return "syntax";
  if (has("module '") && has("not found")) return "require";
  if (has("stack overflow")) return "stack_overflow";
  if (has("not enough memory")) return "memory";
  if (has("interrupted by Stop()")) return "interrupted";
  return "runtime";
}

}  // namespace

const char* LuaRuntimeManager::EditorBufferName() { return kEditorBufferName; }

LuaRuntimeManager::LuaRuntimeManager() = default;

LuaRuntimeManager::~LuaRuntimeManager() { StopAll(); }

void LuaRuntimeManager::Bind(ServiceRegistry* services) {
  services_ = services;
  SerialPort* serial = services_ ? services_->Get<SerialPort>(ServiceNames::kSerial) : nullptr;
  ZkController* zk = services_ ? services_->Get<ZkController>(ServiceNames::kZk) : nullptr;
  MqClient* mq = services_ ? services_->Get<MqClient>(ServiceNames::kMq) : nullptr;

  // The device callbacks live here, not on an engine: each device has exactly
  // one callback slot, and with several scripts running there is no single
  // engine that should own it. Every event is delivered to every live runtime.
  if (serial) {
    // Fires on SerialPort's own read thread; queued rather than dispatched, so
    // a script sitting in its own polling loop (holding its engine mutex for
    // the whole run) can still receive it -- see LuaEngine::QueueEvent.
    serial->SetDataCallback([this](const std::string& line) {
      auto parsed = CitizenIdParser::Parse(line);
      if (parsed) {
        QueueEventAll("OnCitizenCardRead", CitizenIdParser::ToJson(*parsed).dump());
      } else {
        Logger::Instance().Warning(LogCategory::Serial,
                                    "Received serial line that isn't a recognized ID card format");
        QueueEventAll("OnCitizenCardRead", line);
      }
    });
  }

  if (zk) {
    // All three fire on ZkController's RunLoop thread, which is what
    // LuaEngine::QueueZk*Event requires (it tries to deliver immediately with
    // a try_lock, and try_lock on a mutex the calling thread already holds is
    // undefined behaviour).
    zk->SetCardCallback([this](const ZkCardEvent& event) {
      for (const auto& runtime : SnapshotRuntimes()) {
        if (runtime->engine->IsRunning()) runtime->engine->QueueZkCardEvent(event);
      }
    });
    zk->SetRawCallback([this](const std::string& raw) {
      for (const auto& runtime : SnapshotRuntimes()) {
        if (runtime->engine->IsRunning()) runtime->engine->QueueZkRawEvent(raw);
      }
    });
    zk->SetConnectionCallback([this](bool connected) {
      for (const auto& runtime : SnapshotRuntimes()) {
        if (runtime->engine->IsRunning()) runtime->engine->QueueZkConnectionEvent(connected);
      }
    });
    // Auxiliary input edges (RTLog events 220/221). Same thread, same fan-out:
    // a door sensor or a request-to-exit button wired to the panel's input is
    // something several scripts may legitimately watch.
    zk->SetAuxInputCallback([this](int input, bool shorted) {
      for (const auto& runtime : SnapshotRuntimes()) {
        if (runtime->engine->IsRunning()) runtime->engine->QueueZkAuxInputEvent(input, shorted);
      }
    });
  }

  if (mq) {
    // Fires on MqClient's IO thread. Only a notification: the message is
    // already in MqClient's inbox and already acked, so a script may either
    // handle OnMqMessage or poll Mq.Available()/Mq.Get(), and several scripts
    // watching the same broker do not fight over one delivery -- whichever one
    // calls Mq.Get() first takes it out of the shared inbox.
    mq->SetMessageCallback([this](const MqMessage& message) {
      RuntimeVariables::Instance().Set("LastMqMessage", message.body);
      QueueEventAll("OnMqMessage", message.body);
    });
  }

  LogStore::Instance().DefineType(ScriptErrorDefinition());
}

std::vector<LuaRuntimeManager::RuntimePtr> LuaRuntimeManager::SnapshotRuntimes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RuntimePtr> out;
  out.reserve(runtimes_.size());
  for (const auto& [name, runtime] : runtimes_) out.push_back(runtime);
  return out;
}

// static
bool LuaRuntimeManager::IsActive(const RuntimePtr& runtime) {
  Phase phase = runtime->phase.load();
  return phase == Phase::kStarting || phase == Phase::kActive || phase == Phase::kStopping;
}

// static
std::string LuaRuntimeManager::StateName(const RuntimePtr& runtime) {
  switch (runtime->phase.load()) {
    case Phase::kStarting:
      return "STARTING";
    case Phase::kStopping:
      return "STOPPING";
    case Phase::kActive:
      // The engine is the authority once the script is up: it knows whether
      // the VM is still alive and whether it faulted.
      return runtime->engine->RuntimeInfo().state;
    case Phase::kTerminal:
      break;
  }
  LuaRuntimeInfo info = runtime->engine->RuntimeInfo();
  return info.last_error.empty() ? "STOPPED" : "ERROR";
}

LuaRuntimeManager::StartResult LuaRuntimeManager::StartScript(const std::string& scriptName,
                                                               std::string& error) {
  if (scriptName.empty()) {
    error = "no script name given";
    return StartResult::kFailed;
  }
  // Joined through filesystem::path rather than by string concatenation, so
  // the separators the UI reports are consistent instead of the
  // "C:\...\scripts/test/a.lua" mix a raw "+ /" produces on Windows.
  std::filesystem::path path = std::filesystem::path(ConfigManager::Instance().ScriptsDir()) / scriptName;
  return StartInternal(scriptName, path.make_preferred().string(), "", false, error);
}

LuaRuntimeManager::StartResult LuaRuntimeManager::StartFile(const std::string& fullPath, std::string& error) {
  if (fullPath.empty()) {
    error = "no script path given";
    return StartResult::kFailed;
  }

  // Keyed by the same relative name the script manager and the editor use, so
  // starting "card/issue.lua" from the UI and starting it as the configured
  // startup script are recognised as the same script -- otherwise the
  // duplicate-start guard in section 27 would let both run.
  std::error_code ec;
  std::filesystem::path rel =
      std::filesystem::relative(fullPath, ConfigManager::Instance().ScriptsDir(), ec);
  // A relative path that climbs out ("../elsewhere/x.lua") means the script
  // lives outside the scripts directory, so it has no name the script manager
  // would recognise -- fall back to the bare filename rather than showing a
  // path with ".." in it.
  std::string relText = rel.generic_string();
  std::string name = (ec || relText.empty() || relText.rfind("..", 0) == 0)
                         ? std::filesystem::path(fullPath).filename().string()
                         : relText;
  return StartInternal(name, fullPath, "", false, error);
}

LuaRuntimeManager::StartResult LuaRuntimeManager::StartSource(const std::string& code, std::string& error) {
  return StartInternal(kEditorBufferName, "", code, true, error);
}

LuaRuntimeManager::StartResult LuaRuntimeManager::StartPackage(
    const std::string& displayName, const std::vector<std::pair<std::string, std::string>>& modules,
    const std::string& entryBytecode, std::string& error) {
  if (entryBytecode.empty()) {
    error = "the package has no entry chunk";
    return StartResult::kFailed;
  }
  // PackageManager historically named modules relative to the scripts
  // directory (for example, "smartlocker.core.access"), while applications
  // conventionally require modules relative to their own application root
  // ("core.access"). A package has no filesystem fallback, so the latter
  // spelling otherwise fails with "no file ''" even though the module is in
  // the bundle. Keep the original names and add application-root aliases.
  std::vector<std::pair<std::string, std::string>> preload = modules;
  for (const auto& module : modules) {
    const std::string::size_type separator = module.first.find('.');
    if (separator == std::string::npos || separator + 1 >= module.first.size()) continue;

    const std::string alias = module.first.substr(separator + 1);
    const bool alreadyPresent = std::any_of(
        preload.begin(), preload.end(), [&alias](const auto& candidate) { return candidate.first == alias; });
    if (!alreadyPresent) preload.emplace_back(alias, module.second);
  }

  // fromSource, because there is no file: the bytecode is already in memory
  // and must stay there (section 2.7).
  return StartInternal(displayName, "", entryBytecode, true, error, preload);
}

LuaRuntimeManager::StartResult LuaRuntimeManager::StartInternal(const std::string& name,
                                                                 const std::string& path,
                                                                 const std::string& source, bool fromSource,
                                                                 std::string& error,
                                                                 const PreloadModules& preload) {
  if (!fromSource) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
      error = "script not found: " + path;
      return StartResult::kFailed;
    }
  }

  // Section 27: an already-running script must not get a second instance.
  RuntimePtr existing;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(name);
    if (it != runtimes_.end()) {
      if (IsActive(it->second)) {
        error = name + " is already running";
        return StartResult::kAlreadyRunning;
      }
      existing = it->second;
    }
  }

  // A previous run of this script that has since stopped or failed. Its thread
  // may have exited on its own without anybody joining it, and destroying a
  // joinable std::thread terminates the process -- so reap before replacing.
  if (existing) {
    Reap(existing, false);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(name);
    if (it != runtimes_.end() && it->second == existing) runtimes_.erase(it);
  }

  auto runtime = std::make_shared<Runtime>();
  runtime->id = nextId_.fetch_add(1);
  runtime->name = name;
  runtime->path = fromSource ? "" : path;
  runtime->engine = std::make_unique<LuaEngine>();
  runtime->engine->SetRuntimeId(runtime->id);
  runtime->engine->Bind(services_);

  // Thread first, map second: until it is in the map nothing else can see the
  // runtime, so there is no window where a concurrent Stop() could move the
  // thread out from under this assignment and leave it unjoined.
  runtime->thread = std::thread([this, runtime, path, source, fromSource, preload] {
    RunThread(runtime, path, source, fromSource, preload);
  });

  {
    std::lock_guard<std::mutex> lock(mutex_);
    runtimes_[name] = runtime;
  }

  // Wait for the top-level code to settle, so a syntax error is reported to
  // whoever pressed Run instead of only appearing in the log.
  bool decided = false;
  {
    std::unique_lock<std::mutex> lock(runtime->startMutex);
    decided = runtime->startCv.wait_for(lock, std::chrono::milliseconds(kStartDecisionTimeoutMs),
                                        [&runtime] { return runtime->startDecided; });
    if (decided && !runtime->startOk) {
      error = runtime->startError;
    }
  }

  if (decided && !runtime->startOk) return StartResult::kFailed;
  return StartResult::kStarted;
}

void LuaRuntimeManager::RunThread(RuntimePtr runtime, std::string path, std::string source, bool fromSource,
                                   PreloadModules preload) {
  runtime->phase.store(Phase::kActive);

  std::string error;
  bool ok;
  if (!preload.empty()) {
    ok = runtime->engine->RunBundle(preload, source, runtime->name, error);
  } else {
    ok = fromSource ? runtime->engine->RunSource(source, error) : runtime->engine->RunFile(path, error);
  }

  {
    std::lock_guard<std::mutex> lock(runtime->startMutex);
    runtime->startDecided = true;
    runtime->startOk = ok;
    runtime->startError = error;
  }
  runtime->startCv.notify_all();

  if (ok) {
    // The top-level code returned. Handler-style scripts stay live from here
    // on queued events, and polling-style scripts never reach this point until
    // they are stopped -- either way the engine, not this thread, is what
    // "running" means from now on, so the thread's job is done.
    Logger::Instance().Info(LogCategory::Lua, "Lua runtime " + std::to_string(runtime->id) + " (" +
                                                  runtime->name + ") is up");
    return;
  }

  // Section 22: capture the error, stop just this script, leave the others
  // alone. A stop we asked for is not a fault -- the interrupt that unsticks a
  // looping script surfaces as a Lua error, and reporting that as ERROR would
  // mark every deliberate stop as a failure.
  if (runtime->stopRequested.load()) {
    runtime->phase.store(Phase::kTerminal);
    return;
  }

  RecordScriptError(runtime, error);
  runtime->phase.store(Phase::kTerminal);

  // The failed script's dashboard I/O must not linger, but the OTHER scripts'
  // points have to survive -- hence per-owner rather than a global clear.
  ModbusRegistry::Instance().ClearOwner(runtime->id);
}

void LuaRuntimeManager::RecordScriptError(const RuntimePtr& runtime, const std::string& error) {
  LuaRuntimeInfo info = runtime->engine->RuntimeInfo();
  std::string errorType = ClassifyError(error);

  Logger::Instance().Error(LogCategory::Lua, "Lua script " + runtime->name + " failed" +
                                                  (info.error_line > 0
                                                       ? " at line " + std::to_string(info.error_line)
                                                       : std::string()) +
                                                  ": " + error);

  // Section 23's field list, so the failure is searchable on the Logs page and
  // not only visible in the runtime tail that scrolls away.
  std::string writeError;
  LogStore::Instance().Write("script_error", runtime->name, "ERROR",
                              {{"script", runtime->name},
                               {"path", runtime->path},
                               {"error", error},
                               {"error_type", errorType},
                               {"line", std::to_string(info.error_line)},
                               {"runtime_id", std::to_string(runtime->id)}},
                              writeError);
}

void LuaRuntimeManager::Reap(const RuntimePtr& runtime, bool interrupt) {
  // One reaper at a time: joining a thread twice is undefined, and both a
  // user-pressed Stop and a replacing Start can arrive at once.
  if (runtime->reaping.exchange(true)) return;

  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    thread = std::move(runtime->thread);
  }

  if (interrupt) {
    runtime->stopRequested.store(true);
    runtime->phase.store(Phase::kStopping);
    // Interrupts a script with no yield point and closes the Lua state, which
    // is what lets the thread below actually finish. Called with mutex_
    // released: a polling-loop script holds its engine's lock for its entire
    // run, so this can take up to the interrupt latency and must not hold up
    // status reporting or another script's start.
    runtime->engine->Stop();
  }

  if (thread.joinable()) thread.join();

  ModbusRegistry::Instance().ClearOwner(runtime->id);
  runtime->phase.store(Phase::kTerminal);
  runtime->reaping.store(false);
}

bool LuaRuntimeManager::Stop(const std::string& name) {
  RuntimePtr runtime;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(name);
    if (it == runtimes_.end()) return false;
    if (!IsActive(it->second)) return false;  // already stopped or failed
    runtime = it->second;
  }

  Reap(runtime, true);
  Logger::Instance().Info(LogCategory::Lua, "Lua script stopped: " + name);
  return true;
}

void LuaRuntimeManager::StopAll() {
  for (const auto& runtime : SnapshotRuntimes()) {
    if (IsActive(runtime)) Reap(runtime, true);
  }
}

bool LuaRuntimeManager::Restart(const std::string& name, std::string& error) {
  RuntimePtr runtime;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(name);
    if (it == runtimes_.end()) {
      error = "no runtime named " + name;
      return false;
    }
    runtime = it->second;
  }

  const std::string path = runtime->path;
  // An editor buffer has no file; its source is whatever the engine last ran.
  const std::string source = path.empty() ? runtime->engine->LastSource() : std::string();
  if (path.empty() && source.empty()) {
    error = "nothing to restart for " + name;
    return false;
  }

  if (IsActive(runtime)) Reap(runtime, true);

  StartResult result = path.empty() ? StartInternal(name, "", source, true, error)
                                     : StartInternal(name, path, "", false, error);
  return result == StartResult::kStarted;
}

int LuaRuntimeManager::Prune() {
  std::vector<RuntimePtr> dead;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = runtimes_.begin(); it != runtimes_.end();) {
      if (IsActive(it->second)) {
        ++it;
        continue;
      }
      dead.push_back(it->second);
      it = runtimes_.erase(it);
    }
  }

  // Reaped after removal from the map, and with the lock released: these
  // threads have already exited, but they were never joined, and destroying a
  // joinable std::thread terminates the process.
  for (const auto& runtime : dead) Reap(runtime, false);
  return static_cast<int>(dead.size());
}

bool LuaRuntimeManager::IsRunning(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = runtimes_.find(name);
  return it != runtimes_.end() && IsActive(it->second) && it->second->engine->IsRunning();
}

bool LuaRuntimeManager::AnyRunning() const { return RunningCount() > 0; }

int LuaRuntimeManager::RunningCount() const {
  int count = 0;
  for (const auto& runtime : SnapshotRuntimes()) {
    if (IsActive(runtime) && runtime->engine->IsRunning()) ++count;
  }
  return count;
}

std::vector<LuaRuntimeStatus> LuaRuntimeManager::Statuses() const {
  std::vector<LuaRuntimeStatus> out;
  for (const auto& runtime : SnapshotRuntimes()) {
    LuaRuntimeInfo info = runtime->engine->RuntimeInfo();
    LuaRuntimeStatus status;
    status.runtime_id = runtime->id;
    status.name = runtime->name;
    status.path = runtime->path;
    status.state = StateName(runtime);
    status.start_time = info.start_time;
    status.uptime_seconds = info.uptime_seconds;
    status.cpu_percent = info.cpu_percent;
    status.memory_kb = info.memory_kb;
    status.cpu_core = info.cpu_core;
    status.last_error = info.last_error;
    status.error_line = info.error_line;
    out.push_back(std::move(status));
  }
  // By runtime id, so the System Info table keeps a stable order (the map is
  // keyed by name, which would reshuffle rows as scripts come and go).
  std::sort(out.begin(), out.end(),
            [](const LuaRuntimeStatus& a, const LuaRuntimeStatus& b) { return a.runtime_id < b.runtime_id; });
  return out;
}

void LuaRuntimeManager::DispatchEventAll(const std::string& functionName, const std::string& arg) {
  for (const auto& runtime : SnapshotRuntimes()) {
    if (runtime->engine->IsRunning()) runtime->engine->DispatchEvent(functionName, arg);
  }
}

void LuaRuntimeManager::DispatchEventAll(const std::string& functionName, long long argA, double argB) {
  for (const auto& runtime : SnapshotRuntimes()) {
    if (runtime->engine->IsRunning()) runtime->engine->DispatchEvent(functionName, argA, argB);
  }
}

void LuaRuntimeManager::QueueEventAll(const std::string& functionName, const std::string& arg) {
  for (const auto& runtime : SnapshotRuntimes()) {
    if (runtime->engine->IsRunning()) runtime->engine->QueueEvent(functionName, arg);
  }
}

bool LuaRuntimeManager::DispatchHttp(const std::shared_ptr<LuaHttpRequest>& request, int timeoutMs) {
  if (!request) return false;

  // SnapshotRuntimes() walks the map, which std::map keeps in name order --
  // so with two scripts claiming the same route, the same one wins every time
  // rather than it depending on start order.
  for (const auto& runtime : SnapshotRuntimes()) {
    if (!runtime->engine->IsRunning()) continue;
    if (!runtime->engine->HandlesHttp(request->method, request->path)) continue;
    if (!runtime->engine->SubmitHttp(request)) continue;

    std::unique_lock<std::mutex> lock(request->mutex);
    if (request->done.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [&request] { return request->completed; })) {
      return true;
    }

    // The script has the request but never got round to it: it is stuck, or
    // busy in a long blocking call. Answered as a gateway timeout rather than
    // held open -- and the queued entry is left in place, since the script may
    // still complete into it safely (the shared_ptr keeps it alive).
    request->status = 504;
    request->error = "the script serving " + request->method + " " + request->path +
                      " did not answer within " + std::to_string(timeoutMs) + " ms";
    return true;
  }

  return false;
}

std::vector<std::string> LuaRuntimeManager::HttpRoutes() const {
  std::vector<std::string> out;

  for (const auto& runtime : SnapshotRuntimes()) {
    if (!runtime->engine->IsRunning()) continue;
    for (const std::string& route : runtime->engine->HttpRoutes()) {
      if (std::find(out.begin(), out.end(), route) == out.end()) out.push_back(route);
    }
  }

  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace hsf
