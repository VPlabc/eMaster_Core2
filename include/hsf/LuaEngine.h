#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <utility>

#include "hsf/TcpSocket.h"

struct lua_State;
struct lua_Debug;

namespace hsf {

class RestClient;
class SerialPort;
class ModbusClient;
class RfidClient;
class MqClient;
class PluginManager;
class SqlDatabase;
class ZkController;
struct ZkCardEvent;

// One HTTP request handed to a script that registered a route with
// Http.Register, and the response it produced. Owned by a shared_ptr so the web
// thread can keep waiting on it after the engine has taken its copy, and so a
// request abandoned at timeout is still safe for the script to complete into.
struct LuaHttpRequest {
  std::string method;
  std::string path;                             // path below the Lua route prefix, no leading '/'
  std::string body;
  std::string content_type;
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> headers;

  // --- filled in by the script's handler ---
  int status = 0;                               // 0 until answered
  std::string response_body;
  std::string response_content_type = "application/json";
  std::string error;

  // Signalled once by the engine when the handler has returned (or failed).
  // The web thread waits on this with a timeout: a script that never drains
  // its queue must not hold an HTTP worker thread forever.
  std::mutex mutex;
  std::condition_variable done;
  bool completed = false;
};

// Live facts about the running script for the dashboard's System Info panel
// (request/updateUI.md section 10). Every field is measured, never
// synthesised -- section 11 requires an empty panel when nothing is running
// rather than plausible-looking placeholders, so `running` false means the
// UI shows the empty state and ignores the rest.
struct LuaRuntimeInfo {
  // Identifies this VM among the several that can now run at once
  // (request/upgrade.md sections 20-21). Stable for the life of the runtime
  // and carried on the WebSocket messages section 25 defines.
  int64_t runtime_id = 0;
  bool running = false;
  std::string name;        // script path relative to the scripts dir; empty for an editor buffer
  std::string path;        // absolute path on disk; empty for an editor buffer
  std::string state;       // "RUNNING" | "STOPPED" | "ERROR"
  int64_t start_time = 0;  // unix seconds, 0 if never started
  double uptime_seconds = 0.0;
  double cpu_percent = 0.0;
  double memory_kb = 0.0;  // Lua heap, from the VM's own allocator accounting
  int cpu_core = -1;       // -1 when not sampled yet
  std::string last_error;
  // Parsed out of last_error where Lua provided one (its messages are
  // "chunk:LINE: text"), so section 23's error report can name the line
  // without the operator reading the raw message. 0 when unknown.
  int error_line = 0;
};

// Embedded Lua 5.4 engine that runs the gateway's business logic. A script's
// top-level code runs once (to set up runtime variables and define handler
// functions such as OnCitizenCardRead), after which the gateway dispatches
// events into those handler functions as hardware activity occurs. See
// config/scripts/main.lua for the convention and docs/ (web Lua API
// Documentation page) for the bound native API.
//
// ONE instance is ONE script: the VM, its state and its metrics all belong to
// a single running script, and several scripts run at once by having several
// engines (request/upgrade.md section 21 -- "each Lua script must have an
// independent runtime state"). LuaRuntimeManager owns that set and is what
// the rest of the gateway talks to; construct an engine directly only when a
// single anonymous VM is genuinely what's wanted.
class LuaEngine {
 public:
  LuaEngine();
  ~LuaEngine();

  // Identity for this VM, assigned by LuaRuntimeManager. Set before the first
  // Run*, since it is what tags this script's Modbus registrations (so
  // stopping one script can't wipe another's dashboard points) and what
  // stamps its structured log rows.
  void SetRuntimeId(int64_t id) { runtimeId_ = id; }
  int64_t RuntimeId() const { return runtimeId_; }

  // Script path relative to the scripts directory, or "" for an editor
  // buffer. Used as script_name on structured log entries.
  std::string RuntimeName() const;

  // Native modules the Lua API delegates to. Must be called before RunSource
  // / RunFile. Non-owning; the caller keeps these alive for the LuaEngine's
  // lifetime.
  // `serial2` is the second, independent port -- the TDM-800 LED display
  // (request/LEDBoard.md), exposed to Lua as Serial2.* and wrapped by
  // config/scripts/led.lua. Unlike `serial`, no data callback is attached:
  // the LED display is write-mostly, and nothing parses its replies as
  // citizen cards.
  //
  // Hardware callbacks (serial lines, RFID/ZK card reads) are deliberately
  // NOT installed here: each of those has a single callback slot on the
  // device, and with several engines running there is no one engine that
  // should own it. LuaRuntimeManager installs one fan-out callback and
  // delivers to every live engine -- see LuaRuntimeManager::Bind.
  // `mq` is the RabbitMQ client behind the Mq.* table. Unlike the serial/ZK
  // callbacks, its inbound-message callback is safe to share: MqClient keeps its
  // own inbox and LuaRuntimeManager fans the notification out, so several
  // scripts can consume side by side.
  void Bind(RestClient* rest, SerialPort* serial, SerialPort* serial2, ModbusClient* modbus, RfidClient* rfid,
             ZkController* zk, MqClient* mq, PluginManager* plugins = nullptr);

  // Compiles `code` without executing it; used for the editor's "Validate"
  // action. Returns true and leaves `error` empty on success. Static because
  // it builds a throwaway state and touches no engine of its own -- the
  // editor can validate while every runtime is busy.
  static bool Validate(const std::string& code, std::string& error);

  bool RunFile(const std::string& path, std::string& error);
  bool RunSource(const std::string& code, std::string& error);

  // Runs a compiled production bundle (request/AdvanceUpdate.md Phase 2).
  //
  // `modules` is (require-name, bytecode); each is installed into
  // package.preload before the entry chunk runs, so require() inside the
  // application finds its siblings in memory and never consults package.path.
  // That is what makes a deployed package self-contained AND what keeps the
  // decrypted code off the disk -- there is no file for require() to find,
  // because there is no file.
  //
  // luaL_loadbuffer accepts binary chunks, so this shares the whole
  // RunSource path below it; the only difference is the preload table.
  bool RunBundle(const std::vector<std::pair<std::string, std::string>>& modules,
                 const std::string& entryBytecode, const std::string& displayName, std::string& error);

  // Interrupts the running script if necessary (a debug count-hook checks
  // interruptRequested_ every 1000 VM instructions and aborts the script
  // with a Lua error if set — this is what lets Stop() actually break out
  // of a script with no I/O yield point, e.g. `while true do end`) and
  // closes the Lua state.
  void Stop();
  bool Restart(std::string& error);
  bool IsRunning() const { return running_.load(); }

  std::string LastSource() const;

  // Safe to call from any thread at any time, including while a script is
  // mid-run. Deliberately takes no lock on mutex_: a polling-loop script
  // holds that for its entire execution, so anything that waited on it could
  // never report on the very scripts most worth watching. The values come
  // from atomics the debug hook refreshes from inside the script's own
  // thread, plus thread CPU times read straight from the OS.
  LuaRuntimeInfo RuntimeInfo() const;

  // Calls a global Lua function by name with a single string argument,
  // ignoring any return value. No-op (logged) if the function doesn't
  // exist or the script isn't running. Used to fire e.g. OnCitizenCardRead.
  //
  // Only safe to call when the caller knows the running script's top-level
  // code has already returned (the "define handlers, then return" script
  // convention) — it blocks on mutex_, which a script that instead polls in
  // its own `while true do ... end` loop holds for its *entire run*. For
  // dispatching into a script that might be doing that (any hardware
  // callback firing from a background thread, basically), use QueueEvent
  // instead.
  void DispatchEvent(const std::string& functionName, const std::string& arg);

  // Calls a global Lua function by name with (integer, number) arguments,
  // e.g. OnPlcInputChanged(index, value). Same caveat as above.
  void DispatchEvent(const std::string& functionName, long long argA, double argB);

  // Queues a string-argument event for delivery whenever the running
  // script's own thread next calls Sleep() or Serial.Read() (both drain the
  // queue inline, since they already hold mutex_ on the correct thread —
  // no cross-thread lock acquisition needed, so this can't deadlock against
  // a polling-loop script the way DispatchEvent can). Safe to call from any
  // thread. If the script never calls either of those, queued events simply
  // never get delivered — this only serves the polling-loop convention.
  void QueueEvent(const std::string& functionName, const std::string& arg);

  // --- Lua-registered HTTP routes ------------------------------------------
  //
  // A script calls Http.Register(method, path, handler) and this engine records
  // the (method, path) pair. HandlesHttp() is what LuaRuntimeManager asks to
  // find the engine that owns an incoming request; SubmitHttp() hands the
  // request over and returns immediately -- the handler runs on the SCRIPT's
  // own thread, the next time it calls Sleep() (or any other draining
  // binding), because that is the only thread that may enter this VM. The
  // caller then waits on request->done.
  //
  // Why not call the handler directly from the web thread: a polling-loop
  // script holds mutex_ for its entire run, so a direct call would block a
  // Crow worker for as long as the script runs -- forever, in practice. The
  // queue turns "as long as the script runs" into "until its next Sleep",
  // which for the SmartLocker loop is under 100 ms.
  bool HandlesHttp(const std::string& method, const std::string& path) const;
  bool SubmitHttp(const std::shared_ptr<LuaHttpRequest>& request);
  std::vector<std::string> HttpRoutes() const;

  // ZK controller callbacks, queued for this VM. Public because with several
  // engines running, the single callback slot on ZkController belongs to
  // LuaRuntimeManager, which fans each event out to every live engine.
  //
  // MUST only be called from ZkController's RunLoop thread -- see the
  // threading note on TryDeliverZkPendingCallbacksNow below.
  void QueueZkCardEvent(const ZkCardEvent& event);
  void QueueZkRawEvent(const std::string& raw);
  void QueueZkConnectionEvent(bool connected);
  // Auxiliary input edge (event 220/221) for this VM's zk.onAuxInput handler.
  void QueueZkAuxInputEvent(int input, bool shorted);

 private:
  void DrainPendingEvents(lua_State* L);
  bool StartState(std::string& error);
  void CloseState();
  void RegisterBindings();
  void PushEnginePointer();
  static void DebugHook(lua_State* L, lua_Debug* ar);

  // zk_controller is a require()'d native module (see
  // request/HSF_Machine_ZK_Controller_Lua_Integration.md section 13 --
  // "require("zk_controller")" is the spec's explicit API shape), not a
  // global table like Rest/Serial/Card/etc. above -- registered into
  // package.preload so require() finds it without touching the filesystem.
  void RegisterZkControllerModule();
  void DrainZkPendingCallbacks(lua_State* L);

  // Unlike OnCitizenCardRead/OnRfidCardRead (which rely entirely on the
  // running script itself calling Sleep()/Serial.Read() to drain queued
  // events -- dead on arrival for a "handlers, then return" script with no
  // loop, exactly the style request/HSF_Machine_ZK_Controller_Lua_Integration.md's
  // own test script uses), zk_controller's callbacks also get an immediate
  // delivery attempt right after queueing. Non-blocking (try_lock): if a
  // polling-loop script currently holds mutex_, this just backs off and the
  // item stays queued for that script's own Sleep()/Serial.Read() to drain
  // later -- never blocks the ZK thread waiting on a busy Lua engine.
  //
  // MUST only be called from ZkController's RunLoop thread, which never
  // holds mutex_. try_lock on a std::mutex the calling thread already owns
  // is undefined behavior (deadlocks on MSVC), which is exactly what
  // happened when ZkController still delivered connection callbacks
  // synchronously on the caller's thread: a Lua script calling zk.connect()
  // held mutex_, and the resulting callback landed back here on that same
  // thread. ZkController now queues those for RunLoop instead -- see the
  // threading note in ZkController.h; don't reintroduce direct delivery.
  void TryDeliverZkPendingCallbacksNow();

  static int Lua_ZkController_Open(lua_State* L);
  static int Lua_ZkController_Connect(lua_State* L);
  static int Lua_ZkController_Disconnect(lua_State* L);
  static int Lua_ZkController_IsConnected(lua_State* L);
  static int Lua_ZkController_SetAutoReconnect(lua_State* L);
  static int Lua_ZkController_StartRTLog(lua_State* L);
  static int Lua_ZkController_StopRTLog(lua_State* L);
  static int Lua_ZkController_OnCard(lua_State* L);
  static int Lua_ZkController_OnRawRTLog(lua_State* L);
  static int Lua_ZkController_OnConnectionChanged(lua_State* L);
  static int Lua_ZkController_OnAuxInput(lua_State* L);
  static int Lua_ZkController_LastError(lua_State* L);

  // Relay / auxiliary output / device parameter control, per
  // sdk-protocol-reference.md section 3.5. The PullSDK has no beeper command,
  // so zk.beep() pulses whichever output the buzzer is wired to.
  static int Lua_ZkController_ControlDevice(lua_State* L);
  static int Lua_ZkController_OpenDoor(lua_State* L);
  static int Lua_ZkController_AuxOut(lua_State* L);
  static int Lua_ZkController_Pulse(lua_State* L);
  static int Lua_ZkController_Beep(lua_State* L);
  static int Lua_ZkController_CancelAlarm(lua_State* L);
  static int Lua_ZkController_RestartDevice(lua_State* L);
  static int Lua_ZkController_SetNormallyOpen(lua_State* L);
  static int Lua_ZkController_GetParam(lua_State* L);
  static int Lua_ZkController_SetParam(lua_State* L);
  static int Lua_ZkController_IoState(lua_State* L);
  static int Lua_ZkController_DoorState(lua_State* L);
  static int Lua_ZkController_InputState(lua_State* L);

  static int Lua_Rest_SetUrl(lua_State* L);
  static int Lua_Rest_SetApiKey(lua_State* L);
  static int Lua_Rest_PostForm(lua_State* L);
  static int Lua_Rest_Get(lua_State* L);

  static int Lua_Serial_Open(lua_State* L);
  static int Lua_Serial_Close(lua_State* L);
  static int Lua_Serial_Read(lua_State* L);
  static int Lua_Serial_Write(lua_State* L);
  static int Lua_Serial_IsOpen(lua_State* L);
  static int Lua_Serial_Status(lua_State* L);
  // Actually closes the hardware, unlike Serial.Close() which only releases the
  // script's claim on it -- see docs/FixSerial.md §10.
  static int Lua_Serial_Shutdown(lua_State* L);

  static int Lua_Serial2_Open(lua_State* L);
  static int Lua_Serial2_Close(lua_State* L);
  static int Lua_Serial2_Read(lua_State* L);
  static int Lua_Serial2_Write(lua_State* L);
  static int Lua_Serial2_IsOpen(lua_State* L);
  static int Lua_Serial2_Status(lua_State* L);
  static int Lua_Serial2_Shutdown(lua_State* L);

  static int Lua_Tcp_Connect(lua_State* L);
  static int Lua_Tcp_Send(lua_State* L);
  static int Lua_Tcp_Receive(lua_State* L);
  static int Lua_Tcp_Close(lua_State* L);

  static int Lua_Rfid_IsConnected(lua_State* L);

  static int Lua_Modbus_IsConnected(lua_State* L);
  static int Lua_Modbus_CheckConnect(lua_State* L);
  static int Lua_Modbus_ReadCoil(lua_State* L);
  static int Lua_Modbus_WriteCoil(lua_State* L);
  static int Lua_Modbus_ReadCoils(lua_State* L);
  static int Lua_Modbus_WriteCoils(lua_State* L);
  static int Lua_Modbus_ReadHolding(lua_State* L);
  static int Lua_Modbus_WriteHolding(lua_State* L);
  static int Lua_Modbus_ReadHoldingRegisters(lua_State* L);
  static int Lua_Modbus_WriteHoldingRegisters(lua_State* L);
  static int Lua_Modbus_RegisterInput(lua_State* L);
  static int Lua_Modbus_RegisterOutput(lua_State* L);
  static int Lua_Modbus_RegisterDiscreteInput(lua_State* L);
  static int Lua_Modbus_RegisterCoilInput(lua_State* L);
  static int Lua_Modbus_ReadDiscreteInput(lua_State* L);
  static int Lua_Modbus_ReadDiscreteInputs(lua_State* L);
  static int Lua_Modbus_ReadInputRegister(lua_State* L);
  static int Lua_Modbus_ReadInputRegisters(lua_State* L);
  static int Lua_Modbus_RegisterRegister(lua_State* L);
  static int Lua_Modbus_GetRegister(lua_State* L);
  static int Lua_Modbus_SetRegister(lua_State* L);

  static int Lua_Plugin_List(lua_State* L);
  static int Lua_Plugin_Health(lua_State* L);
  static int Lua_Plugin_Read(lua_State* L);
  static int Lua_Plugin_Write(lua_State* L);

  static int Lua_Card_Get(lua_State* L);
  static int Lua_Card_Clear(lua_State* L);
  static int Lua_Card_Available(lua_State* L);

  static int Lua_Mq_IsConnected(lua_State* L);
  static int Lua_Mq_Status(lua_State* L);
  static int Lua_Mq_Publish(lua_State* L);
  static int Lua_Mq_PublishRaw(lua_State* L);
  static int Lua_Mq_Flush(lua_State* L);
  static int Lua_Mq_Available(lua_State* L);
  static int Lua_Mq_Get(lua_State* L);
  static int Lua_Mq_Clear(lua_State* L);
  static int Lua_Mq_Pause(lua_State* L);

  static int Lua_SetVariable(lua_State* L);
  static int Lua_GetVariable(lua_State* L);
  static int Lua_Sleep(lua_State* L);

  static int Lua_Log_Info(lua_State* L);
  static int Lua_Log_Warning(lua_State* L);
  static int Lua_Log_Error(lua_State* L);
  static int Lua_Log_Debug(lua_State* L);
  // Structured business log into SQLite (request/upgrade.md section 10),
  // as opposed to the four runtime levels above.
  static int Lua_Log_Write(lua_State* L);
  static int Lua_Log_Types(lua_State* L);

  static int Lua_Config_Get(lua_State* L);
  static int Lua_Config_Set(lua_State* L);
  static int Lua_Config_Exists(lua_State* L);
  static int Lua_Config_GetCategory(lua_State* L);

  // SQL, over this engine's own SqlDatabase connection. Deliberately a real
  // SQL surface, unlike Config.* -- request/SmartLocker/SmartLockerPlan.md
  // section 15 asks for the application's tables in SQLite, and a typed
  // key/value API cannot express a join or a transaction.
  // Returns this engine's open connection, or raises a Lua error naming the
  // binding that needed it. `what` is that name, for the message.
  static SqlDatabase* DbHandle(lua_State* L, LuaEngine* engine, const char* what);

  // The ZkController behind every zk.* binding; null when this build has no
  // PullSDK or the engine was constructed without one.
  static ZkController* ZkForBinding(lua_State* L);

  static int Lua_Db_Open(lua_State* L);
  static int Lua_Db_Close(lua_State* L);
  static int Lua_Db_IsOpen(lua_State* L);
  static int Lua_Db_Path(lua_State* L);
  static int Lua_Db_Exec(lua_State* L);
  static int Lua_Db_Query(lua_State* L);
  static int Lua_Db_QueryOne(lua_State* L);
  static int Lua_Db_Scalar(lua_State* L);
  static int Lua_Db_Begin(lua_State* L);
  static int Lua_Db_Commit(lua_State* L);
  static int Lua_Db_Rollback(lua_State* L);
  static int Lua_Db_InTransaction(lua_State* L);
  static int Lua_Db_Quote(lua_State* L);

  static int Lua_Http_Register(lua_State* L);
  static int Lua_Http_Unregister(lua_State* L);
  static int Lua_Http_Routes(lua_State* L);

  // Refreshed by DebugHook from inside the script's own thread, so
  // RuntimeInfo() can report on a script that is holding mutex_.
  void SampleRuntimeMetrics(lua_State* L);
  void BeginRuntimeTracking(const std::string& name, const std::string& path);
  void EndRuntimeTracking(const std::string& state, const std::string& error);
  void ReleaseCpuHandle();

  mutable std::mutex mutex_;
  lua_State* L_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> interruptRequested_{false};
  std::string lastSource_;
  std::atomic<int64_t> runtimeId_{0};

  // --- runtime metrics (all atomic / separately guarded, never under mutex_) ---
  mutable std::mutex runtimeMutex_;  // guards the three strings only
  std::string runtimeName_;
  std::string runtimePath_;
  std::string runtimeError_;
  std::atomic<int64_t> runtimeStart_{0};
  std::atomic<double> runtimeMemoryKb_{0.0};
  std::atomic<int> runtimeCore_{-1};
  // OS handle for the thread executing the script, so CPU time can be read
  // without touching the Lua state. Owned: closed in EndRuntimeTracking.
  std::atomic<void*> runtimeThread_{nullptr};
  // Mutable because RuntimeInfo() is const but has to advance the CPU
  // sampling window: a rate needs two samples, and the observer is the only
  // thing positioned to take the second one. This is sampling bookkeeping,
  // not observable engine state.
  mutable std::atomic<double> runtimeCpuPercent_{0.0};
  mutable std::atomic<uint64_t> runtimeLastCpu100ns_{0};
  mutable std::atomic<int64_t> runtimeLastSampleMs_{0};
  std::atomic<uint64_t> hookTicks_{0};
  // Directory of the last file run via RunFile(), so require()'d sibling
  // modules resolve regardless of the process's current working directory
  // (see StartState's package.path setup). Empty when the running script
  // came from RunSource() directly (e.g. the web editor) rather than a file.
  std::string scriptDir_;
  // Set by RunFile just before it calls RunSource, consumed there. Empty
  // means the code came from the editor with no file behind it, which the
  // System Info panel labels as such rather than inventing a filename.
  std::string pendingRunName_;
  std::string pendingRunPath_;
  // Set by RunBundle just before it calls RunSource, consumed there: the
  // application's modules as (require-name, bytecode), installed into
  // package.preload once the fresh lua_State exists. Cleared after use, so an
  // ordinary editor Run that follows a package Run does not inherit the
  // package's modules.
  std::vector<std::pair<std::string, std::string>> pendingPreload_;

  RestClient* rest_ = nullptr;
  SerialPort* serial_ = nullptr;
  SerialPort* serial2_ = nullptr;
  ModbusClient* modbus_ = nullptr;
  RfidClient* rfid_ = nullptr;
  ZkController* zk_ = nullptr;
  MqClient* mq_ = nullptr;
  PluginManager* plugins_ = nullptr;
  TcpSocket luaTcp_;

  // One connection per engine (one script), created on the first Db.Open().
  // Owned rather than shared: two scripts writing the same file get two
  // connections and SQLite arbitrates between them with WAL, which is what it
  // is for -- sharing one handle would serialise them behind this engine's
  // mutex instead and make one script's long transaction stall the other's
  // reads. Closed with the state, so a restarted script starts clean.
  std::unique_ptr<SqlDatabase> db_;

  // Routes this script registered, keyed "METHOD /path". The Lua handler is
  // kept as a registry ref (same technique as the zk callbacks) since
  // Http.Register takes a closure, not a named global.
  struct HttpRoute {
    std::string method;
    std::string path;
    int handlerRef = -2;  // LUA_NOREF
  };
  mutable std::mutex httpMutex_;
  std::map<std::string, HttpRoute> httpRoutes_;
  std::queue<std::shared_ptr<LuaHttpRequest>> httpPending_;
  void DrainPendingHttp(lua_State* L);
  static std::string HttpRouteKey(const std::string& method, const std::string& path);
  // Route serving (method, path), honouring <segment> wildcards and the ANY
  // method. Empty when nothing matches. Caller holds httpMutex_.
  std::string MatchHttpRouteUnlocked(const std::string& method, const std::string& path) const;

  struct PendingEvent {
    std::string functionName;
    std::string arg;
  };
  std::mutex pendingEventsMutex_;
  std::queue<PendingEvent> pendingEvents_;

  // zk.onCard()/onRawRTLog()/onConnectionChanged() register arbitrary Lua
  // closures (not named globals like the OnXxx convention above), so they're
  // kept as LUA_REGISTRYINDEX refs rather than looked up by name. -2 is
  // LUA_NOREF's value (stable across Lua 5.1-5.4) -- used as a literal here
  // since lua.h/lauxlib.h aren't included in this header (matches the
  // lua_State/lua_Debug forward-declare convention above); CloseState()
  // resets these to -2 too, since the registry (and any ref numbers in it)
  // goes away with the lua_State that owned it.
  int zkCardCallbackRef_ = -2;
  int zkRawCallbackRef_ = -2;
  int zkConnectionCallbackRef_ = -2;
  int zkAuxInputCallbackRef_ = -2;

  struct ZkPendingCallback {
    enum class Kind { Card, Raw, Connection, AuxInput } kind;
    std::string cardData;
    int doorId = 0;
    int inOutStatus = 0;
    int verifyMode = 0;
    int eventType = 0;
    int64_t timestamp = 0;
    std::string raw;
    bool connected = false;
  };
  std::mutex zkPendingMutex_;
  std::queue<ZkPendingCallback> zkPending_;
};

}  // namespace hsf
