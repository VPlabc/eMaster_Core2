#include "hsf/LuaEngine.h"
#include "hsf/plugin_manager/PluginManager.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cctype>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
// GetThreadTimes / DuplicateHandle / GetCurrentProcessorNumber, for the
// per-script CPU and core figures in RuntimeInfo().
#include <windows.h>
#endif

#include "hsf/CardCache.h"
#include "hsf/CitizenIdParser.h"
#include "hsf/ConfigManager.h"
#include "hsf/DynamicConfigManager.h"
#include "hsf/LogStore.h"
#include "hsf/Logger.h"
#include "hsf/ModbusClient.h"
#include "hsf/ModbusRegistry.h"
#include "hsf/MqClient.h"
#include "hsf/RestClient.h"
#include "hsf/RfidClient.h"
#include "hsf/RuntimeVariables.h"
#include "hsf/SerialPort.h"
#include "hsf/SqlDatabase.h"
#include "hsf/zk_controller/ZkController.h"

namespace hsf {

namespace {
const char* kEngineRegistryKey = "hsf.LuaEngine";

// Db.NULL is a light userdata pointing here. A unique address is all that is
// needed for identity, and unlike a table or a string it can never be produced
// by accident from script data.
const void* DbNullSentinel() {
  static const char sentinel = 0;
  return &sentinel;
}

LuaEngine* GetEngine(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, kEngineRegistryKey);
  auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return engine;
}

void RegisterFunction(lua_State* L, const char* name, lua_CFunction fn) {
  lua_pushcfunction(L, fn);
  lua_setfield(L, -2, name);
}

// Digs the line number out of a Lua error message so section 23's error
// report can name it. Lua formats these as "<chunk>:<line>: <text>", but the
// chunk name is a file path -- which on Windows starts "C:/", so taking the
// text before the first ':' would report the drive letter as the chunk and
// "/Users/..." as the line. Scanning for the first ":<digits>:" instead skips
// that, since a drive colon is never followed by a digit and a slash.
// Returns 0 when the message carries no position (e.g. an error raised with a
// non-string value, or one from a C binding).
int ParseErrorLine(const std::string& message) {
  for (size_t i = 0; i + 1 < message.size(); ++i) {
    if (message[i] != ':') continue;
    size_t j = i + 1;
    while (j < message.size() && std::isdigit(static_cast<unsigned char>(message[j]))) ++j;
    if (j == i + 1 || j >= message.size() || message[j] != ':') continue;
    try {
      return std::stoi(message.substr(i + 1, j - i - 1));
    } catch (const std::exception&) {
      return 0;
    }
  }
  return 0;
}

std::map<std::string, std::string> TableToStringMap(lua_State* L, int index) {
  std::map<std::string, std::string> result;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    // key at -2, value at -1
    if (lua_type(L, -2) == LUA_TSTRING) {
      std::string key = lua_tostring(L, -2);
      const char* value = lua_tostring(L, -1);
      result[key] = value ? value : "";
    }
    lua_pop(L, 1);  // pop value, keep key for lua_next
  }
  return result;
}

std::vector<bool> TableToBoolVector(lua_State* L, int index) {
  std::vector<bool> result;
  lua_Integer len = luaL_len(L, index);
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_geti(L, index, i);
    result.push_back(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
  }
  return result;
}

std::vector<uint16_t> TableToU16Vector(lua_State* L, int index) {
  std::vector<uint16_t> result;
  lua_Integer len = luaL_len(L, index);
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_geti(L, index, i);
    result.push_back(static_cast<uint16_t>(lua_tointeger(L, -1)));
    lua_pop(L, 1);
  }
  return result;
}

// Joins a path/URL onto the configured base URL for Rest.PostForm(path, data)
// / Rest.Get(path): an already-absolute URL (contains "://") is used as-is,
// otherwise it's appended to base with exactly one '/' between them.
std::string JoinUrl(const std::string& base, const std::string& pathOrUrl) {
  if (pathOrUrl.empty()) return base;
  if (pathOrUrl.find("://") != std::string::npos) return pathOrUrl;
  if (base.empty()) return pathOrUrl;

  bool baseHasSlash = base.back() == '/';
  bool pathHasSlash = pathOrUrl.front() == '/';
  if (baseHasSlash && pathHasSlash) return base + pathOrUrl.substr(1);
  if (!baseHasSlash && !pathHasSlash) return base + "/" + pathOrUrl;
  return base + pathOrUrl;
}

// Builds the single table Rest.PostForm/Rest.Get return: transport-level
// `ok`/`status`/`body` (plus `error` when set), with any top-level fields
// from a JSON object response body merged in directly — so a server
// response like {"success":true,"message":"..."} is usable as
// result.success/result.message without the script parsing JSON itself.
void PushRestResponseTable(lua_State* L, const RestResponse& response) {
  lua_newtable(L);
  lua_pushboolean(L, response.ok ? 1 : 0);
  lua_setfield(L, -2, "ok");
  lua_pushinteger(L, response.status_code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, response.body.c_str());
  lua_setfield(L, -2, "body");
  if (!response.error.empty()) {
    lua_pushstring(L, response.error.c_str());
    lua_setfield(L, -2, "error");
  }

  try {
    nlohmann::json parsed = nlohmann::json::parse(response.body);
    if (parsed.is_object()) {
      for (const auto& item : parsed.items()) {
        const auto& value = item.value();
        if (value.is_boolean()) {
          lua_pushboolean(L, value.get<bool>() ? 1 : 0);
        } else if (value.is_number_integer()) {
          lua_pushinteger(L, value.get<int64_t>());
        } else if (value.is_number_float()) {
          lua_pushnumber(L, value.get<double>());
        } else if (value.is_string()) {
          lua_pushstring(L, value.get<std::string>().c_str());
        } else {
          lua_pushstring(L, value.dump().c_str());
        }
        lua_setfield(L, -2, item.key().c_str());
      }
    }
  } catch (const nlohmann::json::exception&) {
    // Body isn't JSON (or isn't an object) - fine, script can still read .body directly.
  }
}
}  // namespace

LuaEngine::LuaEngine() = default;

LuaEngine::~LuaEngine() { Stop(); }

void LuaEngine::Bind(RestClient* rest, SerialPort* serial, SerialPort* serial2, ModbusClient* modbus,
                      RfidClient* rfid, ZkController* zk, MqClient* mq, PluginManager* plugins) {
  rest_ = rest;
  serial_ = serial;
  serial2_ = serial2;
  modbus_ = modbus;
  rfid_ = rfid;
  zk_ = zk;
  mq_ = mq;
  plugins_ = plugins;

  // No device callbacks are installed here. SerialPort and ZkController each
  // hold ONE callback, so with several engines alive the last one constructed
  // would silently steal every card read from the others. LuaRuntimeManager
  // owns those slots and fans each event out to every live engine.
}

void LuaEngine::Bind(ServiceRegistry* services) {
  if (!services) {
    Bind(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    return;
  }

  Bind(services->Get<RestClient>(ServiceNames::kRest),
       services->Get<SerialPort>(ServiceNames::kSerial),
       services->Get<SerialPort>(ServiceNames::kSerial2),
       services->Get<ModbusClient>(ServiceNames::kModbus),
       services->Get<RfidClient>(ServiceNames::kRfid),
       services->Get<ZkController>(ServiceNames::kZk),
       services->Get<MqClient>(ServiceNames::kMq),
       services->Get<PluginManager>(ServiceNames::kPlugins));
}

std::string LuaEngine::RuntimeName() const {
  std::lock_guard<std::mutex> lock(runtimeMutex_);
  return runtimeName_;
}

void LuaEngine::PushEnginePointer() {
  lua_pushlightuserdata(L_, this);
  lua_setfield(L_, LUA_REGISTRYINDEX, kEngineRegistryKey);
}

void LuaEngine::RegisterBindings() {
  PushEnginePointer();

  lua_newtable(L_);
  RegisterFunction(L_, "SetUrl", Lua_Rest_SetUrl);
  RegisterFunction(L_, "SetServer", Lua_Rest_SetUrl);  // alias
  RegisterFunction(L_, "SetApiKey", Lua_Rest_SetApiKey);
  RegisterFunction(L_, "PostForm", Lua_Rest_PostForm);
  RegisterFunction(L_, "Get", Lua_Rest_Get);
  lua_setglobal(L_, "Rest");

  // Close() releases the script's claim; the port stays open and C++ keeps it
  // that way (docs/FixSerial.md §7-§10). Shutdown() is the one that actually
  // closes the hardware.
  lua_newtable(L_);
  RegisterFunction(L_, "Open", Lua_Serial_Open);
  RegisterFunction(L_, "Close", Lua_Serial_Close);
  RegisterFunction(L_, "Release", Lua_Serial_Close);  // §10's alternative name, same function
  RegisterFunction(L_, "Shutdown", Lua_Serial_Shutdown);
  RegisterFunction(L_, "Read", Lua_Serial_Read);
  RegisterFunction(L_, "Write", Lua_Serial_Write);
  RegisterFunction(L_, "IsOpen", Lua_Serial_IsOpen);
  RegisterFunction(L_, "Status", Lua_Serial_Status);
  lua_setglobal(L_, "Serial");

  lua_newtable(L_);
  RegisterFunction(L_, "Open", Lua_Serial2_Open);
  RegisterFunction(L_, "Close", Lua_Serial2_Close);
  RegisterFunction(L_, "Release", Lua_Serial2_Close);
  RegisterFunction(L_, "Shutdown", Lua_Serial2_Shutdown);
  RegisterFunction(L_, "Read", Lua_Serial2_Read);
  RegisterFunction(L_, "Write", Lua_Serial2_Write);
  RegisterFunction(L_, "IsOpen", Lua_Serial2_IsOpen);
  RegisterFunction(L_, "Status", Lua_Serial2_Status);
  lua_setglobal(L_, "Serial2");

  lua_newtable(L_);
  RegisterFunction(L_, "Connect", Lua_Tcp_Connect);
  RegisterFunction(L_, "Send", Lua_Tcp_Send);
  RegisterFunction(L_, "Receive", Lua_Tcp_Receive);
  RegisterFunction(L_, "Close", Lua_Tcp_Close);
  lua_setglobal(L_, "Tcp");

  lua_newtable(L_);
  RegisterFunction(L_, "IsConnected", Lua_Rfid_IsConnected);
  lua_setglobal(L_, "Rfid");

  lua_newtable(L_);
  RegisterFunction(L_, "IsConnected", Lua_Modbus_IsConnected);
  RegisterFunction(L_, "CheckConnect", Lua_Modbus_CheckConnect);
  RegisterFunction(L_, "ReadCoil", Lua_Modbus_ReadCoil);
  RegisterFunction(L_, "WriteCoil", Lua_Modbus_WriteCoil);
  RegisterFunction(L_, "ReadCoils", Lua_Modbus_ReadCoils);
  RegisterFunction(L_, "WriteCoils", Lua_Modbus_WriteCoils);
  RegisterFunction(L_, "ReadHolding", Lua_Modbus_ReadHolding);
  RegisterFunction(L_, "WriteHolding", Lua_Modbus_WriteHolding);
  RegisterFunction(L_, "ReadHoldingRegisters", Lua_Modbus_ReadHoldingRegisters);
  RegisterFunction(L_, "WriteHoldingRegisters", Lua_Modbus_WriteHoldingRegisters);
  RegisterFunction(L_, "RegisterInput", Lua_Modbus_RegisterInput);
  RegisterFunction(L_, "RegisterOutput", Lua_Modbus_RegisterOutput);
  // request/upgrade.md section 19 spells the mapping call out per address
  // space, which is also the safer API: RegisterInput's default is "auto",
  // and a point that must be read with FC02 should not depend on probing.
  RegisterFunction(L_, "RegisterDiscreteInput", Lua_Modbus_RegisterDiscreteInput);
  RegisterFunction(L_, "RegisterCoilInput", Lua_Modbus_RegisterCoilInput);
  RegisterFunction(L_, "ReadDiscreteInput", Lua_Modbus_ReadDiscreteInput);
  RegisterFunction(L_, "ReadDiscreteInputs", Lua_Modbus_ReadDiscreteInputs);
  // Aliases: PLC tooling commonly labels function code 0x02 "DiscInput", so
  // that is the name people reach for first. Same functions, two spellings,
  // rather than a "no such function" dead end.
  RegisterFunction(L_, "ReadDiscInput", Lua_Modbus_ReadDiscreteInput);
  RegisterFunction(L_, "ReadDiscInputs", Lua_Modbus_ReadDiscreteInputs);
  // Input registers (FC04) -- the read-only counterpart to holding registers.
  RegisterFunction(L_, "ReadInputRegister", Lua_Modbus_ReadInputRegister);
  RegisterFunction(L_, "ReadInputRegisters", Lua_Modbus_ReadInputRegisters);
  // Typed register points shown on the dashboard.
  RegisterFunction(L_, "RegisterRegister", Lua_Modbus_RegisterRegister);
  RegisterFunction(L_, "GetRegister", Lua_Modbus_GetRegister);
  RegisterFunction(L_, "SetRegister", Lua_Modbus_SetRegister);
  lua_setglobal(L_, "Modbus");

  lua_newtable(L_);
  RegisterFunction(L_, "List", Lua_Plugin_List);
  RegisterFunction(L_, "Health", Lua_Plugin_Health);
  RegisterFunction(L_, "Read", Lua_Plugin_Read);
  RegisterFunction(L_, "Write", Lua_Plugin_Write);
  lua_setglobal(L_, "Plugin");

  lua_newtable(L_);
  RegisterFunction(L_, "Get", Lua_Card_Get);
  RegisterFunction(L_, "Clear", Lua_Card_Clear);
  RegisterFunction(L_, "Available", Lua_Card_Available);
  lua_setglobal(L_, "Card");

  // RabbitMQ (MqClient). Publishing is asynchronous -- Publish() queues and the
  // client's own thread hands it to the broker -- so a script that must know a
  // message left the process calls Mq.Flush() after it. Receiving is polled the
  // same way Card.* is, with OnMqMessage as the optional wake-up.
  lua_newtable(L_);
  RegisterFunction(L_, "IsConnected", Lua_Mq_IsConnected);
  RegisterFunction(L_, "Status", Lua_Mq_Status);
  RegisterFunction(L_, "Publish", Lua_Mq_Publish);
  RegisterFunction(L_, "PublishRaw", Lua_Mq_PublishRaw);
  RegisterFunction(L_, "Flush", Lua_Mq_Flush);
  RegisterFunction(L_, "Available", Lua_Mq_Available);
  RegisterFunction(L_, "Get", Lua_Mq_Get);
  RegisterFunction(L_, "Clear", Lua_Mq_Clear);
  RegisterFunction(L_, "Pause", Lua_Mq_Pause);
  lua_setglobal(L_, "Mq");

  lua_newtable(L_);
  RegisterFunction(L_, "Info", Lua_Log_Info);
  RegisterFunction(L_, "Warning", Lua_Log_Warning);
  RegisterFunction(L_, "Error", Lua_Log_Error);
  RegisterFunction(L_, "Debug", Lua_Log_Debug);
  // Structured logs into SQLite, searchable from the Logs page. Same table as
  // the four runtime levels above because from a script's point of view both
  // are "record this" -- what differs is where it lands and for how long
  // (request/upgrade.md section 12).
  RegisterFunction(L_, "Write", Lua_Log_Write);
  RegisterFunction(L_, "Types", Lua_Log_Types);
  lua_setglobal(L_, "Log");

  // Gateway configuration, read-through to ConfigManager rather than to
  // SQLite: request/upgrade.md section 6 is explicit that Lua must not run
  // SQL of its own.
  lua_newtable(L_);
  RegisterFunction(L_, "Get", Lua_Config_Get);
  RegisterFunction(L_, "Set", Lua_Config_Set);
  RegisterFunction(L_, "Exists", Lua_Config_Exists);
  RegisterFunction(L_, "GetCategory", Lua_Config_GetCategory);
  RegisterFunction(L_, "RegisterSchema", Lua_DynamicConfig_RegisterSchema);
  lua_setglobal(L_, "Config");

  // Project-scoped configuration API. The lowercase table is intentionally
  // separate from the legacy gateway-wide Config.Get/Set API.
  lua_newtable(L_);
  RegisterFunction(L_, "register_schema", Lua_DynamicConfig_RegisterSchema);
  RegisterFunction(L_, "extend", Lua_DynamicConfig_Extend);
  RegisterFunction(L_, "get", Lua_DynamicConfig_Get);
  RegisterFunction(L_, "set", Lua_DynamicConfig_Set);
  RegisterFunction(L_, "get_all", Lua_DynamicConfig_GetAll);
  RegisterFunction(L_, "set_all", Lua_DynamicConfig_SetAll);
  RegisterFunction(L_, "get_schema", Lua_DynamicConfig_GetSchema);
  lua_setglobal(L_, "config");

  // SQL over this script's own SQLite connection. This IS a raw SQL surface,
  // unlike Config.* above -- SmartLockerPlan.md section 15 asks for the
  // application's own tables (employees, lockers, locker_logs, system_logs) in
  // SQLite, with transactions around assignment, and no key/value API can
  // express that. Values are always BOUND, never interpolated: Db.Exec takes a
  // parameter array, and Db.Quote exists only for the identifiers binding
  // cannot cover.
  lua_newtable(L_);
  RegisterFunction(L_, "Open", Lua_Db_Open);
  RegisterFunction(L_, "Close", Lua_Db_Close);
  RegisterFunction(L_, "IsOpen", Lua_Db_IsOpen);
  RegisterFunction(L_, "Path", Lua_Db_Path);
  RegisterFunction(L_, "Exec", Lua_Db_Exec);
  RegisterFunction(L_, "Execute", Lua_Db_Exec);  // alias
  RegisterFunction(L_, "Query", Lua_Db_Query);
  RegisterFunction(L_, "QueryOne", Lua_Db_QueryOne);
  RegisterFunction(L_, "Scalar", Lua_Db_Scalar);
  RegisterFunction(L_, "Begin", Lua_Db_Begin);
  RegisterFunction(L_, "Commit", Lua_Db_Commit);
  RegisterFunction(L_, "Rollback", Lua_Db_Rollback);
  RegisterFunction(L_, "InTransaction", Lua_Db_InTransaction);
  RegisterFunction(L_, "Quote", Lua_Db_Quote);
  // Db.NULL binds SQL NULL. A Lua nil cannot do it: nil inside a parameter
  // array ends the array as far as # is concerned, so {1, nil, 3} is a
  // one-element list and the statement would be rejected for having too few
  // parameters.
  lua_pushlightuserdata(L_, const_cast<void*>(DbNullSentinel()));
  lua_setfield(L_, -2, "NULL");
  lua_setglobal(L_, "Db");

  // HTTP routes served by this script. The gateway forwards
  // /api/app/<path> to whichever script registered <path>; the handler runs on
  // the script's own thread at its next Sleep(), so a handler must return
  // promptly -- it is holding an HTTP connection open.
  lua_newtable(L_);
  RegisterFunction(L_, "Register", Lua_Http_Register);
  RegisterFunction(L_, "Unregister", Lua_Http_Unregister);
  RegisterFunction(L_, "Routes", Lua_Http_Routes);
  lua_setglobal(L_, "Http");

  lua_pushcfunction(L_, Lua_SetVariable);
  lua_setglobal(L_, "SetVariable");
  lua_pushcfunction(L_, Lua_GetVariable);
  lua_setglobal(L_, "GetVariable");
  lua_pushcfunction(L_, Lua_Sleep);
  lua_setglobal(L_, "Sleep");
}

bool LuaEngine::StartState(std::string& error) {
  L_ = luaL_newstate();
  if (!L_) {
    error = "luaL_newstate failed";
    return false;
  }
  luaL_openlibs(L_);
  RegisterBindings();
  RegisterZkControllerModule();

  // Let require()'d sibling modules resolve relative to the script's own
  // directory rather than the process's current working directory (which
  // varies depending on how/where hsf_gateway.exe was launched).
  //
  // Code run from the editor's Run button arrives via RunSource with no file
  // behind it, so scriptDir_ is empty -- fall back to the configured scripts
  // directory. Without that, editing any script that require()s a sibling
  // (which is most of them here) and pressing Run fails with "module not
  // found", while the script list's own Run button works, because that goes
  // through RunFile. Same code, two buttons, different outcome.
  // Two directories, in order: the script's own folder first, then the
  // scripts root. Once scripts can live in subfolders, a script in
  // scripts/test/ still needs the shared modules at scripts/ -- searching
  // only its own folder breaks every require of a common module, and
  // searching only the root breaks requires between siblings in a folder.
  std::vector<std::string> moduleDirs;
  if (!scriptDir_.empty()) moduleDirs.push_back(scriptDir_);
  std::string rootDir = ConfigManager::Instance().ScriptsDir();
  if (!rootDir.empty() && rootDir != scriptDir_) moduleDirs.push_back(rootDir);

  if (!moduleDirs.empty()) {
    lua_getglobal(L_, "package");
    lua_getfield(L_, -1, "path");
    std::string oldPath = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
    lua_pop(L_, 1);

    std::string newPath;
    for (const std::string& dir : moduleDirs) {
      newPath += dir + "/?.lua;" + dir + "/?/init.lua;";
    }
    newPath += oldPath;

    lua_pushstring(L_, newPath.c_str());
    lua_setfield(L_, -2, "path");
    lua_pop(L_, 1);
  }

  // Every 1000 VM instructions, check whether Stop()/Restart() asked this
  // script to abort. Without this, a script with no I/O yield point (e.g.
  // `while true do end`) would hold mutex_ forever and Stop() — which also
  // needs that same lock — would hang right along with it.
  lua_sethook(L_, DebugHook, LUA_MASKCOUNT, 1000);
  return true;
}

void LuaEngine::DebugHook(lua_State* L, lua_Debug* /*ar*/) {
  LuaEngine* engine = GetEngine(L);
  if (!engine) return;

  // Piggy-backs on the interrupt hook that already runs every 1000 VM
  // instructions. Sampling here (rather than from the web thread) is what
  // makes metrics available for a script holding mutex_ for its whole run --
  // this executes on the script's own thread, with the state already safe to
  // read. Throttled, since the hook itself fires very frequently and
  // lua_gc/GetCurrentProcessorNumber are not free.
  if ((engine->hookTicks_.fetch_add(1) % 64) == 0) {
    engine->SampleRuntimeMetrics(L);
  }

  if (engine->interruptRequested_.load()) {
    luaL_error(L, "script interrupted by Stop()");
  }
}

void LuaEngine::SampleRuntimeMetrics(lua_State* L) {
  if (!L) return;

  // Lua's own accounting of its heap -- the meaningful "how much memory is
  // this script using" number. Process RSS would be dominated by the
  // gateway itself and say nothing about the script.
  int kb = lua_gc(L, LUA_GCCOUNT);
  int bytes = lua_gc(L, LUA_GCCOUNTB);
  runtimeMemoryKb_.store(static_cast<double>(kb) + static_cast<double>(bytes) / 1024.0);

#if defined(_WIN32)
  runtimeCore_.store(static_cast<int>(::GetCurrentProcessorNumber()));
#endif
}

void LuaEngine::BeginRuntimeTracking(const std::string& name, const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(runtimeMutex_);
    runtimeName_ = name;
    runtimePath_ = path;
    runtimeError_.clear();
  }
  runtimeStart_.store(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  runtimeMemoryKb_.store(0.0);
  runtimeCore_.store(-1);
  runtimeCpuPercent_.store(0.0);
  runtimeLastCpu100ns_.store(0);
  runtimeLastSampleMs_.store(0);

  ReleaseCpuHandle();
#if defined(_WIN32)
  // A real handle to *this* thread -- the pseudo-handle from
  // GetCurrentThread() is only meaningful on the thread that called it, and
  // the metrics are read from the web thread.
  HANDLE dup = nullptr;
  if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &dup,
                        THREAD_QUERY_INFORMATION, FALSE, 0)) {
    runtimeThread_.store(dup);
  }
#endif
}

void LuaEngine::ReleaseCpuHandle() {
#if defined(_WIN32)
  void* previous = runtimeThread_.exchange(nullptr);
  if (previous) ::CloseHandle(static_cast<HANDLE>(previous));
#else
  runtimeThread_.store(nullptr);
#endif
}

void LuaEngine::EndRuntimeTracking(const std::string& state, const std::string& error) {
  {
    std::lock_guard<std::mutex> lock(runtimeMutex_);
    if (state == "STOPPED") {
      // A deliberate stop is not a failure. Without clearing, the interrupt
      // the debug hook raises to unstick a looping script ("script
      // interrupted by Stop()") lands in last_error and the System Info
      // panel reports ERROR for every normal stop.
      runtimeError_.clear();
    } else if (!error.empty()) {
      runtimeError_ = error;
    }
  }
  ReleaseCpuHandle();
  runtimeCpuPercent_.store(0.0);
}

LuaRuntimeInfo LuaEngine::RuntimeInfo() const {
  LuaRuntimeInfo info;
  info.running = running_.load();
  info.runtime_id = runtimeId_.load();

  {
    std::lock_guard<std::mutex> lock(runtimeMutex_);
    info.name = runtimeName_;
    info.path = runtimePath_;
    info.last_error = runtimeError_;
  }
  info.error_line = ParseErrorLine(info.last_error);

  info.start_time = runtimeStart_.load();
  info.memory_kb = runtimeMemoryKb_.load();
  info.cpu_core = runtimeCore_.load();

  if (info.running) {
    info.state = "RUNNING";
  } else if (!info.last_error.empty()) {
    info.state = "ERROR";
  } else {
    info.state = "STOPPED";
  }

  int64_t nowSec =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (info.running && info.start_time > 0) {
    info.uptime_seconds = static_cast<double>(nowSec - info.start_time);
  }

#if defined(_WIN32)
  // CPU% as a delta between samples: total thread CPU time alone would only
  // ever report a lifetime average, which flattens out and stops reflecting
  // what the script is doing now.
  void* handle = runtimeThread_.load();
  if (info.running && handle) {
    FILETIME creation, exit, kernel, user;
    if (::GetThreadTimes(static_cast<HANDLE>(handle), &creation, &exit, &kernel, &user)) {
      ULARGE_INTEGER k, u;
      k.LowPart = kernel.dwLowDateTime;
      k.HighPart = kernel.dwHighDateTime;
      u.LowPart = user.dwLowDateTime;
      u.HighPart = user.dwHighDateTime;
      uint64_t cpu100ns = k.QuadPart + u.QuadPart;

      int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
      uint64_t prevCpu = runtimeLastCpu100ns_.load();
      int64_t prevMs = runtimeLastSampleMs_.load();

      if (prevMs > 0 && nowMs > prevMs && cpu100ns >= prevCpu) {
        double cpuMs = static_cast<double>(cpu100ns - prevCpu) / 10000.0;
        double wallMs = static_cast<double>(nowMs - prevMs);
        double percent = (cpuMs / wallMs) * 100.0;
        if (percent < 0.0) percent = 0.0;
        if (percent > 100.0) percent = 100.0;
        runtimeCpuPercent_.store(percent);
      }
      runtimeLastCpu100ns_.store(cpu100ns);
      runtimeLastSampleMs_.store(nowMs);
    }
  }
#endif

  info.cpu_percent = runtimeCpuPercent_.load();
  return info;
}

void LuaEngine::CloseState() {
  if (L_) {
    lua_close(L_);
    L_ = nullptr;
  }
  // The registry (and every ref number in it) went away with L_ -- reset so
  // DrainZkPendingCallbacks() can't rawgeti a stale ref number into a *new*
  // state that happens to reuse the same small integer.
  zkCardCallbackRef_ = LUA_NOREF;
  zkRawCallbackRef_ = LUA_NOREF;
  zkConnectionCallbackRef_ = LUA_NOREF;
  zkAuxInputCallbackRef_ = LUA_NOREF;

  // Routes belonged to the state that just closed; their handler refs are as
  // stale as the zk ones above. Anything still queued has to be answered here
  // and not left to time out: a stopped script means the request will never be
  // served, and 503 now beats a web client blocked for five seconds.
  std::queue<std::shared_ptr<LuaHttpRequest>> abandoned;
  {
    std::lock_guard<std::mutex> lock(httpMutex_);
    httpRoutes_.clear();
    abandoned.swap(httpPending_);
  }

  while (!abandoned.empty()) {
    auto request = abandoned.front();
    abandoned.pop();
    if (!request) continue;
    std::lock_guard<std::mutex> lock(request->mutex);
    request->status = 503;
    request->error = "the script serving this route stopped";
    request->completed = true;
    request->done.notify_all();
  }

  // The SQLite connection is per-run: a script that is restarted re-opens it
  // with its own Db.Open(), and holding the file open past Stop() would keep a
  // WAL sidecar around for a runtime the operator believes is gone.
  if (db_) db_->Close();
}

// static
bool LuaEngine::Validate(const std::string& code, std::string& error) {
  lua_State* tmp = luaL_newstate();
  if (!tmp) {
    error = "luaL_newstate failed";
    return false;
  }
  int rc = luaL_loadstring(tmp, code.c_str());
  bool ok = rc == LUA_OK;
  if (!ok) {
    const char* msg = lua_tostring(tmp, -1);
    error = msg ? msg : "unknown syntax error";
  }
  lua_close(tmp);
  return ok;
}

bool LuaEngine::RunFile(const std::string& path, std::string& error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    error = "failed to open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  scriptDir_ = std::filesystem::path(path).parent_path().string();

  // Named relative to the scripts directory so the System Info panel shows
  // "card/issue.lua" rather than an absolute path nobody can read at a
  // glance. Set before RunSource, which is what stamps the start time.
  std::error_code relEc;
  std::filesystem::path rel =
      std::filesystem::relative(path, ConfigManager::Instance().ScriptsDir(), relEc);
  pendingRunName_ = relEc ? std::filesystem::path(path).filename().string() : rel.generic_string();
  pendingRunPath_ = path;

  return RunSource(ss.str(), error);
}

bool LuaEngine::RunBundle(const std::vector<std::pair<std::string, std::string>>& modules,
                          const std::string& entryBytecode, const std::string& displayName,
                          std::string& error) {
  if (entryBytecode.empty()) {
    error = "the package has no entry chunk";
    return false;
  }
  // Left EMPTY, deliberately. scriptDir_ is what StartState uses to extend
  // package.path, and a deployed package must not be able to fall back to
  // loading a plaintext .lua from disk when a require() misses -- that would
  // quietly reintroduce exactly the source files the package exists to
  // replace, and it would do so silently.
  scriptDir_.clear();

  pendingPreload_ = modules;
  pendingRunName_ = displayName;
  pendingRunPath_.clear();  // there is no file; the System Info panel says so

  return RunSource(entryBytecode, error);
}

bool LuaEngine::RunSource(const std::string& code, std::string& error) {
  // Whatever's currently running (if anything) gets replaced by this call,
  // so it needs the same unstick-then-lock sequence as Stop()/Restart():
  // without this, running a new script while an infinite-loop script (with
  // no I/O yield point) is already active would just hang forever waiting
  // for a lock nothing will ever release on its own.
  interruptRequested_.store(true);
  std::lock_guard<std::mutex> lock(mutex_);
  interruptRequested_.store(false);

  CloseState();
  running_.store(false);

  // Modbus point names describe the script that registered them, so the
  // outgoing script's I/O must not linger on the dashboard driving nothing.
  // Cleared here rather than in Stop() so the points stay visible while a
  // script is merely stopped, and only vanish once something replaces it.
  //
  // Scoped to THIS runtime's registrations, not the whole registry: several
  // scripts now run at once (request/upgrade.md section 20), and a global
  // Clear() here meant re-running one script erased every other script's
  // dashboard points -- they would keep running while their named I/O
  // silently disappeared.
  ModbusRegistry::Instance().ClearOwner(runtimeId_.load());

  if (!StartState(error)) return false;

  // A production bundle's modules go into package.preload before anything
  // runs, so require("smartlocker.locker") inside the application finds a
  // loader already in memory and never reaches package.path. Nothing was
  // written to disk for it to find -- which is the point of section 2.7 --
  // and the application needs no change to be packaged.
  //
  // Consumed here and cleared, like the run name below: an ordinary editor Run
  // after a package Run must not inherit the package's modules.
  if (!pendingPreload_.empty()) {
    lua_getglobal(L_, "package");
    if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "preload");
      if (lua_istable(L_, -1)) {
        for (const auto& [moduleName, moduleBytecode] : pendingPreload_) {
          // Named with '=' so a runtime error inside a module reads as
          // "smartlocker.locker:42:" rather than quoting the binary chunk.
          const std::string chunk = "=" + moduleName;
          if (luaL_loadbuffer(L_, moduleBytecode.data(), moduleBytecode.size(), chunk.c_str()) != LUA_OK) {
            const char* message = lua_tostring(L_, -1);
            error = "module \"" + moduleName + "\": " + (message ? message : "failed to load");
            lua_pop(L_, 3);  // message, preload, package
            pendingPreload_.clear();
            CloseState();
            return false;
          }
          lua_setfield(L_, -2, moduleName.c_str());
        }
      }
      lua_pop(L_, 1);  // preload

      // package.path and package.cpath are EMPTIED for a package run, so
      // preload is the only way a require() can be satisfied.
      //
      // This is not tidiness. Without it, a module missing from the bundle --
      // or named wrongly when it was built -- falls through to the filesystem
      // and quietly loads the plaintext .lua sitting next door, so the package
      // "works" on the build machine and fails on the first device that does
      // not have the sources. That exact bug got through here once: the module
      // names were computed relative to the wrong root, every require() missed
      // preload, and the whole thing appeared to run correctly until the
      // source tree was moved away. A deployed package must depend on nothing
      // outside itself, and the way to be sure is to make it impossible.
      lua_pushstring(L_, "");
      lua_setfield(L_, -2, "path");
      lua_pushstring(L_, "");
      lua_setfield(L_, -2, "cpath");
    }
    lua_pop(L_, 1);  // package
    Logger::Instance().Info(LogCategory::Lua, "Preloaded " + std::to_string(pendingPreload_.size()) +
                                                   " module(s) from a compiled package; package.path is "
                                                   "empty, so require() can only resolve in-memory");
    pendingPreload_.clear();
  }

  // Consumed before the load, since the chunk name below is built from it.
  // Cleared so a subsequent editor Run doesn't inherit the previous file's
  // name.
  std::string runName = pendingRunName_;
  std::string runPath = pendingRunPath_;
  pendingRunName_.clear();
  pendingRunPath_.clear();

  // Names the chunk after the script, so Lua's own error messages read
  // "card/issue.lua:145: attempt to call nil value" -- the form
  // request/upgrade.md section 23 shows. luaL_loadstring names the chunk after
  // the SOURCE TEXT, which produced messages like `[string "-- card issuing
  // workflow..."]:145:` that identify nothing when several scripts run at
  // once. A leading '@' tells Lua the name is a filename and should be printed
  // bare; '=' means "print verbatim", which is what the editor buffer wants.
  std::string chunkName = runName.empty() ? "=[editor buffer]" : "@" + runName;

  if (luaL_loadbuffer(L_, code.c_str(), code.size(), chunkName.c_str()) != LUA_OK) {
    const char* msg = lua_tostring(L_, -1);
    error = msg ? msg : "unknown load error";
    CloseState();

    // Recorded here rather than left to EndRuntimeTracking, which only runs
    // for a script that actually started: without this a script that fails to
    // COMPILE reports as a silent STOPPED in the runtime list, with the syntax
    // error visible only to whoever pressed Run.
    {
      std::lock_guard<std::mutex> guard(runtimeMutex_);
      runtimeName_ = runName;
      runtimePath_ = runPath;
      runtimeError_ = error;
    }
    return false;
  }

  // Set before the pcall, not after: a top-level script that never returns
  // (e.g. an intentional `while true do ... end` polling loop, rather than
  // one that just defines handlers) is still genuinely running the whole
  // time it executes, and should report that on the dashboard instead of
  // looking stopped until a pcall that may never come back.
  lastSource_ = code;
  running_.store(true);

  BeginRuntimeTracking(runName, runPath);

  Logger::Instance().Info(LogCategory::Lua,
                           "Script started: " + (runName.empty() ? std::string("(editor buffer)") : runName));

  if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L_, -1);
    error = msg ? msg : "unknown runtime error";
    Logger::Instance().Error(LogCategory::Lua, error);
    CloseState();
    running_.store(false);
    // Stop()/Restart()/a replacing RunSource unstick a looping script by
    // raising an error from the debug hook, so a failed pcall while an
    // interrupt is pending is that mechanism working -- not a script fault.
    // Recording it would mark every stop as ERROR.
    EndRuntimeTracking(interruptRequested_.load() ? "STOPPED" : "ERROR", error);
    return false;
  }

  // Falling out of the pcall means the top-level code returned. Handler-only
  // scripts do that immediately and stay live via queued events, so this is
  // not "finished" -- running_ stays true and only Stop() clears it.
  //
  // Sampled here because for exactly those scripts nothing else will: the
  // debug hook only samples every 64th firing (64k VM instructions) and
  // Sleep() is never called, so a handler-only script sat in System Info
  // reporting 0.00 MB and an unknown core for its whole life. This runs on the
  // script's own thread with the state alive, which is what the sampler needs.
  SampleRuntimeMetrics(L_);

  EndRuntimeTracking("RUNNING", "");
  return true;
}

void LuaEngine::Stop() {
  // Set before attempting the lock: if a script is stuck in a loop with no
  // yield point, this is what the debug hook installed in StartState()
  // checks to abort it, so mutex_ actually gets released instead of Stop()
  // hanging right along with the stuck script.
  interruptRequested_.store(true);
  std::lock_guard<std::mutex> lock(mutex_);
  CloseState();
  running_.store(false);
  interruptRequested_.store(false);

  // request/upgrade.md section 28 asks a stopped script to release its
  // resources. Closing the Lua state above covers everything owned by the VM;
  // this socket is owned by the engine instead (Tcp.Connect leaves it open
  // across calls by design), so a script stopped mid-conversation would
  // otherwise hold the connection until the whole gateway exited. The shared
  // devices -- serial ports, the PLC link -- are deliberately NOT touched:
  // other scripts are still using them.
  luaTcp_.Close();

  EndRuntimeTracking("STOPPED", "");
}

bool LuaEngine::Restart(std::string& error) {
  interruptRequested_.store(true);
  std::string source;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    source = lastSource_;
    interruptRequested_.store(false);
  }
  if (source.empty()) {
    error = "no script has been run yet";
    return false;
  }
  return RunSource(source, error);
}

std::string LuaEngine::LastSource() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastSource_;
}

void LuaEngine::DispatchEvent(const std::string& functionName, const std::string& arg) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!L_ || !running_.load()) return;

  lua_getglobal(L_, functionName.c_str());
  if (!lua_isfunction(L_, -1)) {
    lua_pop(L_, 1);
    return;
  }
  lua_pushstring(L_, arg.c_str());
  if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L_, -1);
    Logger::Instance().Error(LogCategory::Lua, functionName + " failed: " + (msg ? msg : "unknown error"));
    lua_pop(L_, 1);
  }

  // Handler-style scripts spend their whole life in these calls, so this is
  // where their memory figure has to come from -- same reason as the sample
  // after RunSource's pcall. Safe here: mutex_ is held and this thread is the
  // only one inside the state.
  SampleRuntimeMetrics(L_);
}

void LuaEngine::DispatchEvent(const std::string& functionName, long long argA, double argB) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!L_ || !running_.load()) return;

  lua_getglobal(L_, functionName.c_str());
  if (!lua_isfunction(L_, -1)) {
    lua_pop(L_, 1);
    return;
  }
  lua_pushinteger(L_, argA);
  lua_pushnumber(L_, argB);
  if (lua_pcall(L_, 2, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L_, -1);
    Logger::Instance().Error(LogCategory::Lua, functionName + " failed: " + (msg ? msg : "unknown error"));
    lua_pop(L_, 1);
  }
}

void LuaEngine::QueueEvent(const std::string& functionName, const std::string& arg) {
  std::lock_guard<std::mutex> lock(pendingEventsMutex_);
  pendingEvents_.push(PendingEvent{functionName, arg});
}

void LuaEngine::DrainPendingEvents(lua_State* L) {
  // Called from within a native binding (Sleep, Serial.Read) on the same
  // thread that's already running the script inside RunSource's pcall —
  // mutex_ is already held by that call, on this thread, so calling
  // straight into L here is safe without acquiring it again (and trying to
  // would deadlock: std::mutex isn't recursive).
  for (;;) {
    PendingEvent event;
    {
      std::lock_guard<std::mutex> lock(pendingEventsMutex_);
      if (pendingEvents_.empty()) break;
      event = std::move(pendingEvents_.front());
      pendingEvents_.pop();
    }

    lua_getglobal(L, event.functionName.c_str());
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    lua_pushstring(L, event.arg.c_str());
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      Logger::Instance().Error(LogCategory::Lua,
                                event.functionName + " failed: " + (msg ? msg : "unknown error"));
      lua_pop(L, 1);
    }
  }

  DrainZkPendingCallbacks(L);

  // HTTP last, and on every drain: a web client is holding a connection open
  // waiting for this, so the latency that matters is "how often does the script
  // sleep", not "how often does it check for requests".
  DrainPendingHttp(L);
}

void LuaEngine::DrainZkPendingCallbacks(lua_State* L) {
  for (;;) {
    ZkPendingCallback item;
    {
      std::lock_guard<std::mutex> lock(zkPendingMutex_);
      if (zkPending_.empty()) return;
      item = std::move(zkPending_.front());
      zkPending_.pop();
    }

    int ref = LUA_NOREF;
    switch (item.kind) {
      case ZkPendingCallback::Kind::Card:
        ref = zkCardCallbackRef_;
        break;
      case ZkPendingCallback::Kind::Raw:
        ref = zkRawCallbackRef_;
        break;
      case ZkPendingCallback::Kind::Connection:
        ref = zkConnectionCallbackRef_;
        break;
      case ZkPendingCallback::Kind::AuxInput:
        ref = zkAuxInputCallbackRef_;
        break;
    }
    if (ref == LUA_NOREF) continue;  // no zk.onXxx() callback registered -- drop

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    switch (item.kind) {
      case ZkPendingCallback::Kind::Card: {
        lua_newtable(L);
        lua_pushstring(L, item.cardData.c_str());
        lua_setfield(L, -2, "CardData");
        lua_pushinteger(L, item.doorId);
        lua_setfield(L, -2, "DoorID");
        // RTLog field 5 -- confirmed as the reader index by
        // sdk-protocol-reference.md's GetRTLog buffer table (Attachment 7):
        // "Entry/Exit status: 0=entry, 1=exit, 2=none". Each door has an
        // entry and an exit reader, so this identifies which of the two
        // produced the swipe. InOutStatus below is the same value under its
        // raw RTLog field name, kept for scripts that prefer that reading.
        lua_pushinteger(L, item.inOutStatus);
        lua_setfield(L, -2, "ReaderID");
        lua_pushinteger(L, item.inOutStatus);
        lua_setfield(L, -2, "InOutStatus");
        lua_pushinteger(L, item.verifyMode);
        lua_setfield(L, -2, "VerifyMode");
        lua_pushinteger(L, item.eventType);
        lua_setfield(L, -2, "EventType");
        lua_pushinteger(L, item.timestamp);
        lua_setfield(L, -2, "Timestamp");
        break;
      }
      case ZkPendingCallback::Kind::Raw:
        lua_pushstring(L, item.raw.c_str());
        break;
      case ZkPendingCallback::Kind::Connection:
        lua_pushboolean(L, item.connected ? 1 : 0);
        break;
      case ZkPendingCallback::Kind::AuxInput:
        // onAuxInput(input, shorted) -- two arguments, unlike the others,
        // which is what `args` below exists for.
        lua_pushinteger(L, item.doorId);
        lua_pushboolean(L, item.connected ? 1 : 0);
        break;
    }

    int args = item.kind == ZkPendingCallback::Kind::AuxInput ? 2 : 1;

    if (lua_pcall(L, args, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      Logger::Instance().Error(LogCategory::Lua,
                                std::string("zk_controller callback failed: ") + (msg ? msg : "unknown error"));
      lua_pop(L, 1);
    }
  }
}

void LuaEngine::QueueZkCardEvent(const ZkCardEvent& event) {
  ZkPendingCallback item;
  item.kind = ZkPendingCallback::Kind::Card;
  item.cardData = event.cardData;
  item.doorId = event.doorId;
  item.inOutStatus = event.inOutStatus;
  item.verifyMode = event.verifyMode;
  item.eventType = event.eventType;
  item.timestamp = event.timestamp;
  {
    std::lock_guard<std::mutex> lock(zkPendingMutex_);
    zkPending_.push(std::move(item));
  }
  TryDeliverZkPendingCallbacksNow();
}

void LuaEngine::QueueZkRawEvent(const std::string& raw) {
  ZkPendingCallback item;
  item.kind = ZkPendingCallback::Kind::Raw;
  item.raw = raw;
  {
    std::lock_guard<std::mutex> lock(zkPendingMutex_);
    zkPending_.push(std::move(item));
  }
  TryDeliverZkPendingCallbacksNow();
}

void LuaEngine::QueueZkConnectionEvent(bool connected) {
  ZkPendingCallback item;
  item.kind = ZkPendingCallback::Kind::Connection;
  item.connected = connected;
  {
    std::lock_guard<std::mutex> lock(zkPendingMutex_);
    zkPending_.push(std::move(item));
  }
  TryDeliverZkPendingCallbacksNow();
}

void LuaEngine::QueueZkAuxInputEvent(int input, bool shorted) {
  ZkPendingCallback item;
  item.kind = ZkPendingCallback::Kind::AuxInput;
  // Reuses doorId/connected rather than adding two more fields: for an
  // auxiliary-input record the RTLog "door" field IS the input number, which is
  // the same reuse the protocol itself does.
  item.doorId = input;
  item.connected = shorted;
  {
    std::lock_guard<std::mutex> lock(zkPendingMutex_);
    zkPending_.push(std::move(item));
  }
  TryDeliverZkPendingCallbacksNow();
}

void LuaEngine::TryDeliverZkPendingCallbacksNow() {
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // script is mid-run (e.g. a polling loop) -- its own
                                   // Sleep()/Serial.Read() will drain this later instead
  if (!L_ || !running_.load()) return;
  DrainZkPendingCallbacks(L_);
}

void LuaEngine::RegisterZkControllerModule() {
  lua_getglobal(L_, "package");
  lua_getfield(L_, -1, "preload");
  lua_pushcfunction(L_, Lua_ZkController_Open);
  lua_setfield(L_, -2, "zk_controller");
  lua_pop(L_, 2);  // preload, package
}

int LuaEngine::Lua_ZkController_Open(lua_State* L) {
  lua_newtable(L);
  RegisterFunction(L, "connect", Lua_ZkController_Connect);
  RegisterFunction(L, "disconnect", Lua_ZkController_Disconnect);
  RegisterFunction(L, "isConnected", Lua_ZkController_IsConnected);
  RegisterFunction(L, "setAutoReconnect", Lua_ZkController_SetAutoReconnect);
  RegisterFunction(L, "startRTLog", Lua_ZkController_StartRTLog);
  RegisterFunction(L, "stopRTLog", Lua_ZkController_StopRTLog);
  RegisterFunction(L, "onCard", Lua_ZkController_OnCard);
  RegisterFunction(L, "onRawRTLog", Lua_ZkController_OnRawRTLog);
  RegisterFunction(L, "onConnectionChanged", Lua_ZkController_OnConnectionChanged);
  RegisterFunction(L, "onAuxInput", Lua_ZkController_OnAuxInput);
  RegisterFunction(L, "lastError", Lua_ZkController_LastError);

  // Output control (sdk-protocol-reference.md section 3.5). controlDevice is
  // the raw call; the rest are the operations by name, which is what a script
  // should reach for.
  RegisterFunction(L, "controlDevice", Lua_ZkController_ControlDevice);
  RegisterFunction(L, "openDoor", Lua_ZkController_OpenDoor);
  RegisterFunction(L, "auxOut", Lua_ZkController_AuxOut);
  RegisterFunction(L, "pulse", Lua_ZkController_Pulse);
  RegisterFunction(L, "beep", Lua_ZkController_Beep);
  RegisterFunction(L, "cancelAlarm", Lua_ZkController_CancelAlarm);
  RegisterFunction(L, "restartDevice", Lua_ZkController_RestartDevice);
  RegisterFunction(L, "setNormallyOpen", Lua_ZkController_SetNormallyOpen);

  // Parameters and the I/O state the panel reports.
  RegisterFunction(L, "getParam", Lua_ZkController_GetParam);
  RegisterFunction(L, "setParam", Lua_ZkController_SetParam);
  RegisterFunction(L, "ioState", Lua_ZkController_IoState);
  RegisterFunction(L, "doorState", Lua_ZkController_DoorState);
  RegisterFunction(L, "inputState", Lua_ZkController_InputState);
  return 1;
}

int LuaEngine::Lua_ZkController_Connect(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* ip = luaL_checkstring(L, 1);
  int port = static_cast<int>(luaL_optinteger(L, 2, 4370));
  int timeoutMs = static_cast<int>(luaL_optinteger(L, 3, 2000));
  const char* password = luaL_optstring(L, 4, "");

  if (!engine || !engine->zk_) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "zk_controller not available");
    return 2;
  }

  bool ok = engine->zk_->Connect(ip, port, timeoutMs, password);
  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) return 1;
  lua_pushstring(L, engine->zk_->LastError().c_str());
  return 2;
}

int LuaEngine::Lua_ZkController_Disconnect(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->zk_) engine->zk_->Disconnect();
  return 0;
}

int LuaEngine::Lua_ZkController_IsConnected(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  bool connected = engine && engine->zk_ && engine->zk_->IsConnected();
  lua_pushboolean(L, connected ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_ZkController_SetAutoReconnect(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  bool enable = lua_toboolean(L, 1) != 0;
  if (engine && engine->zk_) engine->zk_->SetAutoReconnect(enable);
  return 0;
}

int LuaEngine::Lua_ZkController_StartRTLog(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->zk_) engine->zk_->StartRTLog();
  return 0;
}

int LuaEngine::Lua_ZkController_StopRTLog(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->zk_) engine->zk_->StopRTLog();
  return 0;
}

int LuaEngine::Lua_ZkController_LastError(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  std::string err = engine && engine->zk_ ? engine->zk_->LastError() : "";
  lua_pushstring(L, err.c_str());
  return 1;
}

int LuaEngine::Lua_ZkController_OnAuxInput(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (!engine) return 0;
  if (engine->zkAuxInputCallbackRef_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, engine->zkAuxInputCallbackRef_);
  }
  lua_pushvalue(L, 1);
  engine->zkAuxInputCallbackRef_ = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

// --- zk output control -----------------------------------------------------

namespace {

// Every control binding answers the same way: true, or nil plus the reason, so
// a script can `if not zk.openDoor(1, 5) then` and get a message worth logging.
int PushZkResult(lua_State* L, ZkController* zk, bool ok) {
  if (ok) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, zk ? zk->LastError().c_str() : "the ZK controller is not available in this build");
  return 2;
}

}  // namespace

// The controller this script's zk.* calls act on, or null when the engine was
// constructed without one (Validate()'s throwaway state) or the PullSDK is not
// in this build.
ZkController* LuaEngine::ZkForBinding(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  return engine ? engine->zk_ : nullptr;
}

// zk.controlDevice(operationId, param1, param2, param3, param4 [, options])
//
// The raw call, for an operation this module does not wrap. See
// sdk-protocol-reference.md Attached Table 3 for the parameter meanings.
int LuaEngine::Lua_ZkController_ControlDevice(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int operationId = static_cast<int>(luaL_checkinteger(L, 1));
  int param1 = static_cast<int>(luaL_optinteger(L, 2, 0));
  int param2 = static_cast<int>(luaL_optinteger(L, 3, 0));
  int param3 = static_cast<int>(luaL_optinteger(L, 4, 0));
  int param4 = static_cast<int>(luaL_optinteger(L, 5, 0));
  const char* options = luaL_optstring(L, 6, "");

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->ControlDevice(operationId, param1, param2, param3, param4, options));
}

// zk.openDoor(door [, seconds]) -- 0 releases now, 1..60 holds for that many
// seconds, 255 latches the relay open.
int LuaEngine::Lua_ZkController_OpenDoor(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int door = static_cast<int>(luaL_checkinteger(L, 1));
  int seconds = static_cast<int>(luaL_optinteger(L, 2, 5));

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->OpenDoor(door, seconds));
}

// zk.auxOut(output [, seconds]) -- same, for an auxiliary output.
int LuaEngine::Lua_ZkController_AuxOut(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int output = static_cast<int>(luaL_checkinteger(L, 1));
  int seconds = static_cast<int>(luaL_optinteger(L, 2, 1));

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->SetAuxOutput(output, seconds));
}

// zk.pulse(number, durationMs [, auxiliary]) -- millisecond control, by
// latching the relay and releasing it after the wait. `auxiliary` defaults to
// true, since that is what a buzzer or a signal lamp is wired to.
//
// BLOCKS for durationMs on the calling script's thread.
int LuaEngine::Lua_ZkController_Pulse(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int number = static_cast<int>(luaL_checkinteger(L, 1));
  int durationMs = static_cast<int>(luaL_optinteger(L, 2, 150));
  bool auxiliary = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->PulseOutput(number, auxiliary, durationMs));
}

// zk.beep(number [, count] [, durationMs] [, gapMs] [, auxiliary])
//
// The PullSDK has NO beeper command -- nothing in plcommpro.dll addresses the
// reader's own sounder, and no device parameter configures it either (see
// sdk-protocol-reference.md Attached Table 2). This drives whatever is wired to
// the named output, which is how an audible signal is produced on this panel.
int LuaEngine::Lua_ZkController_Beep(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int number = static_cast<int>(luaL_checkinteger(L, 1));
  int count = static_cast<int>(luaL_optinteger(L, 2, 1));
  int durationMs = static_cast<int>(luaL_optinteger(L, 3, 150));
  int gapMs = static_cast<int>(luaL_optinteger(L, 4, 150));
  bool auxiliary = lua_isnoneornil(L, 5) ? true : (lua_toboolean(L, 5) != 0);

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->Beep(number, auxiliary, count, durationMs, gapMs));
}

int LuaEngine::Lua_ZkController_CancelAlarm(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->CancelAlarm());
}

int LuaEngine::Lua_ZkController_RestartDevice(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->RestartDevice());
}

int LuaEngine::Lua_ZkController_SetNormallyOpen(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int door = static_cast<int>(luaL_checkinteger(L, 1));
  bool enable = lua_toboolean(L, 2) != 0;

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->SetNormallyOpen(door, enable));
}

// zk.getParam("LockCount,AuxOutCount") -> { LockCount = "4", ... } | nil
//
// Values stay strings: the panel's parameter space mixes numbers, IP addresses
// and bit masks, and guessing which is which here would be worse than letting
// the script call tonumber().
int LuaEngine::Lua_ZkController_GetParam(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  const char* items = luaL_checkstring(L, 1);

  if (!zk) {
    lua_pushnil(L);
    lua_pushstring(L, "the ZK controller is not available in this build");
    return 2;
  }

  std::map<std::string, std::string> params = zk->GetParams(items);

  if (params.empty()) {
    lua_pushnil(L);
    lua_pushstring(L, zk->LastError().c_str());
    return 2;
  }

  lua_newtable(L);
  for (const auto& entry : params) {
    lua_pushlstring(L, entry.second.c_str(), entry.second.size());
    lua_setfield(L, -2, entry.first.c_str());
  }
  return 1;
}

// zk.setParam("Door1Drivertime=6")
int LuaEngine::Lua_ZkController_SetParam(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  const char* itemValues = luaL_checkstring(L, 1);

  if (!zk) return PushZkResult(L, nullptr, false);
  return PushZkResult(L, zk, zk->SetParams(itemValues));
}

namespace {

// Door sensor byte -> the word a script wants. Kept as text rather than a
// boolean because "no sensor configured" is a third state, and a nil there
// would be indistinguishable from "no such door".
const char* DoorStateName(int dss) {
  switch (dss) {
    case 1:
      return "CLOSED";
    case 2:
      return "OPEN";
    case 0:
      return "NO_SENSOR";
    default:
      return "UNKNOWN";
  }
}

}  // namespace

// zk.ioState() -> {
//   status_seen, status_time, doors = { "CLOSED", ... }, alarms = { n, ... },
//   inputs = { [1] = true }, counts = { locks, aux_out, aux_in, readers } }
//
// This is everything the controller has told us about its own I/O. Read the
// note on ZkIoState (ZkController.h) before trusting a field: the PullSDK has
// no read-input call, so `doors` is the last status record the panel sent and
// `inputs` is the last EDGE seen per auxiliary input -- an input nobody has
// triggered since start-up is absent, not false.
int LuaEngine::Lua_ZkController_IoState(lua_State* L) {
  ZkController* zk = ZkForBinding(L);

  lua_newtable(L);

  if (!zk) {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "status_seen");
    return 1;
  }

  ZkIoState state = zk->IoState();

  lua_pushboolean(L, state.status_seen ? 1 : 0);
  lua_setfield(L, -2, "status_seen");
  lua_pushlstring(L, state.status_time.c_str(), state.status_time.size());
  lua_setfield(L, -2, "status_time");

  lua_newtable(L);
  for (size_t i = 0; i < state.doors.size(); ++i) {
    lua_pushstring(L, DoorStateName(state.doors[i]));
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "doors");

  lua_newtable(L);
  for (size_t i = 0; i < state.doors.size(); ++i) {
    lua_pushinteger(L, state.doors[i]);
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "door_bytes");

  lua_newtable(L);
  for (size_t i = 0; i < state.alarms.size(); ++i) {
    lua_pushinteger(L, state.alarms[i]);
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "alarms");

  lua_newtable(L);
  for (const auto& entry : state.aux_inputs) {
    lua_pushboolean(L, entry.second ? 1 : 0);
    lua_seti(L, -2, entry.first);
  }
  lua_setfield(L, -2, "inputs");

  lua_pushboolean(L, state.aux_input_seen ? 1 : 0);
  lua_setfield(L, -2, "input_seen");

  lua_newtable(L);
  lua_pushinteger(L, state.lock_count);
  lua_setfield(L, -2, "locks");
  lua_pushinteger(L, state.aux_out_count);
  lua_setfield(L, -2, "aux_out");
  lua_pushinteger(L, state.aux_in_count);
  lua_setfield(L, -2, "aux_in");
  lua_pushinteger(L, state.reader_count);
  lua_setfield(L, -2, "readers");
  lua_setfield(L, -2, "counts");

  return 1;
}

// zk.doorState(door) -> "OPEN" | "CLOSED" | "NO_SENSOR" | nil
//
// nil means the panel has not reported a status record yet (it only sends them
// when it has no events queued), NOT that the door is shut.
int LuaEngine::Lua_ZkController_DoorState(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int door = static_cast<int>(luaL_checkinteger(L, 1));

  if (!zk) {
    lua_pushnil(L);
    return 1;
  }

  ZkIoState state = zk->IoState();

  if (!state.status_seen || door < 1 || door > static_cast<int>(state.doors.size())) {
    lua_pushnil(L);
    return 1;
  }

  lua_pushstring(L, DoorStateName(state.doors[static_cast<size_t>(door - 1)]));
  return 1;
}

// zk.inputState(input) -> true (shorted) | false (open) | nil (never seen)
int LuaEngine::Lua_ZkController_InputState(lua_State* L) {
  ZkController* zk = ZkForBinding(L);
  int input = static_cast<int>(luaL_checkinteger(L, 1));

  if (!zk) {
    lua_pushnil(L);
    return 1;
  }

  ZkIoState state = zk->IoState();
  auto it = state.aux_inputs.find(input);

  if (it == state.aux_inputs.end()) {
    lua_pushnil(L);
    return 1;
  }

  lua_pushboolean(L, it->second ? 1 : 0);
  return 1;
}

// onCard()/onRawRTLog()/onConnectionChanged() each replace any previously
// registered callback (unref the old one first, so repeated calls don't
// leak registry slots) -- matches the spec's "replace, don't stack" usage
// (one zk.onCard(fn) call per script).
int LuaEngine::Lua_ZkController_OnCard(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (!engine) return 0;
  if (engine->zkCardCallbackRef_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, engine->zkCardCallbackRef_);
  }
  lua_pushvalue(L, 1);
  engine->zkCardCallbackRef_ = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

int LuaEngine::Lua_ZkController_OnRawRTLog(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (!engine) return 0;
  if (engine->zkRawCallbackRef_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, engine->zkRawCallbackRef_);
  }
  lua_pushvalue(L, 1);
  engine->zkRawCallbackRef_ = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

int LuaEngine::Lua_ZkController_OnConnectionChanged(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (!engine) return 0;
  if (engine->zkConnectionCallbackRef_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, engine->zkConnectionCallbackRef_);
  }
  lua_pushvalue(L, 1);
  engine->zkConnectionCallbackRef_ = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

// ---------------------------------------------------------------------------
// Lua bindings
// ---------------------------------------------------------------------------

int LuaEngine::Lua_Rest_SetUrl(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* url = luaL_checkstring(L, 1);
  if (engine && engine->rest_) engine->rest_->SetUrl(url);
  return 0;
}

int LuaEngine::Lua_Rest_SetApiKey(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* key = luaL_checkstring(L, 1);
  if (engine && engine->rest_) engine->rest_->SetApiKey(key);
  return 0;
}

int LuaEngine::Lua_Rest_PostForm(lua_State* L) {
  LuaEngine* engine = GetEngine(L);

  // Rest.PostForm(data) posts to the configured server; Rest.PostForm(path,
  // data) posts to that path joined onto it (e.g. "/api/citizen/verify").
  std::string path;
  int tableIndex = 1;
  if (lua_gettop(L) >= 2 && lua_type(L, 1) == LUA_TSTRING) {
    path = lua_tostring(L, 1);
    tableIndex = 2;
  }
  luaL_checktype(L, tableIndex, LUA_TTABLE);
  auto fields = TableToStringMap(L, tableIndex);

  if (!engine || !engine->rest_) {
    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ok");
    return 1;
  }

  RestResponse response;
  if (path.empty()) {
    response = engine->rest_->PostForm(fields);
  } else {
    RestConfig config = engine->rest_->GetConfig();
    response = engine->rest_->PostForm(JoinUrl(config.url, path), fields);
  }
  PushRestResponseTable(L, response);
  return 1;
}

int LuaEngine::Lua_Rest_Get(lua_State* L) {
  LuaEngine* engine = GetEngine(L);

  // Optional argument may be a full URL (used as-is) or a path joined onto
  // the configured server (e.g. "/api/status"); omitted, uses that server
  // URL directly.
  std::string arg;
  if (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TSTRING) {
    arg = lua_tostring(L, 1);
  }

  if (!engine || !engine->rest_) {
    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ok");
    return 1;
  }

  RestConfig config = engine->rest_->GetConfig();
  std::string url = arg.empty() ? config.url : JoinUrl(config.url, arg);
  RestResponse response = engine->rest_->Get(url);
  PushRestResponseTable(L, response);
  return 1;
}

int LuaEngine::Lua_Serial_Open(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  bool ok = false;
  if (engine && engine->serial_) {
    ok = engine->serial_->Open(ConfigManager::Instance().GetSerial());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Releases the script's claim on the port; the hardware stays open
// (docs/FixSerial.md §10). C++ owns the physical connection -- the gateway is
// its only user, the reader is permanently attached, and a script that closed
// the port between polls was paying a full open/close cycle per iteration for
// nothing. Use Serial.Shutdown() to actually close it.
int LuaEngine::Lua_Serial_Close(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  // Logged ONCE per process, not per call. A script may call this every loop
  // iteration, and a log line costs a file write -- measured at ~5 ms a call,
  // which would make the "free" release the slowest thing in the loop. The note
  // is there to explain the changed semantics once, not to trace traffic.
  static std::atomic<bool> explained{false};
  if (engine && engine->serial_ && !explained.exchange(true)) {
    Logger::Instance().Debug(LogCategory::Serial,
                              "Serial.Close(): claim released, port stays open (Serial.Shutdown() closes it)");
  }
  return 0;
}

int LuaEngine::Lua_Serial_Shutdown(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->serial_) engine->serial_->Close();
  return 0;
}

int LuaEngine::Lua_Serial_IsOpen(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  lua_pushboolean(L, (engine && engine->serial_ && engine->serial_->IsOpen()) ? 1 : 0);
  return 1;
}

// CLOSED | OPEN | ERROR | RECONNECTING (docs/FixSerial.md §23)
int LuaEngine::Lua_Serial_Status(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (!engine || !engine->serial_) {
    lua_pushstring(L, "CLOSED");
    return 1;
  }
  lua_pushstring(L, SerialPort::StateName(engine->serial_->GetState()));
  return 1;
}

int LuaEngine::Lua_Serial_Read(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (!engine || !engine->serial_) {
    lua_pushnil(L);
    return 1;
  }
  // Called every loop iteration by any script that polls Serial.Read() in
  // a `while true do` loop, so this is also where queued events (RFID
  // scans, etc.) get delivered to that kind of script — see QueueEvent's
  // doc comment for why they can't just be dispatched directly from
  // whatever background thread detected them.
  engine->DrainPendingEvents(L);

  // Raw bytes accumulated since the last read, with no line-splitting or
  // other framing — see SerialPort::ReadAvailable(). Uses lua_pushlstring
  // (length-prefixed), not lua_pushstring (null-terminated), so a binary
  // frame with an embedded 0x00 byte isn't truncated either.
  std::string data = engine->serial_->ReadAvailable();
  if (data.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, data.data(), data.size());
  }
  return 1;
}

int LuaEngine::Lua_Serial_Write(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* text = luaL_checkstring(L, 1);
  bool ok = engine && engine->serial_ && engine->serial_->Write(text);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Serial2_Open(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  bool ok = false;
  if (engine && engine->serial2_) {
    ok = engine->serial2_->Open(ConfigManager::Instance().GetSerial2());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Same release-not-close semantics as Serial.Close(); the LED panel is even
// more clearly a permanently attached device (docs/FixSerial.md §25).
int LuaEngine::Lua_Serial2_Close(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  static std::atomic<bool> explained{false};
  if (engine && engine->serial2_ && !explained.exchange(true)) {
    Logger::Instance().Debug(LogCategory::Serial,
                              "Serial2.Close(): claim released, port stays open (Serial2.Shutdown() closes it)");
  }
  return 0;
}

int LuaEngine::Lua_Serial2_Shutdown(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->serial2_) engine->serial2_->Close();
  return 0;
}

int LuaEngine::Lua_Serial2_IsOpen(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  lua_pushboolean(L, (engine && engine->serial2_ && engine->serial2_->IsOpen()) ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Serial2_Status(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (!engine || !engine->serial2_) {
    lua_pushstring(L, "CLOSED");
    return 1;
  }
  lua_pushstring(L, SerialPort::StateName(engine->serial2_->GetState()));
  return 1;
}

int LuaEngine::Lua_Serial2_Read(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (!engine || !engine->serial2_) {
    lua_pushnil(L);
    return 1;
  }
  // No DrainPendingEvents() here, unlike Serial.Read(): that exists so a
  // script polling the *citizen reader* also delivers queued events, and
  // this port is the LED display -- a script polling it isn't the
  // event-delivery path.
  std::string data = engine->serial2_->ReadAvailable();
  if (data.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, data.data(), data.size());
  }
  return 1;
}

int LuaEngine::Lua_Serial2_Write(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  size_t len = 0;
  // lua_tolstring, not luaL_checkstring: LED protocol frames are binary
  // (they start with 0x00 CMD and end with 0x0D), so a null-terminated read
  // would send nothing at all.
  const char* data = luaL_checklstring(L, 1, &len);
  // The LED is status-only: a new status supersedes any unsent old one. Queue
  // it on Serial2's own worker so a stuck COM driver never freezes Lua.
  bool ok = engine && engine->serial2_ && engine->serial2_->WriteLatest(std::string(data, len));
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Tcp_Connect(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* ip = luaL_checkstring(L, 1);
  int port = static_cast<int>(luaL_checkinteger(L, 2));
  bool ok = engine && engine->luaTcp_.Connect(ip, port);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Tcp_Send(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* data = luaL_checkstring(L, 1);
  bool ok = engine && engine->luaTcp_.Send(data);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Tcp_Receive(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  std::string data;
  bool ok = engine && engine->luaTcp_.Receive(data, 4096, 1000);
  if (ok) {
    lua_pushstring(L, data.c_str());
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int LuaEngine::Lua_Tcp_Close(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine) engine->luaTcp_.Close();
  return 0;
}

int LuaEngine::Lua_Rfid_IsConnected(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  bool connected = engine && engine->rfid_ && engine->rfid_->IsConnected();
  lua_pushboolean(L, connected ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Modbus_IsConnected(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  bool connected = engine && engine->modbus_ && engine->modbus_->IsConnected();
  lua_pushboolean(L, connected ? 1 : 0);
  return 1;
}

// Plugin.List() -> { { id=..., name=..., version=..., state=..., enabled=... }, ... }
int LuaEngine::Lua_Plugin_List(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  lua_newtable(L);
  if (!engine || !engine->plugins_) return 1;

  int index = 1;
  for (const PluginRecord& record : engine->plugins_->List()) {
    lua_newtable(L);
    lua_pushstring(L, record.manifest.id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, record.manifest.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, record.manifest.version.c_str());
    lua_setfield(L, -2, "version");
    lua_pushstring(L, PluginStateName(record.state));
    lua_setfield(L, -2, "state");
    lua_pushboolean(L, record.enabled ? 1 : 0);
    lua_setfield(L, -2, "enabled");
    lua_seti(L, -2, index++);
  }
  return 1;
}

// Plugin.Health("hsf.driver.modbus") -> true | nil, error
int LuaEngine::Lua_Plugin_Health(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* id = luaL_checkstring(L, 1);
  if (!engine || !engine->plugins_) {
    lua_pushnil(L);
    lua_pushstring(L, "plugin manager is not available");
    return 2;
  }
  std::string error;
  if (!engine->plugins_->HealthCheck(id, error)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

// Plugin.Read(id, a1, a2 [, a3 [, unit [, encoding]]]) -> value | nil, error
// BLOB values are returned as binary-safe Lua strings.
int LuaEngine::Lua_Plugin_Read(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* id = luaL_checkstring(L, 1);
  if (!engine || !engine->plugins_) {
    lua_pushnil(L);
    lua_pushstring(L, "plugin manager is not available");
    return 2;
  }

  HSFDriverRef driver = engine->plugins_->Driver(id);
  if (!driver.vt || !driver.self || !driver.vt->read) {
    lua_pushnil(L);
    lua_pushfstring(L, "plugin %s has no readable driver", id);
    return 2;
  }

  HSFDeviceAddress address = hsf_device_address_init();
  address.a1 = static_cast<int64_t>(luaL_checkinteger(L, 2));
  address.a2 = static_cast<int64_t>(luaL_optinteger(L, 3, 0));
  address.a3 = static_cast<int64_t>(luaL_optinteger(L, 4, 0));
  address.unit = static_cast<int32_t>(luaL_optinteger(L, 5, 0));
  address.encoding = static_cast<HSFEncoding>(luaL_optinteger(L, 6, HSF_ENC_NONE));

  HSFValue value = hsf_value_null();
  HSFStatus status = HSF_ERR_INTERNAL;
  try {
    status = driver.vt->read(driver.self, &address, &value);
  } catch (...) {
    status = HSF_ERR_INTERNAL;
  }
  if (status < 0) {
    lua_pushnil(L);
    lua_pushstring(L, hsf_status_name(status));
    return 2;
  }

  switch (value.kind) {
    case HSF_VALUE_NULL:
      lua_pushnil(L);
      break;
    case HSF_VALUE_BOOL:
      lua_pushboolean(L, value.as.b ? 1 : 0);
      break;
    case HSF_VALUE_I64:
      lua_pushinteger(L, static_cast<lua_Integer>(value.as.i64));
      break;
    case HSF_VALUE_U64:
      if (value.as.u64 <= static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max())) {
        lua_pushinteger(L, static_cast<lua_Integer>(value.as.u64));
      } else {
        lua_pushnumber(L, static_cast<lua_Number>(value.as.u64));
      }
      break;
    case HSF_VALUE_F64:
      lua_pushnumber(L, static_cast<lua_Number>(value.as.f64));
      break;
    case HSF_VALUE_STR:
      lua_pushlstring(L, value.as.str.ptr ? value.as.str.ptr : "", value.as.str.len);
      break;
    case HSF_VALUE_BLOB:
      lua_pushlstring(L,
                      value.as.blob.ptr ? reinterpret_cast<const char*>(value.as.blob.ptr) : "",
                      value.as.blob.len);
      break;
    default:
      lua_pushnil(L);
      lua_pushstring(L, "plugin returned an invalid value kind");
      return 2;
  }
  return 1;
}

// Plugin.Write(id, a1, a2, value [, a3 [, unit [, encoding]]])
// -> true | nil, error
int LuaEngine::Lua_Plugin_Write(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* id = luaL_checkstring(L, 1);
  if (!engine || !engine->plugins_) {
    lua_pushnil(L);
    lua_pushstring(L, "plugin manager is not available");
    return 2;
  }

  HSFDriverRef driver = engine->plugins_->Driver(id);
  if (!driver.vt || !driver.self || !driver.vt->write) {
    lua_pushnil(L);
    lua_pushfstring(L, "plugin %s has no writable driver", id);
    return 2;
  }

  HSFDeviceAddress address = hsf_device_address_init();
  address.a1 = static_cast<int64_t>(luaL_checkinteger(L, 2));
  address.a2 = static_cast<int64_t>(luaL_optinteger(L, 3, 0));
  address.a3 = static_cast<int64_t>(luaL_optinteger(L, 5, 0));
  address.unit = static_cast<int32_t>(luaL_optinteger(L, 6, 0));
  address.encoding = static_cast<HSFEncoding>(luaL_optinteger(L, 7, HSF_ENC_NONE));

  HSFValue value = hsf_value_null();
  size_t stringLength = 0;
  const char* stringValue = nullptr;
  if (lua_isboolean(L, 4)) {
    value = hsf_value_bool(lua_toboolean(L, 4));
  } else if (lua_isinteger(L, 4)) {
    lua_Integer integerValue = lua_tointeger(L, 4);
    value = integerValue < 0
                ? hsf_value_i64(static_cast<int64_t>(integerValue))
                : hsf_value_u64(static_cast<uint64_t>(integerValue));
  } else if (lua_isnumber(L, 4)) {
    value = hsf_value_f64(static_cast<double>(lua_tonumber(L, 4)));
  } else if (lua_isstring(L, 4)) {
    stringValue = lua_tolstring(L, 4, &stringLength);
    value = hsf_value_blob(reinterpret_cast<const uint8_t*>(stringValue), stringLength);
  } else {
    return luaL_argerror(L, 4, "expected boolean, integer, number, or string");
  }

  HSFStatus status = HSF_ERR_INTERNAL;
  try {
    status = driver.vt->write(driver.self, &address, &value);
  } catch (...) {
    status = HSF_ERR_INTERNAL;
  }
  if (status < 0) {
    lua_pushnil(L);
    lua_pushstring(L, hsf_status_name(status));
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Modbus_CheckConnect(lua_State* L) {
  std::string ip = luaL_checkstring(L, 1);
  int port = static_cast<int>(luaL_checkinteger(L, 2));
  std::string error;
  bool ok = ModbusClient::TestConnect(ip, port, error);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Modbus.ReadCoil(addr) reads from the gateway's configured PLC connection;
// Modbus.ReadCoil(ip, port, addr) opens a one-shot connection to a PLC at an
// explicit address instead, per ModbusClient::ReadCoilOnce.
int LuaEngine::Lua_Modbus_ReadCoil(lua_State* L) {
  if (lua_gettop(L) >= 3) {
    std::string ip = luaL_checkstring(L, 1);
    int port = static_cast<int>(luaL_checkinteger(L, 2));
    int addr = static_cast<int>(luaL_checkinteger(L, 3));
    bool value = false;
    std::string error;
    if (ModbusClient::ReadCoilOnce(ip, port, addr, value, error)) {
      lua_pushboolean(L, value ? 1 : 0);
    } else {
      lua_pushnil(L);
    }
    return 1;
  }

  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  bool value = false;
  if (engine && engine->modbus_ && engine->modbus_->ReadCoil(addr, value)) {
    lua_pushboolean(L, value ? 1 : 0);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// Modbus.WriteCoil(addr, value) writes via the gateway's configured PLC
// connection; Modbus.WriteCoil(ip, port, addr, value) opens a one-shot
// connection to a PLC at an explicit address instead.
int LuaEngine::Lua_Modbus_WriteCoil(lua_State* L) {
  if (lua_gettop(L) >= 4) {
    std::string ip = luaL_checkstring(L, 1);
    int port = static_cast<int>(luaL_checkinteger(L, 2));
    int addr = static_cast<int>(luaL_checkinteger(L, 3));
    bool value = lua_toboolean(L, 4) != 0;
    std::string error;
    bool ok = ModbusClient::WriteCoilOnce(ip, port, addr, value, error);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  bool value = lua_toboolean(L, 2) != 0;
  bool ok = engine && engine->modbus_ && engine->modbus_->WriteCoil(addr, value);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Modbus_ReadCoils(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  int count = static_cast<int>(luaL_checkinteger(L, 2));
  std::vector<bool> values;
  if (!engine || !engine->modbus_ || !engine->modbus_->ReadCoils(addr, count, values)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, static_cast<int>(values.size()), 0);
  for (size_t i = 0; i < values.size(); ++i) {
    lua_pushboolean(L, values[i] ? 1 : 0);
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

int LuaEngine::Lua_Modbus_WriteCoils(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  luaL_checktype(L, 2, LUA_TTABLE);
  auto values = TableToBoolVector(L, 2);
  bool ok = engine && engine->modbus_ && engine->modbus_->WriteCoils(addr, values);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Modbus_ReadHolding(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  uint16_t value = 0;
  if (engine && engine->modbus_ && engine->modbus_->ReadHoldingRegister(addr, value)) {
    lua_pushinteger(L, value);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int LuaEngine::Lua_Modbus_WriteHolding(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  uint16_t value = static_cast<uint16_t>(luaL_checkinteger(L, 2));
  bool ok = engine && engine->modbus_ && engine->modbus_->WriteHoldingRegister(addr, value);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Modbus_ReadHoldingRegisters(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  int count = static_cast<int>(luaL_checkinteger(L, 2));
  std::vector<uint16_t> values;
  if (!engine || !engine->modbus_ || !engine->modbus_->ReadHoldingRegisters(addr, count, values)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, static_cast<int>(values.size()), 0);
  for (size_t i = 0; i < values.size(); ++i) {
    lua_pushinteger(L, values[i]);
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

int LuaEngine::Lua_Modbus_WriteHoldingRegisters(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  luaL_checktype(L, 2, LUA_TTABLE);
  auto values = TableToU16Vector(L, 2);
  bool ok = engine && engine->modbus_ && engine->modbus_->WriteHoldingRegisters(addr, values);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Modbus.ReadDiscreteInput(addr) reads via the gateway's configured PLC
// connection; Modbus.ReadDiscreteInput(ip, port, addr) opens a one-shot
// connection instead -- same two shapes as Modbus.ReadCoil.
//
// Function code 0x02, not 0x01: discrete inputs are a separate read-only
// address space, and PLCs that map physical inputs there do not answer coil
// reads for those addresses.
int LuaEngine::Lua_Modbus_ReadDiscreteInput(lua_State* L) {
  if (lua_gettop(L) >= 3) {
    std::string ip = luaL_checkstring(L, 1);
    int port = static_cast<int>(luaL_checkinteger(L, 2));
    int addr = static_cast<int>(luaL_checkinteger(L, 3));
    bool value = false;
    std::string error;
    if (ModbusClient::ReadDiscreteInputOnce(ip, port, addr, value, error)) {
      lua_pushboolean(L, value ? 1 : 0);
    } else {
      lua_pushnil(L);
    }
    return 1;
  }

  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  bool value = false;
  if (engine && engine->modbus_ && engine->modbus_->ReadDiscreteInput(addr, value)) {
    lua_pushboolean(L, value ? 1 : 0);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int LuaEngine::Lua_Modbus_ReadDiscreteInputs(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  int count = static_cast<int>(luaL_checkinteger(L, 2));
  std::vector<bool> values;
  if (!engine || !engine->modbus_ || !engine->modbus_->ReadDiscreteInputs(addr, count, values)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, static_cast<int>(values.size()), 0);
  for (size_t i = 0; i < values.size(); ++i) {
    lua_pushboolean(L, values[i] ? 1 : 0);
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

int LuaEngine::Lua_Modbus_ReadInputRegister(lua_State* L) {
  if (lua_gettop(L) >= 3) {
    std::string ip = luaL_checkstring(L, 1);
    int port = static_cast<int>(luaL_checkinteger(L, 2));
    int addr = static_cast<int>(luaL_checkinteger(L, 3));
    std::vector<uint16_t> values;
    std::string error;
    if (ModbusClient::ReadInputRegistersOnce(ip, port, addr, 1, values, error) && !values.empty()) {
      lua_pushinteger(L, values[0]);
    } else {
      lua_pushnil(L);
    }
    return 1;
  }

  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  uint16_t value = 0;
  if (engine && engine->modbus_ && engine->modbus_->ReadInputRegister(addr, value)) {
    lua_pushinteger(L, value);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int LuaEngine::Lua_Modbus_ReadInputRegisters(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int addr = static_cast<int>(luaL_checkinteger(L, 1));
  int count = static_cast<int>(luaL_checkinteger(L, 2));
  std::vector<uint16_t> values;
  if (!engine || !engine->modbus_ || !engine->modbus_->ReadInputRegisters(addr, count, values)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, static_cast<int>(values.size()), 0);
  for (size_t i = 0; i < values.size(); ++i) {
    lua_pushinteger(L, values[i]);
    lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

namespace {

// Reads an optional string field from the options table at `index`.
std::string OptString(lua_State* L, int index, const char* key, const std::string& fallback) {
  lua_getfield(L, index, key);
  std::string value = fallback;
  if (lua_isstring(L, -1)) value = lua_tostring(L, -1);
  lua_pop(L, 1);
  return value;
}

int OptInt(lua_State* L, int index, const char* key, int fallback) {
  lua_getfield(L, index, key);
  int value = fallback;
  if (lua_isnumber(L, -1)) value = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  return value;
}

bool OptBool(lua_State* L, int index, const char* key, bool fallback) {
  lua_getfield(L, index, key);
  bool value = fallback;
  if (lua_isboolean(L, -1)) value = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return value;
}

// Lua value -> JSON, for the typed register encoder. Only the scalar kinds a
// register can hold are accepted.
bool LuaValueToJson(lua_State* L, int index, nlohmann::json& out, std::string& error) {
  if (lua_isstring(L, index) && !lua_isnumber(L, index)) {
    out = std::string(lua_tostring(L, index));
    return true;
  }
  if (lua_isnumber(L, index)) {
    if (lua_isinteger(L, index)) {
      out = static_cast<long long>(lua_tointeger(L, index));
    } else {
      out = static_cast<double>(lua_tonumber(L, index));
    }
    return true;
  }
  if (lua_isboolean(L, index)) {
    out = lua_toboolean(L, index) ? 1 : 0;
    return true;
  }
  error = "value must be a number or string";
  return false;
}

void PushJsonValue(lua_State* L, const nlohmann::json& value) {
  if (value.is_string()) {
    lua_pushstring(L, value.get<std::string>().c_str());
  } else if (value.is_number_integer()) {
    lua_pushinteger(L, static_cast<lua_Integer>(value.get<long long>()));
  } else if (value.is_number_unsigned()) {
    lua_pushinteger(L, static_cast<lua_Integer>(value.get<unsigned long long>()));
  } else if (value.is_number_float()) {
    lua_pushnumber(L, value.get<double>());
  } else {
    lua_pushnil(L);
  }
}

}  // namespace

// Modbus.RegisterRegister(name, address, opts) -- declares a named, typed
// value living in a run of 16-bit registers, for the Dashboard's PLC
// Registers card.
//
// opts = {
//   type    = "uint16"|"int16"|"uint32"|"int32"|"uint64"|"int64"
//             |"float32"|"float64"|"string",   -- default "uint16"
//   endian  = "ABCD"|"BADC"|"CDAB"|"DCBA" (or "big"/"little"), default "ABCD"
//   length  = <bytes>,        -- string only
//   source  = "holding"|"input",  -- FC03 (default) or FC04
//   write   = true|false,     -- holding registers only
//   unit    = "degC",         -- display only
// }
int LuaEngine::Lua_Modbus_RegisterRegister(lua_State* L) {
  ModbusRegisterPoint point;
  point.name = luaL_checkstring(L, 1);
  point.address = static_cast<int>(luaL_checkinteger(L, 2));

  std::string typeText = "uint16";
  std::string endianText = "ABCD";
  std::string sourceText = "holding";
  bool wantWrite = false;

  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    typeText = OptString(L, 3, "type", typeText);
    endianText = OptString(L, 3, "endian", endianText);
    sourceText = OptString(L, 3, "source", sourceText);
    point.format.length = OptInt(L, 3, "length", 0);
    point.unit = OptString(L, 3, "unit", "");
    wantWrite = OptBool(L, 3, "write", false);
  }

  if (!ParseRegisterType(typeText, point.format.type)) {
    return luaL_error(L, "Modbus.RegisterRegister: unknown type \"%s\"", typeText.c_str());
  }
  if (!ParseEndian(endianText, point.format.byte_swap, point.format.word_swap)) {
    return luaL_error(L, "Modbus.RegisterRegister: unknown endian \"%s\" (use ABCD/BADC/CDAB/DCBA)",
                      endianText.c_str());
  }

  if (sourceText == "input" || sourceText == "input_register" || sourceText == "fc04" ||
      sourceText == "FC04" || sourceText == "4") {
    point.input_register = true;
  } else if (sourceText == "holding" || sourceText == "holding_register" || sourceText == "fc03" ||
             sourceText == "FC03" || sourceText == "3") {
    point.input_register = false;
  } else {
    return luaL_error(L, "Modbus.RegisterRegister: unknown source \"%s\" (use \"holding\" or \"input\")",
                      sourceText.c_str());
  }

  if (point.format.type == RegisterType::kString && point.format.length <= 0) {
    return luaL_error(L, "Modbus.RegisterRegister: string type needs a length (in bytes)");
  }

  // Input registers are read-only in the protocol itself -- there is no FC04
  // write frame. Refuse rather than accept a flag the gateway could never
  // honour, so the failure surfaces at registration instead of at the first
  // attempted write.
  if (wantWrite && point.input_register) {
    return luaL_error(L,
                      "Modbus.RegisterRegister: \"%s\" is an input register (FC04), which is read-only in "
                      "Modbus -- use source=\"holding\" (FC03) for a writable value",
                      point.name.c_str());
  }
  point.writable = wantWrite;

  LuaEngine* engine = GetEngine(L);
  point.owner = engine ? engine->RuntimeId() : 0;

  ModbusRegistry::Instance().RegisterRegister(point);
  return 0;
}

// Modbus.GetRegister(name) -> value, error
// Returns the last polled value for a registered register point.
int LuaEngine::Lua_Modbus_GetRegister(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  ModbusRegisterPoint point;
  if (!ModbusRegistry::Instance().FindRegister(name, point)) {
    lua_pushnil(L);
    lua_pushstring(L, "no registered register named that");
    return 2;
  }
  if (!point.valid) {
    lua_pushnil(L);
    lua_pushstring(L, point.error.empty() ? "not read yet" : point.error.c_str());
    return 2;
  }
  PushJsonValue(L, point.value);
  return 1;
}

// Modbus.SetRegister(name, value) -> ok, error
// Encodes `value` per the point's declared type/endianness and writes it.
int LuaEngine::Lua_Modbus_SetRegister(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* name = luaL_checkstring(L, 1);

  ModbusRegisterPoint point;
  if (!ModbusRegistry::Instance().FindRegister(name, point)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no registered register named that");
    return 2;
  }
  if (!point.writable) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, point.input_register
                          ? "input registers (FC04) are read-only in Modbus"
                          : "register was not declared writable (pass write=true)");
    return 2;
  }

  nlohmann::json value;
  std::string error;
  if (!LuaValueToJson(L, 2, value, error)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, error.c_str());
    return 2;
  }

  std::vector<uint16_t> words;
  if (!EncodeRegisters(point.format, value, words, error)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, error.c_str());
    return 2;
  }

  if (!engine || !engine->modbus_) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no PLC connection");
    return 2;
  }

  // FC06 for a lone register, FC16 for a run -- some devices implement only
  // one of the two, and a single-register FC16 is the less widely accepted.
  bool ok = words.size() == 1 ? engine->modbus_->WriteHoldingRegister(point.address, words[0])
                              : engine->modbus_->WriteHoldingRegisters(point.address, words);
  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) return 1;
  lua_pushstring(L, "write failed");
  return 2;
}

// Modbus.RegisterInput(name, address [, source]) /
// Modbus.RegisterOutput(name, address) -- attaches a label to a coil so the
// dashboard can show named PLC I/O instead of bare addresses. Registration
// only describes the point; the gateway's poll thread does the reading (see
// main.cpp).
//
// `source` is "auto" (default), "discrete"/"fc02", or "coil"/"fc01". It
// exists because coils and discrete inputs are separate address spaces and
// some PLCs answer both at one address while only one holds the live value --
// in that case "auto" cannot tell them apart and the script must say which.
int LuaEngine::Lua_Modbus_RegisterInput(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  int address = static_cast<int>(luaL_checkinteger(L, 2));

  ModbusSource source = ModbusSource::kAuto;
  if (!lua_isnoneornil(L, 3)) {
    const char* text = luaL_checkstring(L, 3);
    // Reject a typo loudly rather than silently polling the wrong space --
    // that failure looks like "the input never changes", which is exactly
    // the bug this argument exists to fix.
    std::string requested(text);
    source = ParseModbusSource(requested, ModbusSource::kAuto);
    if (source == ModbusSource::kAuto && requested != "auto") {
      return luaL_error(L, "Modbus.RegisterInput: unknown source \"%s\" (use \"auto\", \"discrete\" or \"coil\")",
                        text);
    }
  }

  LuaEngine* engine = GetEngine(L);
  ModbusRegistry::Instance().RegisterInput(name, address, source, engine ? engine->RuntimeId() : 0);
  return 0;
}

int LuaEngine::Lua_Modbus_RegisterOutput(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  int address = static_cast<int>(luaL_checkinteger(L, 2));
  LuaEngine* engine = GetEngine(L);
  ModbusRegistry::Instance().RegisterOutput(name, address, engine ? engine->RuntimeId() : 0);
  return 0;
}

// Modbus.RegisterDiscreteInput(name, address) / RegisterCoilInput(name,
// address) -- request/upgrade.md section 19. Identical to RegisterInput with
// an explicit `source`, and preferred over it: naming the address space at the
// call site is what stops a discrete input being polled as a coil (section
// 17's "Warning: Poll ReadCoil"), and unlike "auto" it cannot resolve to the
// wrong space on a PLC that answers both function codes at one address.
int LuaEngine::Lua_Modbus_RegisterDiscreteInput(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  int address = static_cast<int>(luaL_checkinteger(L, 2));
  LuaEngine* engine = GetEngine(L);
  ModbusRegistry::Instance().RegisterInput(name, address, ModbusSource::kDiscreteInput,
                                            engine ? engine->RuntimeId() : 0);
  return 0;
}

int LuaEngine::Lua_Modbus_RegisterCoilInput(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  int address = static_cast<int>(luaL_checkinteger(L, 2));
  LuaEngine* engine = GetEngine(L);
  ModbusRegistry::Instance().RegisterInput(name, address, ModbusSource::kCoil,
                                            engine ? engine->RuntimeId() : 0);
  return 0;
}

int LuaEngine::Lua_Card_Get(lua_State* L) {
  auto entry = CardCache::Instance().Get();
  if (!entry) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, entry->uid.c_str());
  lua_setfield(L, -2, "uid");
  lua_pushinteger(L, entry->position);
  lua_setfield(L, -2, "position");
  lua_pushinteger(L, entry->timestamp);
  lua_setfield(L, -2, "timestamp");
  return 1;
}

int LuaEngine::Lua_Card_Clear(lua_State*) {
  CardCache::Instance().Clear();
  return 0;
}

int LuaEngine::Lua_Card_Available(lua_State* L) {
  lua_pushboolean(L, CardCache::Instance().Available() ? 1 : 0);
  return 1;
}

// --- Mq.* (RabbitMQ) -------------------------------------------------------
//
// Every one of these has to cope with mq_ being null: LuaEngine can legitimately
// be constructed without a broker client (Validate() builds a throwaway state,
// and a future embedder need not wire one), and a script must get a usable
// `false, "reason"` rather than a crash.

namespace {

// Shared by the two publish bindings. `wrapEnvelope` is what separates them:
// Mq.Publish honours the configured envelope setting, Mq.PublishRaw never wraps.
int PublishFromLua(lua_State* L, MqClient* mq, bool allowEnvelope) {
  const char* body = luaL_checkstring(L, 1);
  const std::string routingKey = luaL_optstring(L, 2, "");
  const std::string exchange = luaL_optstring(L, 3, "");

  if (!mq) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "RabbitMQ is not available in this runtime");
    return 2;
  }

  const bool wrap = allowEnvelope && mq->GetConfig().envelope;

  std::string error;
  const bool ok = mq->Publish(body, routingKey, exchange, wrap, error);
  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) return 1;
  lua_pushstring(L, error.c_str());
  return 2;
}

void PushMqMessage(lua_State* L, const MqMessage& message) {
  lua_newtable(L);
  lua_pushstring(L, message.body.c_str());
  lua_setfield(L, -2, "body");
  // The payload exactly as it arrived, envelope and all -- for a message this
  // gateway's envelope rules don't model.
  lua_pushstring(L, message.raw.c_str());
  lua_setfield(L, -2, "raw");
  lua_pushstring(L, message.id.c_str());
  lua_setfield(L, -2, "id");
  lua_pushstring(L, message.timestamp.c_str());
  lua_setfield(L, -2, "ts");
  lua_pushinteger(L, message.version);
  lua_setfield(L, -2, "v");
  lua_pushstring(L, message.exchange.c_str());
  lua_setfield(L, -2, "exchange");
  lua_pushstring(L, message.routing_key.c_str());
  lua_setfield(L, -2, "routing_key");
  lua_pushboolean(L, message.redelivered ? 1 : 0);
  lua_setfield(L, -2, "redelivered");
  lua_pushinteger(L, static_cast<lua_Integer>(message.delivery_tag));
  lua_setfield(L, -2, "delivery_tag");
}

}  // namespace

int LuaEngine::Lua_Mq_IsConnected(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  lua_pushboolean(L, (engine && engine->mq_ && engine->mq_->IsConnected()) ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Mq_Status(lua_State* L) {
  LuaEngine* engine = GetEngine(L);

  lua_newtable(L);
  if (!engine || !engine->mq_) {
    // Same shape as a real status, so a script can read .connected without
    // testing for nil first.
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "connected");
    lua_pushstring(L, "RabbitMQ is not available in this runtime");
    lua_setfield(L, -2, "error");
    return 1;
  }

  const MqStatus status = engine->mq_->Status();
  const MqConfig config = engine->mq_->GetConfig();

  lua_pushboolean(L, status.enabled ? 1 : 0);
  lua_setfield(L, -2, "enabled");
  lua_pushboolean(L, status.connected ? 1 : 0);
  lua_setfield(L, -2, "connected");
  lua_pushboolean(L, status.consuming ? 1 : 0);
  lua_setfield(L, -2, "consuming");
  lua_pushboolean(L, status.paused ? 1 : 0);
  lua_setfield(L, -2, "paused");
  lua_pushstring(L, status.error.c_str());
  lua_setfield(L, -2, "error");
  lua_pushinteger(L, static_cast<lua_Integer>(status.published));
  lua_setfield(L, -2, "published");
  lua_pushinteger(L, static_cast<lua_Integer>(status.received));
  lua_setfield(L, -2, "received");
  lua_pushinteger(L, static_cast<lua_Integer>(status.rejected));
  lua_setfield(L, -2, "rejected");
  lua_pushinteger(L, static_cast<lua_Integer>(status.dropped));
  lua_setfield(L, -2, "dropped");
  lua_pushinteger(L, static_cast<lua_Integer>(status.overflowed));
  lua_setfield(L, -2, "overflowed");
  lua_pushinteger(L, static_cast<lua_Integer>(status.pending));
  lua_setfield(L, -2, "pending");
  lua_pushinteger(L, static_cast<lua_Integer>(status.available));
  lua_setfield(L, -2, "available");

  // Where it is pointed, so a script can log or display the endpoint without a
  // second Config.Get() round trip. No password, deliberately.
  lua_pushstring(L, config.host.c_str());
  lua_setfield(L, -2, "host");
  lua_pushinteger(L, config.port);
  lua_setfield(L, -2, "port");
  lua_pushstring(L, config.vhost.c_str());
  lua_setfield(L, -2, "vhost");
  lua_pushstring(L, config.exchange.c_str());
  lua_setfield(L, -2, "exchange");
  lua_pushstring(L, config.queue.c_str());
  lua_setfield(L, -2, "queue");
  lua_pushstring(L, config.routing_key.c_str());
  lua_setfield(L, -2, "routing_key");
  return 1;
}

// Mq.Publish(body [, routingKey [, exchange]]) -> ok [, error]
int LuaEngine::Lua_Mq_Publish(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  return PublishFromLua(L, engine ? engine->mq_ : nullptr, true);
}

// Mq.PublishRaw(body [, routingKey [, exchange]]) -> ok [, error]
int LuaEngine::Lua_Mq_PublishRaw(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  return PublishFromLua(L, engine ? engine->mq_ : nullptr, false);
}

// Mq.Flush([timeoutMs]) -> ok. False means messages are still queued.
int LuaEngine::Lua_Mq_Flush(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const int timeoutMs = static_cast<int>(luaL_optinteger(L, 1, 2000));
  lua_pushboolean(L, (engine && engine->mq_ && engine->mq_->Flush(timeoutMs)) ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Mq_Available(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  lua_pushboolean(L, (engine && engine->mq_ && engine->mq_->Available()) ? 1 : 0);
  return 1;
}

// Mq.Get() -> table | nil. Removes the message from the inbox; the broker was
// already acked when it arrived (see MqClient), so this cannot fail after the
// fact and a script that drops the returned value simply loses it.
int LuaEngine::Lua_Mq_Get(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  MqMessage message;
  if (!engine || !engine->mq_ || !engine->mq_->Get(message)) {
    lua_pushnil(L);
    return 1;
  }
  PushMqMessage(L, message);
  return 1;
}

int LuaEngine::Lua_Mq_Clear(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->mq_) engine->mq_->Clear();
  return 0;
}

// Mq.Pause(true) cancels the consumer, Mq.Pause(false) restarts it. Publishing
// is unaffected.
int LuaEngine::Lua_Mq_Pause(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  luaL_checktype(L, 1, LUA_TBOOLEAN);
  const bool paused = lua_toboolean(L, 1) != 0;
  if (!engine || !engine->mq_) {
    lua_pushboolean(L, 0);
    return 1;
  }
  engine->mq_->Pause(paused);
  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_SetVariable(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  int type = lua_type(L, 2);

  switch (type) {
    case LUA_TBOOLEAN:
      RuntimeVariables::Instance().Set(name, static_cast<bool>(lua_toboolean(L, 2)));
      break;
    case LUA_TNUMBER:
      if (lua_isinteger(L, 2)) {
        RuntimeVariables::Instance().Set(name, static_cast<int64_t>(lua_tointeger(L, 2)));
      } else {
        RuntimeVariables::Instance().Set(name, lua_tonumber(L, 2));
      }
      break;
    case LUA_TSTRING:
      RuntimeVariables::Instance().Set(name, std::string(lua_tostring(L, 2)));
      break;
    default:
      return luaL_error(L, "SetVariable: unsupported value type '%s'", lua_typename(L, type));
  }
  return 0;
}

int LuaEngine::Lua_GetVariable(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  auto value = RuntimeVariables::Instance().Get(name);
  if (!value) {
    lua_pushnil(L);
    return 1;
  }
  std::visit(
      [L](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          lua_pushboolean(L, v ? 1 : 0);
        } else if constexpr (std::is_same_v<T, int64_t>) {
          lua_pushinteger(L, v);
        } else if constexpr (std::is_same_v<T, double>) {
          lua_pushnumber(L, v);
        } else {
          lua_pushstring(L, v.c_str());
        }
      },
      *value);
  return 1;
}

int LuaEngine::Lua_Sleep(lua_State* L) {
  double milliseconds = luaL_checknumber(L, 1);
  LuaEngine* engine = GetEngine(L);

  // Also called every loop iteration in most polling-style scripts —
  // deliver any queued events (see QueueEvent) before sleeping.
  if (engine) {
    engine->DrainPendingEvents(L);

    // Second sampling point for the System Info metrics. The debug hook
    // fires per VM *instruction*, which a `while true do Sleep(200) end`
    // script barely accumulates -- it would take ~20 minutes to reach the
    // hook's threshold, so memory and core stayed unreported for exactly
    // the long-lived polling scripts most worth watching. Sleep() is where
    // those spend their time, so sample here too.
    engine->SampleRuntimeMetrics(L);
  }

  // Slept in small increments rather than one long sleep_for(): the debug
  // hook that lets Stop() interrupt a stuck script only fires between Lua
  // VM instructions, so it can never preempt a single long C-side sleep. A
  // script that loops "read, Sleep(long), read, ..." would otherwise make
  // Stop() take up to the full sleep duration (or several, if the loop body
  // between sleeps doesn't rack up enough instructions to trip the hook)
  // instead of a bounded interruption latency.
  constexpr double kStepMs = 20.0;
  double remaining = milliseconds;
  while (remaining > 0) {
    double step = remaining < kStepMs ? remaining : kStepMs;
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(step));
    remaining -= step;
    if (engine && engine->interruptRequested_.load()) {
      luaL_error(L, "script interrupted by Stop()");
    }
  }
  return 0;
}

int LuaEngine::Lua_Log_Info(lua_State* L) {
  Logger::Instance().Info(LogCategory::Lua, luaL_checkstring(L, 1));
  return 0;
}
int LuaEngine::Lua_Log_Warning(lua_State* L) {
  Logger::Instance().Warning(LogCategory::Lua, luaL_checkstring(L, 1));
  return 0;
}
int LuaEngine::Lua_Log_Error(lua_State* L) {
  Logger::Instance().Error(LogCategory::Lua, luaL_checkstring(L, 1));
  return 0;
}
int LuaEngine::Lua_Log_Debug(lua_State* L) {
  Logger::Instance().Debug(LogCategory::Lua, luaL_checkstring(L, 1));
  return 0;
}

// Log.Write(log_type, data [, level]) -> ok, error
//
// request/upgrade.md section 10. `data` is a flat table of field = value; the
// C++ side validates it against the log type's definition before inserting
// (section 11), so an unknown type or a misspelled field comes back here as
// an error rather than producing a row nothing can display.
//
// Returns (true) on success and (false, message) on failure -- two values, so
// `local ok, err = Log.Write(...)` works and a script that ignores the result
// still doesn't blow up.
int LuaEngine::Lua_Log_Write(lua_State* L) {
  const char* logType = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  const char* level = luaL_optstring(L, 3, "INFO");

  // Values are stringified here (lua_tostring on a copy, never on the live
  // key -- lua_tostring COERCES a number in place, which would corrupt the
  // key lua_next is using to walk the table and can silently end the
  // traversal early).
  std::map<std::string, std::string> fields;
  lua_pushnil(L);
  while (lua_next(L, 2) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      std::string key = lua_tostring(L, -2);
      std::string value;
      int valueType = lua_type(L, -1);
      if (valueType == LUA_TBOOLEAN) {
        value = lua_toboolean(L, -1) ? "true" : "false";
      } else if (valueType == LUA_TNIL) {
        value = "";
      } else {
        lua_pushvalue(L, -1);
        const char* text = lua_tostring(L, -1);
        value = text ? text : "";
        lua_pop(L, 1);
      }
      fields[key] = value;
    }
    lua_pop(L, 1);
  }

  LuaEngine* engine = GetEngine(L);
  std::string scriptName = engine ? engine->RuntimeName() : "";
  if (scriptName.empty()) scriptName = "(editor buffer)";

  std::string error;
  bool ok = LogStore::Instance().Write(logType, scriptName, level, fields, error);
  if (!ok) {
    // Also surfaced on the runtime log: a script whose Log.Write calls are all
    // being rejected (a renamed field, say) would otherwise look like it was
    // logging fine while the Logs page stayed empty.
    Logger::Instance().Warning(LogCategory::Lua, std::string("Log.Write(\"") + logType + "\") rejected: " + error);
  }

  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) return 1;
  lua_pushstring(L, error.c_str());
  return 2;
}

// Log.Types() -> { { type=, description=, fields={ {name=, type=, required=} } } }
//
// Lets a script check what it may write instead of discovering the answer from
// a rejected Log.Write.
int LuaEngine::Lua_Log_Types(lua_State* L) {
  auto definitions = LogStore::Instance().Definitions();
  lua_newtable(L);
  int index = 1;
  for (const auto& def : definitions) {
    lua_newtable(L);
    lua_pushstring(L, def.type.c_str());
    lua_setfield(L, -2, "type");
    lua_pushstring(L, def.description.c_str());
    lua_setfield(L, -2, "description");

    lua_newtable(L);
    int fieldIndex = 1;
    for (const auto& field : def.fields) {
      lua_newtable(L);
      lua_pushstring(L, field.name.c_str());
      lua_setfield(L, -2, "name");
      lua_pushstring(L, field.type.c_str());
      lua_setfield(L, -2, "type");
      lua_pushboolean(L, field.required ? 1 : 0);
      lua_setfield(L, -2, "required");
      lua_seti(L, -2, fieldIndex++);
    }
    lua_setfield(L, -2, "fields");

    lua_seti(L, -2, index++);
  }
  return 1;
}

namespace {

// Pushes a configuration value as the closest Lua type. Distinct from
// PushJsonValue above (which serves Modbus.GetRegister, where a value is
// always a number or a string): a configuration document also carries
// booleans, and GetCategory hands back a whole section, so this one recurses
// into objects and arrays as tables.
void PushConfigValue(lua_State* L, const nlohmann::json& value) {
  if (value.is_boolean()) {
    lua_pushboolean(L, value.get<bool>() ? 1 : 0);
  } else if (value.is_number_integer()) {
    lua_pushinteger(L, value.get<int64_t>());
  } else if (value.is_number_float()) {
    lua_pushnumber(L, value.get<double>());
  } else if (value.is_string()) {
    lua_pushstring(L, value.get<std::string>().c_str());
  } else if (value.is_null()) {
    lua_pushnil(L);
  } else if (value.is_object()) {
    lua_newtable(L);
    for (const auto& item : value.items()) {
      PushConfigValue(L, item.value());
      lua_setfield(L, -2, item.key().c_str());
    }
  } else if (value.is_array()) {
    lua_newtable(L);
    int index = 1;
    for (const auto& item : value) {
      PushConfigValue(L, item);
      lua_seti(L, -2, index++);
    }
  } else {
    lua_pushstring(L, value.dump().c_str());
  }
}

nlohmann::json LuaValueToJson(lua_State* L, int index) {
  index = lua_absindex(L, index);
  switch (lua_type(L, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return static_cast<bool>(lua_toboolean(L, index));
    case LUA_TNUMBER: return lua_isinteger(L, index) ? nlohmann::json(static_cast<int64_t>(lua_tointeger(L, index)))
                                                       : nlohmann::json(lua_tonumber(L, index));
    case LUA_TSTRING: return std::string(lua_tostring(L, index));
    case LUA_TTABLE: {
      bool array = true;
      lua_Integer max = 0;
      lua_pushnil(L);
      while (lua_next(L, index) != 0) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1) array = false;
        else max = std::max(max, lua_tointeger(L, -2));
        lua_pop(L, 1);
      }
      if (array) {
        for (lua_Integer i = 1; i <= max; ++i) {
          lua_geti(L, index, i);
          if (lua_isnil(L, -1)) { array = false; lua_pop(L, 1); break; }
          lua_pop(L, 1);
        }
      }
      nlohmann::json result = array ? nlohmann::json::array() : nlohmann::json::object();
      if (array) {
        for (lua_Integer i = 1; i <= max; ++i) {
          lua_geti(L, index, i);
          result.push_back(LuaValueToJson(L, -1));
          lua_pop(L, 1);
        }
      } else {
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
          if (lua_isstring(L, -2)) result[lua_tostring(L, -2)] = LuaValueToJson(L, -1);
          lua_pop(L, 1);
        }
      }
      return result;
    }
    default: return nullptr;
  }
}

std::string DynamicProjectId(LuaEngine* engine) {
  if (!engine) return "default";
  const std::string name = engine->RuntimeName();
  if (name.empty()) return "default";
  std::filesystem::path path(name);
  if (path.has_parent_path()) return path.parent_path().generic_string();
  return path.stem().string();
}

}  // namespace

// Config.Get("modbus.ip") -> value | nil
//
// request/upgrade.md sections 5-6. The key names the same setting the
// Configuration page and PATCH /api/config use, so a script and the web UI
// can't drift apart on what a field is called.
int LuaEngine::Lua_Config_Get(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  nlohmann::json value;
  if (!ConfigManager::Instance().GetValue(key, value)) {
    // Second return value is the reason, for the "did I typo the key or is it
    // genuinely unset?" question -- nil alone cannot answer it.
    lua_pushnil(L);
    lua_pushstring(L, "no such configuration key");
    return 2;
  }
  PushConfigValue(L, value);
  return 1;
}

// Config.Set("modbus.ip", "192.168.1.12") -> ok, error
//
// Persists immediately (the whole configuration is written back to SQLite),
// so a script's change survives a restart. Rejects unknown keys and type
// changes rather than accepting them and dropping them silently.
int LuaEngine::Lua_Config_Set(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);

  nlohmann::json value;
  switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
      value = static_cast<bool>(lua_toboolean(L, 2));
      break;
    case LUA_TNUMBER:
      if (lua_isinteger(L, 2)) {
        value = static_cast<int64_t>(lua_tointeger(L, 2));
      } else {
        value = lua_tonumber(L, 2);
      }
      break;
    case LUA_TSTRING:
      value = std::string(lua_tostring(L, 2));
      break;
    default:
      return luaL_error(L, "Config.Set: unsupported value type '%s'", lua_typename(L, lua_type(L, 2)));
  }

  std::string error;
  bool ok = ConfigManager::Instance().SetValue(key, value, error);
  if (ok) {
    Logger::Instance().Info(LogCategory::System, std::string("Config.Set(\"") + key + "\") from a Lua script");
  }
  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) return 1;
  lua_pushstring(L, error.c_str());
  return 2;
}

int LuaEngine::Lua_Config_Exists(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  lua_pushboolean(L, ConfigManager::Instance().HasValue(key) ? 1 : 0);
  return 1;
}

// Config.GetCategory("modbus") -> table | nil
int LuaEngine::Lua_Config_GetCategory(lua_State* L) {
  const char* category = luaL_checkstring(L, 1);
  nlohmann::json section = ConfigManager::Instance().GetCategory(category);
  if (section.is_null()) {
    lua_pushnil(L);
    lua_pushstring(L, "no such configuration category");
    return 2;
  }
  PushConfigValue(L, section);
  return 1;
}

int LuaEngine::Lua_DynamicConfig_RegisterSchema(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  int schemaIndex = 1;
  std::string project = DynamicProjectId(engine);
  if (lua_isstring(L, 1) && lua_istable(L, 2)) {
    project = lua_tostring(L, 1);
    schemaIndex = 2;
  }
  if (!lua_istable(L, schemaIndex)) return luaL_error(L, "config.register_schema expects a table");
  std::string error;
  bool ok = DynamicConfigManager::Instance().RegisterSchema(project, LuaValueToJson(L, schemaIndex), error);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) { lua_pushstring(L, error.c_str()); return 2; }
  return 1;
}

int LuaEngine::Lua_DynamicConfig_Extend(lua_State* L) {
  if (!lua_istable(L, 1) || lua_gettop(L) < 2)
    return luaL_error(L, "config.extend expects a schema and at least one fragment");
  nlohmann::json schema = LuaValueToJson(L, 1);
  if (!schema.is_object()) return luaL_error(L, "config.extend schema must be an object");
  if (!schema.contains("groups") || !schema["groups"].is_array()) schema["groups"] = nlohmann::json::array();
  for (int i = 2; i <= lua_gettop(L); ++i) {
    if (!lua_istable(L, i)) return luaL_error(L, "config.extend fragments must be tables");
    nlohmann::json fragment = LuaValueToJson(L, i);
    if (!fragment.is_object()) return luaL_error(L, "config.extend fragment must be an object");
    if (fragment.contains("groups") && fragment["groups"].is_array())
      for (const auto& group : fragment["groups"]) schema["groups"].push_back(group);
    else if (fragment.contains("fields") && fragment["fields"].is_array())
      for (const auto& field : fragment["fields"]) schema["fields"].push_back(field);
    else schema["groups"].push_back(fragment);
  }
  PushConfigValue(L, schema);
  return 1;
}

int LuaEngine::Lua_DynamicConfig_Get(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  const auto project = DynamicProjectId(GetEngine(L));
  const auto values = DynamicConfigManager::Instance().Values(project, false);
  auto it = values.find(key);
  if (it == values.end()) { lua_pushnil(L); return 1; }
  PushConfigValue(L, *it); return 1;
}

int LuaEngine::Lua_DynamicConfig_Set(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  std::string error;
  nlohmann::json errors;
  bool ok = DynamicConfigManager::Instance().SetValue(DynamicProjectId(GetEngine(L)), key, LuaValueToJson(L, 2), errors);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) { error = errors.dump(); lua_pushstring(L, error.c_str()); return 2; }
  return 1;
}

int LuaEngine::Lua_DynamicConfig_GetAll(lua_State* L) {
  PushConfigValue(L, DynamicConfigManager::Instance().Values(DynamicProjectId(GetEngine(L)), false)); return 1;
}

int LuaEngine::Lua_DynamicConfig_SetAll(lua_State* L) {
  if (!lua_istable(L, 1)) return luaL_error(L, "config.set_all expects a table");
  nlohmann::json errors;
  bool ok = DynamicConfigManager::Instance().SetValues(DynamicProjectId(GetEngine(L)), LuaValueToJson(L, 1), errors);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) { lua_pushstring(L, errors.dump().c_str()); return 2; }
  return 1;
}

int LuaEngine::Lua_DynamicConfig_GetSchema(lua_State* L) {
  PushConfigValue(L, DynamicConfigManager::Instance().Schema(DynamicProjectId(GetEngine(L)))); return 1;
}

// --- Db.* (SQLite) ---------------------------------------------------------

namespace {

// Resolves a database path the way the rest of the gateway resolves its data
// files: absolute stays absolute, relative is taken against the CONFIG
// directory (where config.db and logs.db already live), not the process's
// working directory -- which varies with how hsf_gateway was launched and
// would otherwise scatter a database per launch method.
std::string ResolveDbPath(const std::string& path) {
  std::filesystem::path candidate(path);
  if (candidate.is_absolute()) return candidate.lexically_normal().string();

  std::filesystem::path configPath(ConfigManager::Instance().Path());
  std::filesystem::path base = configPath.has_parent_path() ? configPath.parent_path() : std::filesystem::path(".");

  return (base / candidate).lexically_normal().string();
}

// Turns the parameter array at `index` into bound values. Missing/absent table
// means "no parameters". Booleans become 0/1 because SQLite has no boolean
// type; Db.NULL becomes SQL NULL.
std::vector<SqlValue> ParamsFromTable(lua_State* L, int index) {
  std::vector<SqlValue> params;

  if (lua_isnoneornil(L, index)) return params;
  luaL_checktype(L, index, LUA_TTABLE);

  lua_Integer count = luaL_len(L, index);
  for (lua_Integer i = 1; i <= count; ++i) {
    lua_geti(L, index, i);
    switch (lua_type(L, -1)) {
      case LUA_TNIL:
        params.push_back(SqlValue::Null());
        break;
      case LUA_TBOOLEAN:
        params.push_back(SqlValue::Int(lua_toboolean(L, -1) ? 1 : 0));
        break;
      case LUA_TNUMBER:
        if (lua_isinteger(L, -1)) {
          params.push_back(SqlValue::Int(static_cast<int64_t>(lua_tointeger(L, -1))));
        } else {
          params.push_back(SqlValue::Real(lua_tonumber(L, -1)));
        }
        break;
      case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, -1) == const_cast<void*>(DbNullSentinel())) {
          params.push_back(SqlValue::Null());
        } else {
          params.push_back(SqlValue::Null());
        }
        break;
      default: {
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        params.push_back(SqlValue::Text(std::string(text ? text : "", length)));
        break;
      }
    }
    lua_pop(L, 1);
  }

  return params;
}

// One SQL value -> the closest Lua type. NULL becomes nil, which means a
// column that is NULL is simply absent from the row table -- the same
// convention Lua tables already have for "no value", and what lets a script
// write `if row.expire_at then` without a sentinel.
void PushSqlValue(lua_State* L, const nlohmann::json& value) {
  if (value.is_null()) {
    lua_pushnil(L);
  } else if (value.is_boolean()) {
    lua_pushboolean(L, value.get<bool>() ? 1 : 0);
  } else if (value.is_number_integer()) {
    lua_pushinteger(L, value.get<int64_t>());
  } else if (value.is_number_float()) {
    lua_pushnumber(L, value.get<double>());
  } else if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    lua_pushlstring(L, text.c_str(), text.size());
  } else {
    std::string dumped = value.dump();
    lua_pushlstring(L, dumped.c_str(), dumped.size());
  }
}

void PushSqlRow(lua_State* L, const nlohmann::json& row) {
  lua_newtable(L);
  for (const auto& item : row.items()) {
    if (item.value().is_null()) continue;  // absent, not a sentinel
    PushSqlValue(L, item.value());
    lua_setfield(L, -2, item.key().c_str());
  }
}

}  // namespace

// Db.Open("smartlocker.db") -> true | nil, error
int LuaEngine::Lua_Db_Open(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* path = luaL_checkstring(L, 1);

  if (!engine) {
    lua_pushnil(L);
    lua_pushstring(L, "no engine");
    return 2;
  }

  if (!engine->db_) engine->db_ = std::make_unique<SqlDatabase>();

  std::string resolved = ResolveDbPath(path);

  // The directory has to exist; SQLite will not create it, and "unable to
  // open database file" is a famously unhelpful message for a missing folder.
  std::filesystem::path parent = std::filesystem::path(resolved).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
  }

  std::string error;
  if (!engine->db_->Open(resolved, false, error)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }

  Logger::Instance().Info(LogCategory::Lua, "Db.Open: " + resolved);

  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Db_Close(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (engine && engine->db_) engine->db_->Close();
  return 0;
}

int LuaEngine::Lua_Db_IsOpen(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  lua_pushboolean(L, (engine && engine->db_ && engine->db_->IsOpen()) ? 1 : 0);
  return 1;
}

int LuaEngine::Lua_Db_Path(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (!engine || !engine->db_ || !engine->db_->IsOpen()) {
    lua_pushnil(L);
    return 1;
  }
  std::string path = engine->db_->Path();
  lua_pushlstring(L, path.c_str(), path.size());
  return 1;
}

// Shared prologue: every Db.* call needs the connection, and a script that
// forgot Db.Open() should get one clear message rather than a crash. Raises a
// Lua error (which the script can pcall) rather than returning nil, because a
// missing Open is a programming mistake, not a runtime condition to branch on.
SqlDatabase* LuaEngine::DbHandle(lua_State* L, LuaEngine* engine, const char* what) {
  if (!engine || !engine->db_ || !engine->db_->IsOpen()) {
    luaL_error(L, "%s: no database is open -- call Db.Open(path) first", what);
    return nullptr;
  }
  return engine->db_.get();
}

// Db.Exec(sql [, params]) -> changes, last_insert_id | nil, error
int LuaEngine::Lua_Db_Exec(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* sql = luaL_checkstring(L, 1);
  std::vector<SqlValue> params = ParamsFromTable(L, 2);

  SqlDatabase* db = DbHandle(L, engine, "Db.Exec");
  if (!db) return 0;

  SqlResult result = db->Execute(sql, params);

  if (!result.ok) {
    lua_pushnil(L);
    lua_pushstring(L, result.error.c_str());
    return 2;
  }

  lua_pushinteger(L, result.changes);
  lua_pushinteger(L, result.last_insert_id);
  return 2;
}

// Db.Query(sql [, params]) -> { row, ... } | nil, error
int LuaEngine::Lua_Db_Query(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* sql = luaL_checkstring(L, 1);
  std::vector<SqlValue> params = ParamsFromTable(L, 2);

  SqlDatabase* db = DbHandle(L, engine, "Db.Query");
  if (!db) return 0;

  SqlResult result = db->Query(sql, params);

  if (!result.ok) {
    lua_pushnil(L);
    lua_pushstring(L, result.error.c_str());
    return 2;
  }

  lua_createtable(L, static_cast<int>(result.rows.size()), 0);
  int index = 1;
  for (const auto& row : result.rows) {
    PushSqlRow(L, row);
    lua_seti(L, -2, index++);
  }
  return 1;
}

// Db.QueryOne(sql [, params]) -> row | nil [, error]
//
// nil with no second value means "no such row", which is a normal answer;
// nil plus a message means the statement failed. A caller that treats both
// the same way is still correct, which is the point of the ordering.
int LuaEngine::Lua_Db_QueryOne(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* sql = luaL_checkstring(L, 1);
  std::vector<SqlValue> params = ParamsFromTable(L, 2);

  SqlDatabase* db = DbHandle(L, engine, "Db.QueryOne");
  if (!db) return 0;

  SqlResult result = db->Query(sql, params);

  if (!result.ok) {
    lua_pushnil(L);
    lua_pushstring(L, result.error.c_str());
    return 2;
  }

  if (result.rows.empty()) {
    lua_pushnil(L);
    return 1;
  }

  PushSqlRow(L, result.rows.front());
  return 1;
}

// Db.Scalar("SELECT COUNT(*) FROM lockers") -> value | nil [, error]
int LuaEngine::Lua_Db_Scalar(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* sql = luaL_checkstring(L, 1);
  std::vector<SqlValue> params = ParamsFromTable(L, 2);

  SqlDatabase* db = DbHandle(L, engine, "Db.Scalar");
  if (!db) return 0;

  SqlResult result = db->Query(sql, params);

  if (!result.ok) {
    lua_pushnil(L);
    lua_pushstring(L, result.error.c_str());
    return 2;
  }

  if (result.rows.empty() || result.columns.empty()) {
    lua_pushnil(L);
    return 1;
  }

  PushSqlValue(L, result.rows.front()[result.columns.front()]);
  return 1;
}

int LuaEngine::Lua_Db_Begin(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  SqlDatabase* db = DbHandle(L, engine, "Db.Begin");
  if (!db) return 0;

  std::string error;
  if (!db->Begin(error)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Db_Commit(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  SqlDatabase* db = DbHandle(L, engine, "Db.Commit");
  if (!db) return 0;

  std::string error;
  if (!db->Commit(error)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Db_Rollback(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  SqlDatabase* db = DbHandle(L, engine, "Db.Rollback");
  if (!db) return 0;

  std::string error;
  if (!db->Rollback(error)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Db_InTransaction(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  if (!engine || !engine->db_) {
    lua_pushinteger(L, 0);
    return 1;
  }
  lua_pushinteger(L, engine->db_->TransactionDepth());
  return 1;
}

// Db.Quote("O'Brien") -> "'O''Brien'"
//
// For identifiers and ORDER BY clauses only. Values go through the parameter
// array; a script that quotes its way around binding has reinvented SQL
// injection.
int LuaEngine::Lua_Db_Quote(lua_State* L) {
  size_t length = 0;
  const char* text = luaL_checklstring(L, 1, &length);
  std::string quoted = SqlDatabase::Quote(std::string(text, length));
  lua_pushlstring(L, quoted.c_str(), quoted.size());
  return 1;
}

// --- Http.* (script-served routes) -----------------------------------------

std::string LuaEngine::HttpRouteKey(const std::string& method, const std::string& path) {
  std::string upper = method;
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  std::string trimmed = path;
  while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();

  return upper + " " + trimmed;
}

namespace {

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> segments;
  size_t start = 0;

  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    std::string segment =
        slash == std::string::npos ? path.substr(start) : path.substr(start, slash - start);
    if (!segment.empty()) segments.push_back(segment);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }

  return segments;
}

// A registered segment written <something> matches any single segment of a
// request. That is the whole pattern language: "lockers/<id>/unlock" answers
// "lockers/7/unlock", and the handler reads the id back off request.path.
// Nothing matches across a '/', so a route can never swallow a deeper path it
// was not written for.
bool SegmentsMatch(const std::vector<std::string>& pattern, const std::vector<std::string>& actual) {
  if (pattern.size() != actual.size()) return false;

  for (size_t i = 0; i < pattern.size(); ++i) {
    const std::string& segment = pattern[i];
    bool wildcard = segment.size() >= 2 && segment.front() == '<' && segment.back() == '>';
    if (wildcard) continue;
    if (segment != actual[i]) return false;
  }

  return true;
}

}  // namespace

// Finds the route serving (method, path): an exact key first, then a pattern
// match, and the same two again under the ANY method. Returns the key, or an
// empty string. Callers hold httpMutex_.
std::string LuaEngine::MatchHttpRouteUnlocked(const std::string& method, const std::string& path) const {
  const std::string exact = HttpRouteKey(method, path);
  if (httpRoutes_.count(exact) > 0) return exact;

  const std::string anyExact = HttpRouteKey("ANY", path);
  if (httpRoutes_.count(anyExact) > 0) return anyExact;

  std::string upperMethod = method;
  for (char& c : upperMethod) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  const std::vector<std::string> actual = SplitPath(path);

  // Deterministic: std::map iterates in key order, so with two patterns that
  // both fit, the same one wins every time rather than it depending on
  // registration order.
  for (const auto& entry : httpRoutes_) {
    const HttpRoute& route = entry.second;
    if (route.method != upperMethod && route.method != "ANY") continue;
    if (route.path.find('<') == std::string::npos) continue;
    if (SegmentsMatch(SplitPath(route.path), actual)) return entry.first;
  }

  return std::string();
}

// Http.Register("GET", "lockers", function(request) return 200, body end)
//
// The handler is called with one table (method, path, query, headers, body) and
// returns either:
//     status, body [, content_type]
//     table                      -- serialised as JSON with status 200
// Returning nothing at all is a 204.
int LuaEngine::Lua_Http_Register(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* method = luaL_checkstring(L, 1);
  const char* path = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  if (!engine) {
    lua_pushnil(L);
    lua_pushstring(L, "no engine");
    return 2;
  }

  lua_pushvalue(L, 3);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  std::string key = HttpRouteKey(method, path);

  {
    std::lock_guard<std::mutex> lock(engine->httpMutex_);

    auto existing = engine->httpRoutes_.find(key);
    if (existing != engine->httpRoutes_.end() && existing->second.handlerRef != LUA_NOREF) {
      // Re-registering replaces the handler; the old closure has to be
      // unreferenced or the registry grows by one function per reload of the
      // script's route table.
      luaL_unref(L, LUA_REGISTRYINDEX, existing->second.handlerRef);
    }

    HttpRoute route;
    route.method = key.substr(0, key.find(' '));
    route.path = key.substr(key.find(' ') + 1);
    route.handlerRef = ref;
    engine->httpRoutes_[key] = route;
  }

  Logger::Instance().Info(LogCategory::Lua, "Http.Register: " + key);

  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Http_Unregister(lua_State* L) {
  LuaEngine* engine = GetEngine(L);
  const char* method = luaL_checkstring(L, 1);
  const char* path = luaL_checkstring(L, 2);

  if (!engine) return 0;

  std::string key = HttpRouteKey(method, path);

  std::lock_guard<std::mutex> lock(engine->httpMutex_);
  auto it = engine->httpRoutes_.find(key);
  if (it == engine->httpRoutes_.end()) {
    lua_pushboolean(L, 0);
    return 1;
  }

  if (it->second.handlerRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, it->second.handlerRef);
  }
  engine->httpRoutes_.erase(it);

  lua_pushboolean(L, 1);
  return 1;
}

int LuaEngine::Lua_Http_Routes(lua_State* L) {
  LuaEngine* engine = GetEngine(L);

  lua_newtable(L);
  if (!engine) return 1;

  int index = 1;
  for (const std::string& route : engine->HttpRoutes()) {
    lua_pushlstring(L, route.c_str(), route.size());
    lua_seti(L, -2, index++);
  }
  return 1;
}

bool LuaEngine::HandlesHttp(const std::string& method, const std::string& path) const {
  std::lock_guard<std::mutex> lock(httpMutex_);
  if (httpRoutes_.empty()) return false;

  // A route registered as ANY answers every method, so a script can serve one
  // path for GET and POST without registering it twice; MatchHttpRouteUnlocked
  // covers that and the <segment> patterns.
  return !MatchHttpRouteUnlocked(method, path).empty();
}

std::vector<std::string> LuaEngine::HttpRoutes() const {
  std::lock_guard<std::mutex> lock(httpMutex_);
  std::vector<std::string> out;
  out.reserve(httpRoutes_.size());
  for (const auto& entry : httpRoutes_) out.push_back(entry.first);
  return out;
}

bool LuaEngine::SubmitHttp(const std::shared_ptr<LuaHttpRequest>& request) {
  if (!request) return false;
  if (!running_.load()) return false;

  {
    std::lock_guard<std::mutex> lock(httpMutex_);
    if (httpRoutes_.empty()) return false;
    // Bounded: a script that stopped draining must not let the web server
    // accumulate requests until the process runs out of memory. 64 is far more
    // than the dashboard's polling can produce in one 100 ms loop pass.
    if (httpPending_.size() >= 64) return false;
    httpPending_.push(request);
  }

  return true;
}

void LuaEngine::DrainPendingHttp(lua_State* L) {
  // Same contract as DrainPendingEvents: called from inside a native binding
  // on the script's own thread, with mutex_ already held by that call.
  for (;;) {
    std::shared_ptr<LuaHttpRequest> request;
    int handlerRef = LUA_NOREF;

    {
      std::lock_guard<std::mutex> lock(httpMutex_);
      if (httpPending_.empty()) break;
      request = httpPending_.front();
      httpPending_.pop();

      const std::string key = MatchHttpRouteUnlocked(request->method, request->path);
      auto route = key.empty() ? httpRoutes_.end() : httpRoutes_.find(key);
      if (route != httpRoutes_.end()) handlerRef = route->second.handlerRef;
    }

    if (!request) break;

    auto complete = [&request]() {
      std::lock_guard<std::mutex> lock(request->mutex);
      request->completed = true;
      request->done.notify_all();
    };

    if (handlerRef == LUA_NOREF) {
      request->status = 404;
      request->error = "no handler for " + request->method + " " + request->path;
      complete();
      continue;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, handlerRef);
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      request->status = 500;
      request->error = "route handler is not a function";
      complete();
      continue;
    }

    // The request table the handler receives.
    lua_newtable(L);
    lua_pushlstring(L, request->method.c_str(), request->method.size());
    lua_setfield(L, -2, "method");
    lua_pushlstring(L, request->path.c_str(), request->path.size());
    lua_setfield(L, -2, "path");
    lua_pushlstring(L, request->body.c_str(), request->body.size());
    lua_setfield(L, -2, "body");
    lua_pushlstring(L, request->content_type.c_str(), request->content_type.size());
    lua_setfield(L, -2, "content_type");

    lua_newtable(L);
    for (const auto& entry : request->query) {
      lua_pushlstring(L, entry.second.c_str(), entry.second.size());
      lua_setfield(L, -2, entry.first.c_str());
    }
    lua_setfield(L, -2, "query");

    lua_newtable(L);
    for (const auto& entry : request->headers) {
      lua_pushlstring(L, entry.second.c_str(), entry.second.size());
      lua_setfield(L, -2, entry.first.c_str());
    }
    lua_setfield(L, -2, "headers");

    if (lua_pcall(L, 1, 3, 0) != LUA_OK) {
      const char* message = lua_tostring(L, -1);
      request->status = 500;
      request->error = message ? message : "handler error";
      Logger::Instance().Error(LogCategory::Lua,
                                "HTTP handler " + request->method + " " + request->path + " failed: " +
                                    request->error);
      lua_pop(L, 1);
      complete();
      continue;
    }

    // Return shapes, in the order they are checked:
    //   (number status, string body [, string content_type])
    //   (string body)                       -> 200
    //   (nothing)                           -> 204
    int returned = 3;
    if (lua_type(L, -3) == LUA_TNUMBER) {
      request->status = static_cast<int>(lua_tointeger(L, -3));
      if (lua_isstring(L, -2)) {
        size_t length = 0;
        const char* body = lua_tolstring(L, -2, &length);
        request->response_body.assign(body, length);
      }
      if (lua_isstring(L, -1)) {
        request->response_content_type = lua_tostring(L, -1);
      }
    } else if (lua_isstring(L, -3)) {
      size_t length = 0;
      const char* body = lua_tolstring(L, -3, &length);
      request->status = 200;
      request->response_body.assign(body, length);
    } else {
      request->status = 204;
    }

    lua_pop(L, returned);
    complete();
  }
}

}  // namespace hsf
