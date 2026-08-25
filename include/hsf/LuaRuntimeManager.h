#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hsf/LuaEngine.h"

namespace hsf {

class RestClient;
class SerialPort;
class ModbusClient;
class RfidClient;
class MqClient;
class PluginManager;
class ZkController;

// Reported state of one managed runtime -- LuaEngine's own metrics plus the
// lifecycle phase only the manager knows about (request/upgrade.md section 21:
// STOPPED / STARTING / RUNNING / STOPPING / ERROR).
struct LuaRuntimeStatus {
  int64_t runtime_id = 0;
  std::string name;   // script path relative to the scripts dir, or "(editor buffer)"
  std::string path;   // absolute path; empty for an editor buffer
  std::string state;
  int64_t start_time = 0;
  double uptime_seconds = 0.0;
  double cpu_percent = 0.0;
  double memory_kb = 0.0;
  int cpu_core = -1;
  std::string last_error;
  int error_line = 0;
};

// Runs several Lua scripts at once, each in its own LuaEngine (its own
// lua_State, its own mutex, its own metrics) on its own thread -- what
// request/upgrade.md sections 20-28 ask for. Replaces the single LuaEngine the
// gateway used to hold: one script at a time, executed on whichever HTTP
// thread happened to ask for it.
//
// What the manager adds on top of a bare engine:
//
//   - a dedicated thread per script, so a polling-loop script no longer
//     occupies a web-server worker for its entire run;
//   - duplicate-start protection, keyed by script name (section 27);
//   - error isolation: a script that faults is marked ERROR and reaped on its
//     own, and the others keep running (section 22);
//   - fan-out of hardware events to every live runtime. SerialPort and
//     ZkController each hold ONE callback, so those slots belong here rather
//     than to whichever engine was constructed last;
//   - per-runtime Modbus registrations, cleared only for the script that
//     owned them.
//
// Thread-safety: `mutex_` guards the runtime map only. Anything that can block
// for a long time -- interrupting a script, joining its thread, dispatching
// into a VM -- happens with the lock released, so a stuck script can never
// freeze status reporting or another script's start.
class LuaRuntimeManager {
 public:
  LuaRuntimeManager();
  ~LuaRuntimeManager();

  LuaRuntimeManager(const LuaRuntimeManager&) = delete;
  LuaRuntimeManager& operator=(const LuaRuntimeManager&) = delete;

  // The name under which code run from the web editor (no file behind it)
  // appears in the runtime list.
  static const char* EditorBufferName();

  // Native modules every runtime is bound to, plus the device callbacks the
  // manager fans out. Call once before starting anything.
  void Bind(RestClient* rest, SerialPort* serial, SerialPort* serial2, ModbusClient* modbus, RfidClient* rfid,
            ZkController* zk, MqClient* mq, PluginManager* plugins = nullptr);

  enum class StartResult {
    kStarted,
    kAlreadyRunning,  // section 27 -- pressing Run twice must not start a second instance
    kFailed
  };

  // Starts `scriptName` (a path relative to the scripts directory) on its own
  // thread. Returns once the script's top-level code has either finished, or
  // failed, or been running long enough to be considered started -- so a
  // syntax error still comes back to the caller inline, while an intentional
  // `while true do ... end` script doesn't block the HTTP request that
  // launched it.
  StartResult StartScript(const std::string& scriptName, std::string& error);

  // Same, for an absolute path. Used for the configured startup script, which
  // need not live under the scripts directory.
  StartResult StartFile(const std::string& fullPath, std::string& error);

  // Runs source with no file behind it (the editor's Run button), replacing
  // any previous editor-buffer runtime.
  StartResult StartSource(const std::string& code, std::string& error);

  // Starts a decrypted production package (request/AdvanceUpdate.md Phase 2).
  // `displayName` is what the dashboard and the structured logs call it, e.g.
  // "smartlocker-1.4.0.pkg (package)"; there is no file behind it, and
  // deliberately no path -- see LuaEngine::RunBundle.
  StartResult StartPackage(const std::string& displayName,
                           const std::vector<std::pair<std::string, std::string>>& modules,
                           const std::string& entryBytecode, std::string& error);

  // Interrupts the script, joins its thread and releases its resources
  // (section 28). The runtime stays in the list in its terminal state so the
  // UI can show what stopped; Prune() removes those entries. False when there
  // is no such runtime or it wasn't running.
  bool Stop(const std::string& name);

  // Stops every active runtime. Used on shutdown.
  void StopAll();

  // Stop-then-start, keeping the same script. False (with `error`) when there
  // is no runtime by that name to restart.
  bool Restart(const std::string& name, std::string& error);

  // Drops runtimes that are no longer active, so the list stops showing
  // scripts that already stopped or failed. Returns how many were removed.
  int Prune();

  bool IsRunning(const std::string& name) const;
  // True when at least one runtime is active -- what the dashboard's single
  // "Lua" status light reflects.
  bool AnyRunning() const;
  int RunningCount() const;

  std::vector<LuaRuntimeStatus> Statuses() const;

  // --- event fan-out ------------------------------------------------------
  //
  // Delivered to every runtime that is currently running. A handler missing
  // from a given script is simply not called there, which is what lets a
  // gateway split "handle cards" and "drive the LED display" into separate
  // scripts.
  void DispatchEventAll(const std::string& functionName, const std::string& arg);
  void DispatchEventAll(const std::string& functionName, long long argA, double argB);
  void QueueEventAll(const std::string& functionName, const std::string& arg);

  // --- Lua-served HTTP routes ---------------------------------------------
  //
  // Finds the running script that registered (method, path), hands it the
  // request, and waits up to `timeoutMs` for its handler to answer. The wait is
  // what makes this usable from a Crow worker: the handler runs on the script's
  // own thread at its next Sleep(), so a 100 ms loop answers in well under a
  // tick. Returns false when no script serves that route (the caller then
  // answers 404), or when the wait ran out -- request->status carries 504 in
  // that case.
  //
  // With two scripts registering the same route the first match in name order
  // wins, deterministically rather than by whichever started last.
  bool DispatchHttp(const std::shared_ptr<LuaHttpRequest>& request, int timeoutMs);

  // Every route every running script serves, as "METHOD path" strings. For the
  // gateway's own API index and for diagnosing a 404.
  std::vector<std::string> HttpRoutes() const;

  // Compile-only check for the editor; needs no runtime.
  static bool Validate(const std::string& code, std::string& error) {
    return LuaEngine::Validate(code, error);
  }

 private:
  enum class Phase { kStarting, kActive, kStopping, kTerminal };

  struct Runtime {
    int64_t id = 0;
    std::string name;
    std::string path;
    std::unique_ptr<LuaEngine> engine;
    std::thread thread;
    std::atomic<Phase> phase{Phase::kStarting};
    // Set before interrupting, so the thread can tell a requested stop from a
    // genuine script fault -- the interrupt itself surfaces as a Lua error.
    std::atomic<bool> stopRequested{false};
    // Whoever flips this false->true owns joining the thread; a second
    // concurrent Stop() for the same name backs off instead of joining twice.
    std::atomic<bool> reaping{false};

    // Signals that the script's top-level code has been decided (returned,
    // or failed), so StartScript can report a compile/startup error inline.
    std::mutex startMutex;
    std::condition_variable startCv;
    bool startDecided = false;
    bool startOk = false;
    std::string startError;
  };

  using RuntimePtr = std::shared_ptr<Runtime>;

  // `preload` non-empty means `source` is a compiled entry chunk and those are
  // its modules -- the runtime then goes through LuaEngine::RunBundle instead
  // of RunSource. Threaded through rather than stashed on the engine so that
  // starting a package and starting a script stay one code path, with one
  // place that decides a runtime is "up".
  using PreloadModules = std::vector<std::pair<std::string, std::string>>;

  StartResult StartInternal(const std::string& name, const std::string& path, const std::string& source,
                             bool fromSource, std::string& error, const PreloadModules& preload = {});
  void RunThread(RuntimePtr runtime, std::string path, std::string source, bool fromSource,
                  PreloadModules preload);

  // Joins the runtime's thread (interrupting the script first when asked) and
  // clears the Modbus points it registered. Safe to call on a runtime whose
  // thread already exited.
  void Reap(const RuntimePtr& runtime, bool interrupt);

  // Records a script fault everywhere section 23 asks for it: the runtime log,
  // the structured log store, and the runtime's own last_error (which is what
  // the WebSocket message and the editor's error panel show).
  void RecordScriptError(const RuntimePtr& runtime, const std::string& error);

  std::vector<RuntimePtr> SnapshotRuntimes() const;
  static bool IsActive(const RuntimePtr& runtime);
  static std::string StateName(const RuntimePtr& runtime);

  mutable std::mutex mutex_;
  std::map<std::string, RuntimePtr> runtimes_;
  std::atomic<int64_t> nextId_{1};

  RestClient* rest_ = nullptr;
  SerialPort* serial_ = nullptr;
  SerialPort* serial2_ = nullptr;
  ModbusClient* modbus_ = nullptr;
  RfidClient* rfid_ = nullptr;
  ZkController* zk_ = nullptr;
  MqClient* mq_ = nullptr;
  PluginManager* plugins_ = nullptr;
};

}  // namespace hsf
