#include "hsf/WebServer.h"

#include <crow.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "hsf/CardCache.h"
#include "hsf/CardClientManager.h"
#include "hsf/ConfigManager.h"
#include "hsf/DynamicConfigManager.h"
#include "hsf/LogStore.h"
#include "hsf/LuaRuntimeManager.h"
#include "hsf/Logger.h"
#include "hsf/ModbusClient.h"
#include "hsf/ModbusCodec.h"
#include "hsf/ModbusRegistry.h"
#include "hsf/MqClient.h"
#include "hsf/NetPing.h"
#include "hsf/RestClient.h"
#include "hsf/RfidClient.h"
#include "hsf/RuntimeVariables.h"
#include "hsf/SerialPort.h"
#include "hsf/SqlDatabase.h"
#include "hsf/SystemMonitor.h"
#include "hsf/TcpSocket.h"
#include "hsf/ServiceRegistry.h"
#include "hsf/security/Permissions.h"
#include "hsf/security/PasswordHash.h"
#include "hsf/security/SecurityStore.h"
#include "hsf/security/Validation.h"
#include "hsf/lua_package/PackageManager.h"
#include "hsf/plugin_manager/PluginManager.h"
#include "hsf/update/UpdateManager.h"
// Private header, deliberately under src/ -- see the note at its top.
#include "security/SecurityMiddleware.h"
#include "hsf/zk_controller/ZkController.h"

namespace hsf {

using nlohmann::json;
using namespace crow;  // for the "GET"_method / "POST"_method literals

namespace {

// Sends Crow's own log lines through the gateway's Logger, so they carry the
// same LOCAL timestamp as everything else instead of Crow's UTC one, and land
// in the Log Viewer under System rather than only on the console.
class CrowLogBridge : public crow::ILogHandler {
 public:
  void log(const std::string& message, crow::LogLevel level) override {
    switch (level) {
      case crow::LogLevel::Debug:
        Logger::Instance().Debug(LogCategory::System, message);
        break;
      case crow::LogLevel::Warning:
        Logger::Instance().Warning(LogCategory::System, message);
        break;
      case crow::LogLevel::Error:
      case crow::LogLevel::Critical:
        Logger::Instance().Error(LogCategory::System, message);
        break;
      case crow::LogLevel::Info:
      default:
        Logger::Instance().Info(LogCategory::System, message);
        break;
    }
  }
};

// Strips leading/trailing whitespace (including \r\n) from a header value —
// e.g. a trailing newline from `-H "API-Key: $(cat key.txt)"` in a script,
// or accidentally selecting surrounding text when copy-pasting a key from
// the web UI — since CardClientManager::Authenticate does an exact string
// match against the stored key.
std::string TrimWhitespace(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Percent-decoding for query values. Written out rather than taken from Crow's
// query_string::keys(), which does not exist in every Crow release this project
// has been built against -- and a route that fails to compile on someone else's
// vcpkg is worse than twenty lines here.
std::string UrlDecode(const std::string& text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out += ' ';
    } else if (text[i] == '%' && i + 2 < text.size() &&
                std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }

  return out;
}

// "/api/app/lockers?status=EMPTY&limit=20" -> { status: "EMPTY", limit: "20" }
std::map<std::string, std::string> ParseQueryString(const std::string& rawUrl) {
  std::map<std::string, std::string> out;

  size_t start = rawUrl.find('?');
  if (start == std::string::npos) return out;

  std::string query = rawUrl.substr(start + 1);
  size_t position = 0;

  while (position < query.size()) {
    size_t amp = query.find('&', position);
    std::string pair = query.substr(position, amp == std::string::npos ? std::string::npos : amp - position);
    position = amp == std::string::npos ? query.size() : amp + 1;

    if (pair.empty()) continue;

    size_t equals = pair.find('=');
    if (equals == std::string::npos) {
      out[UrlDecode(pair)] = "";
    } else {
      out[UrlDecode(pair.substr(0, equals))] = UrlDecode(pair.substr(equals + 1));
    }
  }

  return out;
}

std::string ContentTypeForExtension(const std::string& path) {
  auto dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  if (ext == "html") return "text/html";
  if (ext == "css") return "text/css";
  if (ext == "js") return "application/javascript";
  if (ext == "json") return "application/json";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  return "application/octet-stream";
}

bool ReadFileToString(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// The directory the script manager lists/reads/writes/deletes files in:
// wherever the configured startup script currently lives, falling back to
// "<config file's directory>/scripts" if there is no startup script
// configured (script_path == "", e.g. after unchecking every script's
// "Auto-run on startup" box) or it has no directory component — otherwise
// clearing the startup script would also make every other script invisible
// to the manager, since it has nowhere else to derive a listing directory.
// Moved to ConfigManager so LuaEngine can build package.path from the same
// directory this lists scripts from -- two copies would let the editor's Run
// resolve require() against a different directory than the script manager
// browses.
std::string ScriptsDir() { return ConfigManager::Instance().ScriptsDir(); }

// A path relative to ScriptsDir(), ending in .lua. Scripts may live in
// subdirectories (request/updateUI.md section 13), so '/' is allowed as a
// separator — but every other guard stays, since these names come straight
// from request URLs/bodies:
//
//   - strict character whitelist rather than blocking known-bad substrings,
//     so it rejects '%', backslash, ':' and anything else regardless of what
//     any given HTTP layer decodes. (Crow's <string> route parameter does
//     not URL-decode its capture — a %2f arrives as three literal
//     characters — but this deliberately doesn't depend on that.)
//   - ".." rejected anywhere, so no segment can climb out of ScriptsDir().
//   - no leading '/', which would make the join absolute and escape the
//     directory entirely.
//   - no empty segments ("a//b"), which normalise unpredictably.
bool IsValidScriptPath(const std::string& name) {
  if (name.empty() || name.size() > 256) return false;
  if (name.size() < 4 || name.substr(name.size() - 4) != ".lua") return false;
  if (name.find("..") != std::string::npos) return false;
  if (name.front() == '/') return false;
  if (name.find("//") != std::string::npos) return false;
  for (char c : name) {
    bool allowed =
        std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/';
    if (!allowed) return false;
  }
  return true;
}

// Validates `name` AND resolves it, refusing anything that does not land
// inside the scripts directory (request/AdvanceUpdate.md section 1.7).
//
// IsValidScriptPath above is a lexical check: it rejects "..", absolute paths
// and anything outside a narrow character set, which stops every string-shaped
// traversal. What it cannot see is a symlink -- "reports.lua" inside scripts/
// pointing at /etc/shadow contains no forbidden characters at all. Validate::
// PathStaysWithin resolves the path first and then compares it component-wise
// against the base, so the link is followed before the decision is made.
bool ResolveScriptPath(const std::string& name, std::string& resolved) {
  if (!IsValidScriptPath(name)) return false;
  const std::string base = ScriptsDir();
  if (Validate::PathStaysWithin(base, name, resolved)) return true;
  Logger::Instance().Warning(LogCategory::System,
                              "Refused a script path that resolves outside the scripts directory: " +
                                  Validate::SanitiseForLog(name, 128));
  return false;
}

// Script names for the multi-select run/stop routes. Accepts both a single
// ?name= query parameter (what the per-file ▶ button sends, and what the route
// accepted before several scripts could run at once) and a JSON body with a
// "names" array (the editor's "Run Selected"). Validation is left to the
// caller, which reports per-name results.
std::vector<std::string> ScriptNamesFromRequest(const crow::request& req) {
  std::vector<std::string> names;

  if (auto* nameParam = req.url_params.get("name")) {
    std::string name = nameParam;
    if (!name.empty()) names.push_back(name);
  }

  if (!req.body.empty()) {
    try {
      nlohmann::json body = nlohmann::json::parse(req.body);
      if (body.contains("names") && body["names"].is_array()) {
        for (const auto& item : body["names"]) {
          if (!item.is_string()) continue;
          std::string name = item.get<std::string>();
          if (name.empty()) continue;
          // A name can legitimately arrive in both places (the UI sending
          // ?name= and a body); listing it twice would try to start the same
          // script twice and report a spurious "already running".
          if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
      } else if (body.contains("name") && body["name"].is_string()) {
        std::string name = body["name"].get<std::string>();
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
          names.push_back(name);
        }
      }
    } catch (const std::exception&) {
      // Not JSON, or not the shape expected -- the query parameter (if any)
      // still stands on its own.
    }
  }

  return names;
}

}  // namespace

struct WebServer::Impl {
  WebConfig config;
  std::string webRoot;
  ServiceRegistry* services = nullptr;

  RestClient* rest = nullptr;
  SerialPort* serial = nullptr;
  SerialPort* serial2 = nullptr;
  ModbusClient* modbus = nullptr;
  RfidClient* rfid = nullptr;
  LuaRuntimeManager* lua = nullptr;
  MqClient* mq = nullptr;
  // The gateway's own controller session, shared with the Lua application --
  // see the note on WebServer::SetModules.
  ZkController* zk = nullptr;
  UpdateManager* update = nullptr;
  PackageManager* packages = nullptr;
  PluginManager* plugins = nullptr;
  SystemMonitor sysMonitor;
  // When this process started serving, for /api/health's uptime. steady_clock
  // because it is a duration, and a wall-clock jump (NTP settling after boot,
  // which is exactly when a deployment health check runs) must not make the
  // gateway look like it restarted.
  std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();

  // --- OTA status frames --------------------------------------------------
  //
  // UpdateManager pushes a status frame on every transition, including once
  // per 256 KB of a download. Those are handed here instead of being sent
  // straight down the sockets: the frames arrive on the update thread, and
  // Crow's connections belong to the server's own threads. Parking the latest
  // one and letting BroadcastLoop send it also coalesces a burst of progress
  // updates into the one frame per second the browser can actually paint.
  std::mutex updateFrameMutex;
  json pendingUpdateFrame;
  bool haveUpdateFrame = false;

  // --- Test Tool (request/UpdateTestToolPlan.md sections 41-48) -----------
  //
  // Two bounded tails. Both are diagnostics, not records: the durable audit
  // trail is Logger/LogStore, and these exist so a page opened after the fact
  // still has something to show.
  std::mutex zkEventsMutex;
  std::deque<json> zkEvents;
  std::mutex testLogMutex;
  std::deque<json> testLog;

  // Not SimpleApp any more: the security pipeline runs as Crow middleware, so
  // it applies to all 86 routes below without any of them opting in. See
  // src/security/SecurityMiddleware.h and the policy table in
  // src/security/Permissions.cpp.
  using SecureApp = crow::App<SecurityMiddleware>;
  SecureApp app;
  std::thread serverThread;
  std::thread broadcastThread;
  std::atomic<bool> running{false};

  std::mutex wsMutex;
  std::vector<crow::websocket::connection*> wsConnections;

  // --- the SmartLocker floor-plan UI, on its own port --------------------
  //
  // A second Crow app rather than more routes on the first: this is the
  // operator's wall display, and giving it its own listener is what lets a
  // kiosk or a firewall be pointed at exactly one of the two UIs. It shares
  // this process, this object and the same bind address; nothing else.
  crow::SimpleApp lockerApp;
  std::thread lockerServerThread;
  std::thread lockerBroadcastThread;
  std::mutex lockerWsMutex;
  std::vector<crow::websocket::connection*> lockerWsConnections;

  // Read-only handle on the database the SmartLocker script writes. Read-only
  // is the whole design: the UI shows what the script decided and asks it (over
  // a Lua route) when it wants something changed -- a web thread must never
  // write a locker row out from under the state machine that owns it.
  SqlDatabase lockerDb;
  std::mutex lockerDbMutex;      // guards the open/reopen dance, not the queries
  int64_t lockerDbNextRetry = 0; // unix seconds; the file may not exist yet
  // Whether this database has the contractor daily-usage columns
  // (CardScanPlan section 1). The Lua application adds them on its first open,
  // so a gateway whose UI starts first -- or one pointed at a database from an
  // older build -- would otherwise fail the whole locker SELECT on a missing
  // column and show a blank floor plan. Probed once per open.
  bool lockerHasUsageColumns = false;

  // How long a Crow worker waits for a script to answer a route it registered.
  // The script picks the request up at its next Sleep(), so this is generous
  // for a 100 ms loop and still bounded for a script that has wedged.
  static constexpr int kLuaRouteTimeoutMs = 5000;

  // Forwards one request to whichever running script registered `path` with
  // Http.Register. 404 when nothing serves it, 503 when the script stopped
  // mid-request, 504 when it did not answer in time.
  crow::response ServeLuaRoute(const crow::request& req, const std::string& path) {
    auto request = std::make_shared<LuaHttpRequest>();
    request->method = crow::method_name(req.method);
    request->path = path;
    request->body = req.body;
    request->query = ParseQueryString(req.raw_url);

    // Only the headers a script has a use for. Copying all of them would hand
    // the handler a table full of Accept-Encoding noise, and cookies/auth
    // material it has no reason to see.
    for (const char* name : {"Content-Type", "API-Key", "Accept", "X-Requested-With"}) {
      std::string value = req.get_header_value(name);
      if (!value.empty()) request->headers[name] = value;
    }
    request->content_type = req.get_header_value("Content-Type");

    return ForwardToLua(request);
  }

  // The half of ServeLuaRoute that has nothing to do with Crow, so the locker
  // UI's action endpoints (remote unlock, sync now) can reach a script without
  // pretending to be an /api/app/ request.
  crow::response ForwardToLua(const std::shared_ptr<LuaHttpRequest>& request) {
    if (!lua) {
      return crow::response(503, json{{"ok", false}, {"error", "no Lua runtime manager"}}.dump());
    }

    if (!lua->DispatchHttp(request, kLuaRouteTimeoutMs)) {
      json body{{"ok", false},
                 {"error", "no script serves " + request->method + " " + request->path},
                 {"routes", json::array()}};
      for (const std::string& route : lua->HttpRoutes()) body["routes"].push_back(route);
      return crow::response(404, body.dump());
    }

    if (request->status == 0) {
      return crow::response(500, json{{"ok", false}, {"error", "handler produced no status"}}.dump());
    }

    // A handler that failed (or was never reached) reports through `error` with
    // no body of its own; anything else is passed through untouched, so a script
    // can serve HTML or CSV as easily as JSON.
    if (request->response_body.empty() && !request->error.empty()) {
      return crow::response(request->status, json{{"ok", false}, {"error", request->error}}.dump());
    }

    crow::response res(request->status, request->response_body);
    res.set_header("Content-Type", request->response_content_type.empty() ? "application/json"
                                                                          : request->response_content_type);
    return res;
  }

  // --- Test Tool: shared plumbing ----------------------------------------

  // Pushes one frame to every connected dashboard client. Callable from any
  // thread -- the ZK driver's event callback runs on the driver's own thread,
  // and RTLog has to reach the page when it happens rather than on the
  // broadcast loop's next one-second tick (plan section 58).
  void BroadcastJson(const json& frame) {
    std::string text = frame.dump();
    std::lock_guard<std::mutex> lock(wsMutex);
    for (auto* conn : wsConnections) conn->send_text(text);
  }

  // The parsed record, field for field. `in_out_status` keeps the driver's own
  // name and is NOT relabelled reader_id: RTLogEvent.h says in as many words
  // that the field's meaning is unconfirmed against real controller output, and
  // a diagnostic is the last place to guess.
  static json ZkEventJson(const RTLogEvent& event) {
    return json{{"time", event.time},
                 {"pin", event.pin},
                 {"card_no", event.cardNo},
                 {"door_no", event.doorNo},
                 {"event_type", event.eventType},
                 {"in_out_status", event.inOutStatus},
                 {"verify_mode", event.verifyMode}};
  }

  void OnZkEvent(const RTLogEvent& event) {
    json row = ZkEventJson(event);
    row["received_at"] = NowUnixSeconds();

    {
      std::lock_guard<std::mutex> lock(zkEventsMutex);
      zkEvents.push_back(row);
      while (zkEvents.size() > 500) zkEvents.pop_front();
    }

    BroadcastJson(json{{"type", "zk.rtlog"}, {"data", row}});

    // Auxiliary input edges get their own frame as well, so an Input Monitor
    // does not have to know that 220/221 mean anything (section 47). The door
    // field carries the input number for those two types.
    if (event.eventType == 220 || event.eventType == 221) {
      BroadcastJson(json{{"type", "zk.aux_input"},
                          {"data", {{"input", event.doorNo},
                                    {"shorted", event.eventType == 221},
                                    {"time", event.time}}}});
    }
  }

  void AttachZkListeners() {
    if (!zk) return;

    // The ONLY free callback slot on the driver: LuaRuntimeManager owns card,
    // raw, connection and aux-input, and every one of those slots is
    // single-occupancy. Taking one would silently cut the Lua application off
    // from its own card reads, which is why the parsed-event callback was added
    // to ZkController rather than borrowed from it.
    zk->SetEventCallback([this](const RTLogEvent& event) { OnZkEvent(event); });
  }

  // Undone on shutdown -- see WebServer::Stop for why this cannot wait for the
  // destructor.
  void DetachZkListeners() {
    if (!zk) return;
    zk->SetEventCallback(nullptr);
  }

  // Plan section 56: every Test Tool operation is logged with what was asked,
  // what happened and how long it took. Returns the row so a route can hand it
  // straight back to the caller.
  json LogTestOp(const std::string& module, const std::string& operation, const json& params,
                  bool ok, const std::string& error, int64_t durationMs) {
    json row{{"timestamp", NowTimestamp()},
              {"module", module},
              {"operation", operation},
              {"parameters", params},
              {"result", ok ? "SUCCESS" : "FAILED"},
              {"duration_ms", durationMs},
              {"error", error}};

    {
      std::lock_guard<std::mutex> lock(testLogMutex);
      testLog.push_back(row);
      while (testLog.size() > 300) testLog.pop_front();
    }

    std::string line = "test tool: " + module + " / " + operation + " -> " +
                        (ok ? "SUCCESS" : "FAILED " + error) + " (" + std::to_string(durationMs) + " ms)";

    if (ok) {
      Logger::Instance().Info(LogCategory::System, line);
    } else {
      Logger::Instance().Warning(LogCategory::System, line);
    }

    BroadcastJson(json{{"type", "test.log"}, {"data", row}});

    return row;
  }

  // --- locker UI: database access ----------------------------------------

  // Opens (or re-opens) the locker database read-only. The file legitimately
  // does not exist until the SmartLocker script has run once, so a failure here
  // is a normal early state, retried every 5 seconds rather than logged per
  // request.
  bool EnsureLockerDb() {
    if (lockerDb.IsOpen()) return true;

    std::lock_guard<std::mutex> lock(lockerDbMutex);
    if (lockerDb.IsOpen()) return true;

    int64_t now = NowUnixSeconds();
    if (now < lockerDbNextRetry) return false;
    lockerDbNextRetry = now + 5;

    std::filesystem::path configured(config.locker_db_path);
    std::filesystem::path path = configured;

    if (configured.is_relative()) {
      std::filesystem::path configFile(ConfigManager::Instance().Path());
      std::filesystem::path base =
          configFile.has_parent_path() ? configFile.parent_path() : std::filesystem::path(".");
      path = (base / configured).lexically_normal();
    }

    std::string error;
    if (!lockerDb.Open(path.string(), true, error)) {
      Logger::Instance().Debug(LogCategory::System,
                                "locker UI: " + path.string() + " is not readable yet (" + error + ")");
      return false;
    }

    Logger::Instance().Info(LogCategory::System, "locker UI reading " + path.string());

    // Probed here rather than per query: it is a property of the file we just
    // opened, and it changes only when the Lua application upgrades the schema
    // -- which closes and reopens this handle anyway (LockerQuery drops it on
    // any failure).
    lockerHasUsageColumns = false;
    SqlResult columns = lockerDb.Query("PRAGMA table_info(employees)", {});
    if (columns.ok) {
      for (const auto& column : columns.rows) {
        if (column.value("name", std::string()) == "usage_status") {
          lockerHasUsageColumns = true;
          break;
        }
      }
    }

    if (!lockerHasUsageColumns) {
      Logger::Instance().Info(LogCategory::System,
                              "locker UI: this database has no contractor usage columns yet; "
                              "the daily-usage fields will be empty until the SmartLocker "
                              "script has opened it once");
    }

    return true;
  }

  // Every query the UI makes goes through here so a database that was deleted,
  // replaced or not yet created degrades to an empty answer plus a reopen on
  // the next tick, instead of a 500 per request.
  json LockerQuery(const std::string& sql, const std::vector<SqlValue>& params = {}) {
    if (!EnsureLockerDb()) return json::array();

    SqlResult result = lockerDb.Query(sql, params);

    if (!result.ok) {
      // A missing table means the script has created the file but not the
      // schema yet -- again an early state, not a fault worth an error line.
      if (result.error.find("no such table") == std::string::npos) {
        Logger::Instance().Warning(LogCategory::System, "locker UI query failed: " + result.error);
      }
      lockerDb.Close();
      return json::array();
    }

    return result.rows;
  }

  // The whole model the UI renders: every locker with the person in it, the
  // four status counts, and the most recent events. One payload, because the
  // page redraws from a single snapshot and a partial update would leave the
  // grid and the counters disagreeing.
  json BuildLockerStateJson(int eventLimit = 40) {
    // Opened before the SELECT is composed, because which columns exist is what
    // decides the SELECT. A file that is not there yet leaves the flag false and
    // every query below answers empty, which is the same early state as before.
    EnsureLockerDb();

    // The contractor's working day (usage_*) rides along with the locker rather
    // than being a second request: the page draws the door and the person's
    // state in one pass, and two payloads would let a tile and its panel
    // disagree. Aliased card_* where the locker already owns a column of that
    // name -- lockers.expire_at is the ASSIGNMENT's expiry, not the card's.
    const char* kUsageColumns =
        ", e.start_at AS card_start_at, e.usage_status, e.usage_date, "
        "  e.usage_started_at, e.usage_last_open_at, e.usage_completed_at ";

    json lockers = LockerQuery(
        std::string(
            "SELECT l.id, l.block_id, l.block_name, l.locker_number, l.locker_type, l.status, "
            "       l.card_code, l.assigned_at, l.expire_at, l.last_open_at, l.last_close_at, "
            "       l.last_error, l.door_open, l.runtime_state, l.output_register, l.updated_at, "
            "       e.username, e.gender, e.role, e.expire_at AS card_expire_at, e.active ") +
        (lockerHasUsageColumns ? kUsageColumns : " ") +
        "  FROM lockers l "
        "  LEFT JOIN employees e ON e.card_code = l.card_code "
        " ORDER BY l.block_id, l.locker_number");

    json summary = {{"total", 0},   {"empty", 0}, {"assigned", 0},
                     {"expired", 0}, {"error", 0}, {"open", 0},
                     {"no_output", 0}};

    json blocks = json::object();

    for (const auto& locker : lockers) {
      summary["total"] = summary["total"].get<int>() + 1;

      std::string status = locker.value("status", "");
      if (status == "EMPTY") summary["empty"] = summary["empty"].get<int>() + 1;
      else if (status == "ASSIGNED") summary["assigned"] = summary["assigned"].get<int>() + 1;
      else if (status == "EXPIRED") summary["expired"] = summary["expired"].get<int>() + 1;
      else if (status == "ERROR") summary["error"] = summary["error"].get<int>() + 1;

      if (locker.value("door_open", 0) != 0) summary["open"] = summary["open"].get<int>() + 1;
      if (locker["output_register"].is_null()) summary["no_output"] = summary["no_output"].get<int>() + 1;

      // Block list built from the lockers themselves rather than a separate
      // query: the cabinet the UI draws is exactly the set of doors that exist.
      std::string blockId = std::to_string(locker.value("block_id", 0));
      if (!blocks.contains(blockId)) {
        blocks[blockId] = {{"id", locker.value("block_id", 0)},
                            {"name", locker.value("block_name", "")},
                            {"type", locker.value("locker_type", "")},
                            {"count", 0}};
      }
      blocks[blockId]["count"] = blocks[blockId]["count"].get<int>() + 1;
    }

    json events = LockerQuery(
        "SELECT id, locker_id, card_code, event, result, reason, created_at "
        "  FROM locker_logs ORDER BY id DESC LIMIT ?",
        {SqlValue::Int(eventLimit)});

    json employees = LockerQuery(
        std::string("SELECT id, username, card_code, role, gender, expire_at, active, locker_id") +
        (lockerHasUsageColumns
             ? ", start_at, usage_status, usage_date, usage_started_at, usage_completed_at "
             : " ") +
        "  FROM employees ORDER BY (active = 0), username LIMIT 500");

    // The holding policy, published into `meta` by the SmartLocker script (see
    // Assignment.publish_policy). Relayed rather than interpreted: which
    // deadline applies to a locker is the script's decision, and the page only
    // needs the two numbers to count down from.
    json policy = {{"contractor_mode", 1}, {"hold_hours", 24}, {"auto_reclaim", true}};

    for (const auto& row : LockerQuery("SELECT key, value FROM meta WHERE key IN "
                                        "('contractor_mode', 'hold_hours', 'auto_reclaim')")) {
      const std::string key = row.value("key", "");
      const std::string value = row.value("value", "");

      try {
        if (key == "auto_reclaim") {
          policy[key] = std::stoi(value) != 0;
        } else if (!key.empty()) {
          policy[key] = std::stoi(value);
        }
      } catch (const std::exception&) {
        // A meta row we cannot read leaves the default in place: a countdown
        // drawn from a garbled number would be worse than one drawn from the
        // documented default.
      }
    }

    json blockList = json::array();
    for (const auto& entry : blocks) blockList.push_back(entry);

    return json{{"ok", lockerDb.IsOpen()},
                 {"generated_at", NowUnixSeconds()},
                 {"machine_id", ConfigManager::Instance().GetSystem().machine_id},
                 {"device_name", ConfigManager::Instance().GetSystem().device_name},
                 {"blocks", blockList},
                 {"policy", policy},
                 {"lockers", lockers},
                 {"summary", summary},
                 {"employees", employees},
                 {"events", events},
                 {"gateway", {{"plc", modbus && modbus->IsConnected()},
                               {"reader", rfid && rfid->IsConnected()},
                               {"lua", lua && lua->AnyRunning()}}}};
  }

  // Builds the response in place and returns; the caller's route handler
  // returns `res` by value, which is how Crow finalizes a synchronous
  // response. Calling res.end() here too would finalize it twice and hang
  // the connection.
  void ServeStatic(crow::response& res, const std::string& relativePath) {
    std::string fullPath = webRoot + "/" + relativePath;
    std::string body;
    if (!ReadFileToString(fullPath, body)) {
      res.code = 404;
      res.body = "Not found: " + relativePath;
      return;
    }
    res.set_header("Content-Type", ContentTypeForExtension(relativePath));
    res.code = 200;
    res.body = body;
  }

  // updateUI.md section 11 is explicit that System Info must be empty when
  // nothing is running rather than showing stale or invented figures, so an
  // empty `scripts` array is a real answer and the UI renders its empty state.
  //
  // Every runtime is listed, including ones that have stopped or failed:
  // request/upgrade.md section 22 wants a failed script visible AS failed
  // alongside the ones still running, which dropping it from the list could
  // never convey. `running` and `state` stay at the top level for the
  // dashboard's single Lua status light.
  json BuildLuaRuntimeJson() {
    if (!lua) return json{{"running", false}, {"state", "STOPPED"}, {"scripts", json::array()}};

    json scripts = json::array();
    std::string aggregateState = "STOPPED";
    std::string lastError;
    bool anyRunning = false;

    for (const auto& status : lua->Statuses()) {
      scripts.push_back({{"runtime_id", status.runtime_id},
                          {"name", status.name},
                          {"path", status.path},
                          {"state", status.state},
                          {"start_time", status.start_time},
                          {"uptime_seconds", status.uptime_seconds},
                          {"cpu_percent", status.cpu_percent},
                          {"memory_kb", status.memory_kb},
                          {"cpu_core", status.cpu_core},
                          {"last_error", status.last_error},
                          {"error_line", status.error_line}});

      if (status.state == "RUNNING" || status.state == "STARTING") {
        anyRunning = true;
      }
      // An error anywhere is worth surfacing on the aggregate, since the
      // status light is the only Lua indicator on pages other than System Info.
      if (status.state == "ERROR" && lastError.empty()) {
        lastError = status.last_error;
      }
    }

    if (anyRunning) {
      aggregateState = "RUNNING";
    } else if (!lastError.empty()) {
      aggregateState = "ERROR";
    }

    return json{{"running", anyRunning},
                {"state", aggregateState},
                {"running_count", lua->RunningCount()},
                {"last_error", lastError},
                {"scripts", scripts}};
  }

  // "Last connected" bookkeeping for every connection the dashboard shows.
  //
  // Tracked here rather than inside each module because this is the one
  // place that already samples them all on a common tick -- adding a
  // timestamp to RestClient, SerialPort, ModbusClient, RfidClient and
  // LuaEngine separately would be five parallel implementations of the same
  // three lines.
  //
  // Resolution is the caller's polling rate (1s from the broadcast loop),
  // which is ample for "when did this last come up".
  struct ConnectionState {
    bool connected = false;
    bool seen = false;         // false until the first sample, so "never" is honest
    int64_t lastConnectedAt = 0;  // unix seconds; 0 = never connected since boot
    int64_t changedAt = 0;
  };
  std::mutex connectionMutex;
  std::map<std::string, ConnectionState> connectionStates;

  static int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // Local wall clock, the same shape the Lua side and the Logs page use, so a
  // Test Tool row can be lined up against a gateway log line by eye.
  static std::string NowTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
  }

  json TrackConnection(const std::string& key, bool connected) {
    std::lock_guard<std::mutex> lock(connectionMutex);
    ConnectionState& state = connectionStates[key];
    int64_t now = NowUnixSeconds();

    if (!state.seen || state.connected != connected) {
      state.changedAt = now;
      if (connected) state.lastConnectedAt = now;
      state.connected = connected;
      state.seen = true;
    }

    return json{{"connected", state.connected},
                {"last_connected", state.lastConnectedAt},
                {"changed_at", state.changedAt}};
  }

  json BuildStatusJson() {
    SystemStats stats = sysMonitor.Sample();
    json j = SystemMonitor::ToJson(stats);

    bool restOk = rest ? rest->LastRequestSucceeded() : false;
    bool serialOpen = serial ? serial->IsOpen() : false;
    bool plcConnected = modbus ? modbus->IsConnected() : false;
    bool rfidConnected = rfid ? rfid->IsConnected() : false;
    bool luaRunning = lua ? lua->AnyRunning() : false;

    // Build metadata (request/release.md section 27), surfaced on the
    // dashboard's System Info tab. Constant for the life of the process, but
    // carried on /api/status so the page needs no second request -- and so a
    // support report can be taken straight from the running gateway rather
    // than guessed from which binary someone thinks is deployed.
    j["build"] = {{"version", HSF_VERSION},
                   {"commit", HSF_GIT_COMMIT},
                   {"date", HSF_BUILD_DATE},
                   {"platform", HSF_PLATFORM},
                   {"compiler", HSF_COMPILER_ID},
#if defined(HSF_ENABLE_ZK)
                   {"zk_enabled", true}};
#else
                   {"zk_enabled", false}};
#endif

    j["rest_ok"] = restOk;
    j["serial_open"] = serialOpen;
    j["plc_connected"] = plcConnected;
    j["rfid_connected"] = rfidConnected;
    j["lua_running"] = luaRunning;

    j["connections"] = {{"rest", TrackConnection("rest", restOk)},
                         {"plc", TrackConnection("plc", plcConnected)},
                         {"rfid", TrackConnection("rfid", rfidConnected)},
                         {"serial", TrackConnection("serial", serialOpen)},
                         {"lua", TrackConnection("lua", luaRunning)}};

    // Broker state and its counters. Reported even when mq.enabled is off (as
    // enabled=false, connected=false) so the dashboard can say "configured but
    // switched off" instead of leaving the operator guessing.
    if (mq) {
      const MqStatus mqStatus = mq->Status();
      j["mq"] = {{"enabled", mqStatus.enabled},
                  {"connected", mqStatus.connected},
                  {"consuming", mqStatus.consuming},
                  {"paused", mqStatus.paused},
                  {"error", mqStatus.error},
                  {"published", mqStatus.published},
                  {"received", mqStatus.received},
                  {"rejected", mqStatus.rejected},
                  {"dropped", mqStatus.dropped},
                  {"overflowed", mqStatus.overflowed},
                  {"pending", mqStatus.pending},
                  {"available", mqStatus.available}};
      j["mq_connected"] = mqStatus.connected;
      // Only tracked as a connection while it is meant to be up, so a gateway
      // with no broker doesn't show a permanently red light for a feature it
      // isn't using.
      if (mqStatus.enabled) {
        j["connections"]["mq"] = TrackConnection("mq", mqStatus.connected);
      }
    }
    return j;
  }

  void SetupRoutes() {
    CROW_ROUTE(app, "/")([this] {
      crow::response res;
      ServeStatic(res, "index.html");
      return res;
    });

    // Explicit allowlist -- a new page in web/ is a 404 until it is named
    // here. card_clients.html stays listed as a redirect stub to test_tools.
    for (const std::string& page : {"index.html", "config.html", "dynamic_config.html", "lua_editor.html", "lua_docs.html", "visual_flow.html",
                                     "logs.html", "test_tools.html", "plugins.html", "firmware.html", "config_file.html", "card_clients.html", "login.html",
                                     "security.html"}) {
      std::string route = "/" + page;
      app.route_dynamic(route)([this, page](const crow::request&) {
        crow::response res;
        ServeStatic(res, page);
        return res;
      });
    }

    CROW_ROUTE(app, "/css/<string>")([this](const std::string& file) {
      crow::response res;
      ServeStatic(res, "css/" + file);
      return res;
    });
    CROW_ROUTE(app, "/js/<string>")([this](const std::string& file) {
      crow::response res;
      ServeStatic(res, "js/" + file);
      return res;
    });

    // --- deployment health probe (request/deloyToUbuntu.md sections 3.11-3.13)
    //
    // PUBLIC and deliberately thin. A deployment script has to be able to ask
    // "did it come up?" before any credential exists on the machine, and
    // systemd/curl health checks run before anyone logs in. So this is the one
    // /api/ route besides login that needs no token.
    //
    // Which means it must leak nothing. It answers with liveness, the version,
    // and uptime -- no configuration, no addresses, no connection detail, no
    // counts that would map the installation. /api/status has all of that and
    // stays behind DEVICE_READ.
    //
    // 200 means "this process is up and its own state is loadable"; 503 means
    // the opposite and is what makes automatic rollback fire. Hardware is NOT
    // part of the verdict: a PLC that is switched off is a normal Monday, and
    // failing a deployment for it would roll back a perfectly good release.
    CROW_ROUTE(app, "/api/health")([this] {
      const bool configOk = !ConfigManager::Instance().Path().empty();
      const bool healthy = configOk;

      // PROCESS uptime, from our own start time -- not SystemMonitor::Sample().
      // Two reasons: the host's uptime is not what a deployment check is
      // asking (it wants "did this release just crash-loop?"), and Sample() is
      // stateful -- it holds the CPU-delta baseline the dashboard reads, so an
      // unauthenticated caller hitting this route in a loop would quietly
      // corrupt the CPU figures for everyone.
      const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - startedAt)
                               .count();

      json body = {{"status", healthy ? "ok" : "degraded"},
                    {"version", HSF_VERSION},
                    {"uptime_seconds", static_cast<int64_t>(uptime)}};
      if (!healthy) body["reason"] = "configuration is not loaded";

      crow::response res(healthy ? 200 : 503, body.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    });

    CROW_ROUTE(app, "/api/status")([this] {
      return crow::response(200, BuildStatusJson().dump());
    });

    CROW_ROUTE(app, "/api/config").methods("GET"_method, "POST"_method)([](const crow::request& req) {
      if (req.method == "POST"_method) {
        try {
          json patch = json::parse(req.body);
          if (!ConfigManager::Instance().ApplyJson(patch)) {
            return crow::response(400, R"({"error":"invalid config"})");
          }
          ConfigManager::Instance().Save();
          Logger::Instance().Info(LogCategory::System, "Configuration updated via web UI");
        } catch (const std::exception& e) {
          return crow::response(400, json{{"error", e.what()}}.dump());
        }
      }
      return crow::response(200, ConfigManager::Instance().ToJson().dump());
    });

    CROW_ROUTE(app, "/api/config/export")([] {
      return crow::response(200, ConfigManager::Instance().ToJson().dump());
    });
    CROW_ROUTE(app, "/api/config/import").methods("POST"_method)([](const crow::request& req) {
      try {
        json imported = json::parse(req.body);
        if (!ConfigManager::Instance().ApplyJson(imported)) {
          return crow::response(400, R"({"error":"invalid config"})");
        }
        ConfigManager::Instance().Save();
        return crow::response(200, json{{"ok", true}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/serial/ports")([] {
      json ports = json::array();
      for (const auto& port : SerialPort::ScanPorts()) ports.push_back(port);
      return crow::response(200, json{{"ports", ports}}.dump());
    });

    CROW_ROUTE(app, "/api/serial/test").methods("POST"_method)([this](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        SerialConfig config = ConfigManager::Instance().GetSerial();
        config.port = body.value("port", config.port);
        config.baudrate = body.value("baudrate", config.baudrate);
        config.data_bits = body.value("data_bits", config.data_bits);
        config.stop_bits = body.value("stop_bits", config.stop_bits);
        if (body.contains("parity")) {
          std::string parity = body["parity"].get<std::string>();
          if (!parity.empty()) config.parity = parity[0];
        }

        // The gateway holds its own ports open for the whole run now, and a COM
        // port is opened exclusively (CreateFile with no sharing). So a
        // throwaway open of a port we ALREADY own fails with access-denied and
        // reports a perfectly healthy reader as broken. When this is one of our
        // ports, answer from its live state instead of fighting ourselves for
        // the handle.
        if (serial && serial->IsOpen() && ConfigManager::Instance().GetSerial().port == config.port) {
          return crow::response(200, json{{"ok", true},
                                           {"error", ""},
                                           {"detail", "Port " + config.port +
                                                          " is open and in use by the gateway (status " +
                                                          SerialPort::StateName(serial->GetState()) + ")."}}
                                          .dump());
        }
        if (serial2 && serial2->IsOpen() && ConfigManager::Instance().GetSerial2().port == config.port) {
          return crow::response(200, json{{"ok", true},
                                           {"error", ""},
                                           {"detail", "Port " + config.port +
                                                          " is open and in use by the gateway as Serial 2 (the LED "
                                                          "display)."}}
                                          .dump());
        }

        // Not ours: a throwaway instance, opened and immediately closed again.
        SerialPort testPort;
        bool ok = testPort.Open(config);
        testPort.Close();

        std::string error = ok ? "" : "Failed to open " + config.port +
                                           " — check the device path/permissions and that nothing else is using it.";
        return crow::response(200, json{{"ok", ok}, {"error", error}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Sends one real TDM-800 frame to the LED display so the Configuration
    // page can confirm wiring/baud without a Lua script running.
    //
    // The frame format is duplicated from config/scripts/led.lua, which
    // remains the authority for anything the workflow sends. Deliberate: a
    // bring-up test that only works once a script is loaded and correct is
    // useless for diagnosing a display that isn't lighting up at all. Keep
    // the two in step if the frame ever changes.
    CROW_ROUTE(app, "/api/serial2/led-test").methods("POST"_method)([this](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);

        SerialConfig config = ConfigManager::Instance().GetSerial2();
        config.port = body.value("port", config.port);
        config.baudrate = body.value("baudrate", config.baudrate);
        config.data_bits = body.value("data_bits", config.data_bits);
        config.stop_bits = body.value("stop_bits", config.stop_bits);
        if (body.contains("parity")) {
          std::string parity = body["parity"].get<std::string>();
          if (!parity.empty()) config.parity = parity[0];
        }

        std::string text = body.value("text", std::string("HSF GATEWAY TEST"));
        // The panel is ASCII-only, and an embedded CR would end the frame
        // early and truncate the message.
        std::string ascii;
        for (char c : text) {
          unsigned char u = static_cast<unsigned char>(c);
          if (u == '\r' || u == '\n') {
            ascii.push_back(' ');
          } else if (u >= 0x20 && u < 0x7F) {
            ascii.push_back(c);
          }
        }

        // CMD, ADDR(broadcast), SNUM(1 scene), SPEED, DIRECTION(static), COLOR(red)
        const char kHeader[] = {'\x00', '\x30', '\x31', '\x31', '\x32', '\x30'};
        std::string frame(kHeader, sizeof(kHeader));
        frame += ascii;
        frame.push_back('\x0D');  // terminator -- without it the panel never updates

        // Written through the gateway's OWN port when this is the port it
        // already holds open -- which it now is for the whole run. Opening a
        // throwaway handle would fail with access-denied (COM ports are opened
        // exclusively), and this way the test exercises the very path
        // Led.Show() uses rather than a lookalike.
        bool ok = false;
        if (serial2 && serial2->IsOpen() && ConfigManager::Instance().GetSerial2().port == config.port) {
          ok = serial2->WriteLatest(frame);
        } else {
          SerialPort testPort;
          if (!testPort.Open(config)) {
            return crow::response(200, json{{"ok", false},
                                             {"error", "Failed to open " + config.port +
                                                           " - check the port and that nothing else holds it."}}
                                            .dump());
          }
          ok = testPort.Write(frame);
          testPort.Close();
        }

        Logger::Instance().Info(LogCategory::Serial,
                                 "LED test frame sent to " + config.port + ": \"" + ascii + "\"");
        return crow::response(200, json{{"ok", ok},
                                         {"sent", ascii},
                                         {"bytes", frame.size()},
                                         {"error", ok ? "" : "Port opened but the write failed."}}
                                        .dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Writes a typed value to a registered holding-register point. Resolves
    // BY NAME through the registry, like the coil output route -- so this is
    // not a general "write any register" primitive, only points a script
    // deliberately declared with write=true.
    CROW_ROUTE(app, "/api/modbus/register").methods("POST"_method)([this](const crow::request& req) {
      std::string name;
      json value;
      try {
        json body = json::parse(req.body);
        name = body.at("name").get<std::string>();
        value = body.at("value");
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", std::string("invalid payload: ") + e.what()}}.dump());
      }

      ModbusRegisterPoint point;
      if (!ModbusRegistry::Instance().FindRegister(name, point)) {
        return crow::response(404, json{{"error", "no registered register named '" + name + "'"}}.dump());
      }
      if (!point.writable) {
        return crow::response(
            403, json{{"error", point.input_register
                                    ? "input registers (FC04) are read-only in Modbus"
                                    : "register was not declared writable (pass write=true in Lua)"}}
                     .dump());
      }

      // A number arriving as a JSON string ("42" from a text input) is the
      // common case from the dashboard, so coerce rather than reject.
      if (value.is_string() && point.format.type != RegisterType::kString) {
        const std::string text = value.get<std::string>();
        try {
          size_t consumed = 0;
          double parsed = std::stod(text, &consumed);
          if (consumed != text.size()) throw std::invalid_argument("trailing characters");
          if (parsed == std::floor(parsed) && std::abs(parsed) < 9.0e15) {
            value = static_cast<long long>(parsed);
          } else {
            value = parsed;
          }
        } catch (const std::exception&) {
          return crow::response(400, json{{"error", "'" + text + "' is not a number"}}.dump());
        }
      }

      std::vector<uint16_t> words;
      std::string error;
      if (!EncodeRegisters(point.format, value, words, error)) {
        return crow::response(400, json{{"error", error}}.dump());
      }

      if (!modbus || !modbus->IsConnected()) {
        return crow::response(503, json{{"error", "PLC not connected"}}.dump());
      }

      bool ok = words.size() == 1 ? modbus->WriteHoldingRegister(point.address, words[0])
                                  : modbus->WriteHoldingRegisters(point.address, words);
      if (ok) {
        Logger::Instance().Info(LogCategory::Modbus,
                                 "Manual register write: " + name + " @" + std::to_string(point.address) +
                                     " (" + ToString(point.format.type) + ", " +
                                     EndianName(point.format.byte_swap, point.format.word_swap) + ") = " +
                                     value.dump());
      }
      return crow::response(ok ? 200 : 500,
                             json{{"ok", ok}, {"name", name}, {"address", point.address}}.dump());
    });

    // ---- Test Tools (updateUI.md sections 24-31) -------------------------
    //
    // These routes exist purely to drive the Test Tools page. They are
    // deliberately one-shot and stateless: every call opens its own socket or
    // port and closes it again, so exercising them can never disturb the
    // gateway's own long-lived PLC / serial / REST connections.

    CROW_ROUTE(app, "/api/test/rest").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);

        RestRequestSpec spec;
        spec.method = body.value("method", std::string("GET"));
        spec.url = body.value("url", std::string(""));
        spec.body = body.value("body", std::string(""));
        spec.timeout_ms = body.value("timeout_ms", 5000);
        spec.verify_ssl = body.value("verify_ssl", true);

        if (body.contains("headers") && body["headers"].is_object()) {
          for (const auto& item : body["headers"].items()) {
            if (item.value().is_string()) spec.headers[item.key()] = item.value().get<std::string>();
          }
        }
        if (body.contains("form_fields") && body["form_fields"].is_object()) {
          for (const auto& item : body["form_fields"].items()) {
            if (item.value().is_string()) spec.form_fields[item.key()] = item.value().get<std::string>();
          }
        }

        if (spec.url.empty()) {
          return crow::response(400, json{{"error", "url is required"}}.dump());
        }

        RestResponse response = RestClient::Send(spec);
        return crow::response(200, json{{"ok", response.ok},
                                         {"status", response.status_code},
                                         {"elapsed_ms", response.elapsed_ms},
                                         {"headers", response.response_headers},
                                         {"body", response.body},
                                         {"error", response.error}}
                                        .dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/test/modbus").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);

        ModbusConfig configured = ConfigManager::Instance().GetModbus();
        std::string ip = body.value("ip", configured.ip);
        int port = body.value("port", configured.port);
        std::string op = body.value("op", std::string("read_coils"));
        int address = body.value("address", 0);
        int count = body.value("count", 1);

        if (ip.empty()) return crow::response(400, json{{"error", "ip is required"}}.dump());

        std::string error;
        json result{{"op", op}, {"address", address}};

        if (op == "read_coils" || op == "read_discrete_inputs") {
          std::vector<bool> values;
          bool discrete = (op == "read_discrete_inputs");
          bool ok = ModbusClient::ReadBitsOnce(ip, port, address, count, discrete, values, error);
          json bits = json::array();
          for (bool v : values) bits.push_back(v);
          result["values"] = bits;
          result["ok"] = ok;
          result["function_code"] = discrete ? 2 : 1;
        } else if (op == "read_holding") {
          std::vector<uint16_t> values;
          bool ok = ModbusClient::ReadHoldingRegistersOnce(ip, port, address, count, values, error);
          json regs = json::array();
          for (uint16_t v : values) regs.push_back(v);
          result["values"] = regs;
          result["ok"] = ok;
          result["function_code"] = 3;
        } else if (op == "write_coil") {
          bool value = body.value("value", false);
          bool ok = ModbusClient::WriteCoilOnce(ip, port, address, value, error);
          result["ok"] = ok;
          result["written"] = value;
          result["function_code"] = 5;
        } else if (op == "write_holding") {
          int raw = body.value("value", 0);
          if (raw < 0 || raw > 0xFFFF) {
            return crow::response(400, json{{"error", "value must be 0..65535"}}.dump());
          }
          bool ok = ModbusClient::WriteHoldingRegisterOnce(ip, port, address, static_cast<uint16_t>(raw), error);
          result["ok"] = ok;
          result["written"] = raw;
          result["function_code"] = 6;
        } else if (op == "connect") {
          bool ok = ModbusClient::TestConnect(ip, port, error);
          result["ok"] = ok;
        } else {
          return crow::response(400, json{{"error", "unknown op: " + op}}.dump());
        }

        result["error"] = error;
        result["target"] = ip + ":" + std::to_string(port);
        return crow::response(200, result.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Serial master: writes a frame to a port and reports whatever comes back
    // within `read_ms`. Opens its own SerialPort, so pointing it at the port
    // the gateway already holds (Serial1/Serial2) fails to open rather than
    // stealing it -- that failure is the honest answer, not a bug.
    CROW_ROUTE(app, "/api/test/serial").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);

        SerialConfig config;
        config.port = body.value("port", std::string(""));
        config.baudrate = body.value("baudrate", 9600);
        config.data_bits = body.value("data_bits", 8);
        config.stop_bits = body.value("stop_bits", 1);
        std::string parity = body.value("parity", std::string("N"));
        config.parity = parity.empty() ? 'N' : parity[0];

        if (config.port.empty()) return crow::response(400, json{{"error", "port is required"}}.dump());

        std::string payload = body.value("data", std::string(""));
        bool isHex = body.value("hex", false);
        if (isHex) {
          std::string decoded;
          std::string digits;
          for (char c : payload) {
            if (std::isxdigit(static_cast<unsigned char>(c))) digits.push_back(c);
          }
          if (digits.size() % 2 != 0) {
            return crow::response(400, json{{"error", "hex payload needs an even number of digits"}}.dump());
          }
          for (size_t i = 0; i + 1 < digits.size(); i += 2) {
            decoded.push_back(static_cast<char>(std::stoi(digits.substr(i, 2), nullptr, 16)));
          }
          payload = decoded;
        }
        if (body.value("append_cr", false)) payload.push_back('\r');

        // Bounded so a stuck request can't pin a Crow worker thread.
        int readMs = std::min(std::max(body.value("read_ms", 500), 0), 5000);

        SerialPort port;
        if (!port.Open(config)) {
          return crow::response(200, json{{"ok", false},
                                           {"error", "Failed to open " + config.port +
                                                         " - check the port exists and that nothing else "
                                                         "(including this gateway) already holds it."}}
                                          .dump());
        }

        bool wrote = payload.empty() ? true : port.Write(payload);

        std::string received;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(readMs);
        while (std::chrono::steady_clock::now() < deadline) {
          received += port.ReadAvailable();
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        received += port.ReadAvailable();
        port.Close();

        std::ostringstream hex;
        for (unsigned char c : received) {
          hex << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(c) << ' ';
        }

        return crow::response(200, json{{"ok", wrote},
                                         {"sent_bytes", payload.size()},
                                         {"received", received},
                                         {"received_hex", hex.str()},
                                         {"received_bytes", received.size()},
                                         {"error", wrote ? "" : "Port opened but the write failed."}}
                                        .dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/rest/test").methods("POST"_method)([this](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        RestConfig config = ConfigManager::Instance().GetRest();
        config.url = body.value("url", config.url);
        config.api_key = body.value("api_key", config.api_key);
        config.timeout_ms = body.value("timeout_ms", config.timeout_ms);
        config.ssl_enable = body.value("ssl_enable", config.ssl_enable);

        if (!rest) return crow::response(500, R"({"error":"REST client not available"})");
        RestResponse response = rest->TestConnection(config);
        return crow::response(200, json{{"ok", response.ok},
                                         {"status_code", response.status_code},
                                         {"error", response.error}}
                                        .dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/modbus/test").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        ModbusConfig config = ConfigManager::Instance().GetModbus();
        std::string ip = body.value("ip", config.ip);
        int port = body.value("port", config.port);

        std::string error;
        bool ok = ModbusClient::TestConnect(ip, port, error);
        return crow::response(200, json{{"ok", ok}, {"error", error}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Broker probe for the Configuration page. Uses the values in the form
    // rather than the saved ones, so credentials can be checked before they are
    // committed, and a throwaway connection so the gateway's own session is
    // untouched.
    CROW_ROUTE(app, "/api/mq/test").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        MqConfig config = ConfigManager::Instance().GetMq();
        config.host = body.value("host", config.host);
        config.port = body.value("port", config.port);
        config.vhost = body.value("vhost", config.vhost);
        config.user = body.value("user", config.user);
        config.password = body.value("password", config.password);

        std::string error;
        const bool ok = MqClient::TestConnect(config, error);
        return crow::response(200, json{{"ok", ok}, {"error", error}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Sends one message through the gateway's live client -- the "does the
    // whole path work" check, as opposed to /api/mq/test's handshake. Flushes
    // before answering so the reply reflects what actually left the process
    // rather than what got queued.
    CROW_ROUTE(app, "/api/mq/publish").methods("POST"_method)([this](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        if (!mq) {
          return crow::response(503, json{{"ok", false}, {"error", "MQ client not available"}}.dump());
        }

        const std::string text = body.value("body", std::string("eMaster Gateway test message"));
        const std::string routingKey = body.value("routing_key", std::string());
        const std::string exchange = body.value("exchange", std::string());
        const bool envelope = body.value("envelope", mq->GetConfig().envelope);

        std::string error;
        if (!mq->Publish(text, routingKey, exchange, envelope, error)) {
          return crow::response(200, json{{"ok", false}, {"error", error}}.dump());
        }

        const bool flushed = mq->Flush(3000);
        return crow::response(200, json{{"ok", flushed},
                                         {"error", flushed ? "" : "queued, but not delivered within 3s"},
                                         {"pending", mq->Status().pending}}
                                        .dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Mode-aware, because "is the reader working" means something different
    // per method: a TCP session for tcp_json, a PullSDK handshake for zk,
    // and mere reachability for card_api (where the reader dials us).
    CROW_ROUTE(app, "/api/rfid/test").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        RfidConfig config = ConfigManager::Instance().GetRfid();
        std::string mode = body.value("mode", config.mode);
        std::string ip = body.value("ip", config.ip);
        int port = body.value("port", config.port);
        int timeoutMs = body.value("timeout_ms", config.timeout_ms);

        if (mode == "card_api") {
          bool alive = PingHost(ip, timeoutMs, port);
          return crow::response(200, json{{"mode", mode},
                                           {"connected", alive},
                                           {"detail", alive ? "Reader is reachable."
                                                            : "No response to ping or TCP connect."}}
                                          .dump());
        }

        if (mode == "zk") {
          // A throwaway PullSdkClient, so this can't disturb the gateway's
          // own controller session. Reaching the port is not enough -- the
          // SDK handshake is what actually proves it's a ZK controller.
          ZkController probe;
          bool ok = probe.Connect(ip, port > 0 ? port : 4370, timeoutMs, "");
          std::string detail = ok ? "PullSDK connected." : ("PullSDK connect failed: " + probe.LastError());
          probe.Disconnect();
          return crow::response(200, json{{"mode", mode}, {"connected", ok}, {"detail", detail}}.dump());
        }

        // tcp_json
        TcpSocket socket;
        bool connected = socket.Connect(ip, port, timeoutMs);
        std::string data;
        bool gotData = connected && socket.Receive(data, 512, timeoutMs);
        socket.Close();

        // Reports the decoded card value when the payload parses, so the
        // test says whether data.Raw was actually found rather than only
        // that bytes arrived.
        std::string uid;
        bool parsed = gotData && RfidClient::ExtractRawFromJson(data, uid);

        return crow::response(200, json{{"mode", mode},
                                         {"connected", connected},
                                         {"data", gotData ? json(data) : json(nullptr)},
                                         {"raw", parsed ? json(uid) : json(nullptr)}}
                                        .dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Card Reader Client API (request/Request/cardInputRESTAPI.md): external
    // readers POST card UIDs here, authenticated by a per-client API-Key
    // rather than by talking to RFID hardware directly.
    CROW_ROUTE(app, "/api/card/input").methods("POST"_method)([this](const crow::request& req) {
      if (!ConfigManager::Instance().GetCardApi().enabled) {
        return crow::response(
            403, json{{"success", false}, {"code", 403}, {"message", "Card Reader Client API is disabled"}}
                     .dump());
      }

      std::string apiKey = TrimWhitespace(req.get_header_value("API-Key"));
      auto client = CardClientManager::Instance().Authenticate(apiKey);
      if (!client) {
        Logger::Instance().Warning(LogCategory::Card, "Rejected /api/card/input: invalid API-Key");
        return crow::response(
            401, json{{"success", false}, {"code", 401}, {"message", "Invalid API-Key"}}.dump());
      }

      std::string cardData;
      int position = 0;
      try {
        crow::multipart::message msg(req);
        cardData = msg.get_part_by_name("CardData").body;
        std::string positionStr = msg.get_part_by_name("Position").body;
        if (positionStr.empty()) throw std::invalid_argument("Position is required");
        position = std::stoi(positionStr);
      } catch (const std::exception& e) {
        return crow::response(
            400, json{{"success", false}, {"code", 400}, {"message", std::string("Invalid payload: ") + e.what()}}
                     .dump());
      }
      if (cardData.empty()) {
        return crow::response(400,
                               json{{"success", false}, {"code", 400}, {"message", "CardData is required"}}.dump());
      }

      CardCache::Instance().Set(cardData, position);
      Logger::Instance().Info(LogCategory::Card, "Card received from client '" + client->name + "': uid=" +
                                                       cardData + " position=" + std::to_string(position));
      // Complements the poll-based Card.Available()/Card.Get() convention
      // for scripts that prefer an event handler; queued (not dispatched
      // directly) since this runs on a Crow worker thread and a polling
      // script's RunSource holds the engine lock for its entire run — see
      // LuaEngine::QueueEvent's doc comment. No-ops if OnCardReceived isn't
      // defined.
      if (lua) lua->QueueEventAll("OnCardReceived", cardData);

      return crow::response(200, json{{"success", true}, {"message", "Card Accepted"}}.dump());
    });

    CROW_ROUTE(app, "/api/card/clients").methods("GET"_method, "POST"_method)([](const crow::request& req) {
      if (req.method == "POST"_method) {
        try {
          json body = json::parse(req.body);
          std::string name = body.value("name", "");
          if (name.empty()) return crow::response(400, R"({"error":"name is required"})");
          int64_t expiresAt = body.value("expires_at", static_cast<int64_t>(0));
          auto client = CardClientManager::Instance().CreateClient(name, expiresAt);
          if (!client) return crow::response(500, R"({"error":"failed to create client"})");
          Logger::Instance().Info(LogCategory::Card,
                                   "Created card client '" + name + "' (id " + std::to_string(client->id) + ")");
          return crow::response(200, CardClientManager::ToJson(*client).dump());
        } catch (const std::exception& e) {
          return crow::response(400, json{{"error", e.what()}}.dump());
        }
      }

      json arr = json::array();
      for (const auto& c : CardClientManager::Instance().ListClients()) arr.push_back(CardClientManager::ToJson(c));
      return crow::response(200, json{{"clients", arr}}.dump());
    });

    CROW_ROUTE(app, "/api/card/clients/<int>/enable")
        .methods("POST"_method)([](const crow::request&, int64_t id) {
          bool ok = CardClientManager::Instance().SetEnabled(id, true);
          return crow::response(ok ? 200 : 404, json{{"ok", ok}}.dump());
        });

    CROW_ROUTE(app, "/api/card/clients/<int>/disable")
        .methods("POST"_method)([](const crow::request&, int64_t id) {
          bool ok = CardClientManager::Instance().SetEnabled(id, false);
          return crow::response(ok ? 200 : 404, json{{"ok", ok}}.dump());
        });

    CROW_ROUTE(app, "/api/card/clients/<int>")
        .methods("DELETE"_method)([](const crow::request&, int64_t id) {
          bool ok = CardClientManager::Instance().DeleteClient(id);
          return crow::response(ok ? 200 : 404, json{{"ok", ok}}.dump());
        });

    CROW_ROUTE(app, "/api/card/cache").methods("GET"_method, "DELETE"_method)([](const crow::request& req) {
      if (req.method == "DELETE"_method) {
        CardCache::Instance().Clear();
        return crow::response(200, R"({"ok":true})");
      }
      auto entry = CardCache::Instance().Get();
      return crow::response(200, entry ? CardCache::ToJson(*entry).dump() : json(nullptr).dump());
    });

    CROW_ROUTE(app, "/api/variables")([] {
      return crow::response(200, RuntimeVariables::Instance().ToJson().dump());
    });

    // Named PLC I/O the running Lua script declared via
    // Modbus.RegisterInput/RegisterOutput (request/updateUI.md section 8).
    CROW_ROUTE(app, "/api/modbus/io")([] {
      return crow::response(200, ModbusRegistry::Instance().ToJson().dump());
    });

    CROW_ROUTE(app, "/api/lua/runtime")([this] {
      return crow::response(200, BuildLuaRuntimeJson().dump());
    });

    // Generic project-scoped dynamic configuration. Schemas are registered by
    // Lua projects through config.register_schema(); the web layer never
    // contains protocol-specific field logic.
    CROW_ROUTE(app, "/api/lua/projects")([] {
      json projects = json::array();
      for (const auto& project : DynamicConfigManager::Instance().Projects()) projects.push_back(project);
      return crow::response(200, projects.dump());
    });

    CROW_ROUTE(app, "/api/lua/projects/<string>/config/schema")([](const std::string& project) {
      json schema = DynamicConfigManager::Instance().Schema(project);
      if (schema.is_null()) return crow::response(404, json{{"error", "unknown Lua project"}}.dump());
      return crow::response(200, schema.dump());
    });

    CROW_ROUTE(app, "/api/lua/projects/<string>/config")
        .methods("GET"_method, "PUT"_method)([this](const crow::request& req, const std::string& project) {
          if (req.method == "GET"_method) {
            json values = DynamicConfigManager::Instance().Values(project, true);
            if (values.is_null()) return crow::response(404, json{{"error", "unknown Lua project"}}.dump());
            return crow::response(200, values.dump());
          }
          json body;
          try { body = json::parse(req.body); } catch (const std::exception& e) {
            return crow::response(400, json{{"success", false}, {"error", e.what()}}.dump());
          }
          json errors;
          if (!DynamicConfigManager::Instance().SetValues(project, body, errors))
            return crow::response(400, json{{"success", false}, {"errors", errors}}.dump());
          if (lua) lua->QueueEventAll("OnConfigChanged", project);
          return crow::response(200, json{{"success", true}, {"message", "Configuration applied"}}.dump());
        });

    // Lua script root directory (request/updateUI.md section 14). GET
    // reports the directory currently in use; POST validates a candidate
    // before adopting it -- section 14 asks the backend to check the path is
    // actually usable, and silently accepting a bad one would make every
    // script disappear from the manager with no explanation.
    CROW_ROUTE(app, "/api/lua/scripts-dir").methods("GET"_method, "POST"_method)([](const crow::request& req) {
      namespace fs = std::filesystem;

      if (req.method == "POST"_method) {
        std::string dir;
        try {
          dir = json::parse(req.body).value("path", "");
        } catch (const std::exception& e) {
          return crow::response(400, json{{"error", e.what()}}.dump());
        }
        if (dir.empty()) return crow::response(400, R"({"error":"path is required"})");

        std::error_code ec;
        if (!fs::exists(dir, ec) || ec) {
          return crow::response(400, json{{"error", "Directory does not exist: " + dir}}.dump());
        }
        if (!fs::is_directory(dir, ec) || ec) {
          return crow::response(400, json{{"error", "Not a directory: " + dir}}.dump());
        }
        // Writable matters as much as readable -- the manager creates,
        // renames and deletes scripts here, and a read-only directory would
        // only fail later, at save time.
        fs::path probe = fs::path(dir) / ".hsf_write_probe";
        std::ofstream probeFile(probe);
        if (!probeFile.is_open()) {
          return crow::response(400, json{{"error", "Directory is not writable: " + dir}}.dump());
        }
        probeFile.close();
        fs::remove(probe, ec);

        // Stored as the startup script path, since that is what ScriptsDir()
        // derives the directory from. Keeps the existing startup script's
        // filename if there is one, so pointing at a new folder doesn't
        // silently unset auto-run.
        LuaConfig cfg = ConfigManager::Instance().GetLua();
        std::string fileName = fs::path(cfg.script_path).filename().string();
        cfg.script_path = fileName.empty() ? (fs::path(dir) / "main.lua").generic_string()
                                            : (fs::path(dir) / fileName).generic_string();
        ConfigManager::Instance().SetLua(cfg);
        ConfigManager::Instance().Save();
        Logger::Instance().Info(LogCategory::Lua, "Lua script directory set to " + dir);
      }

      std::string dir = ScriptsDir();
      std::error_code ec;
      bool exists = fs::exists(dir, ec) && !ec;
      return crow::response(200, json{{"path", dir}, {"exists", exists}}.dump());
    });

    // Manual output test from the dashboard (section 9). Addressed BY NAME
    // and resolved through the registry, so this can only drive coils a
    // script deliberately registered -- an address parameter here would
    // make it a write-any-coil primitive on a live PLC.
    //
    // NOT AUTHENTICATED. Section 9 restricts this to Admin and section 34
    // requires the backend to enforce that, but the gateway has no server-
    // side auth yet -- the dashboard only hides the button. Anyone who can
    // reach this port can actuate a registered output.
    CROW_ROUTE(app, "/api/modbus/output").methods("POST"_method)([this](const crow::request& req) {
      if (!modbus) return crow::response(503, R"({"error":"modbus not available"})");

      std::string name;
      bool value = false;
      try {
        json body = json::parse(req.body);
        name = body.at("name").get<std::string>();
        value = body.at("value").get<bool>();
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", std::string("invalid payload: ") + e.what()}}.dump());
      }

      int address = 0;
      if (!ModbusRegistry::Instance().FindOutputAddress(name, address)) {
        return crow::response(404, json{{"error", "no registered output named '" + name + "'"}}.dump());
      }

      if (!modbus->IsConnected()) {
        return crow::response(503, json{{"error", "PLC not connected"}}.dump());
      }

      bool ok = modbus->WriteCoil(address, value);
      if (ok) {
        // Reflect immediately rather than waiting for the next poll, so the
        // button doesn't appear to do nothing for up to a poll interval.
        ModbusRegistry::Instance().UpdateOutput(name, value);
        Logger::Instance().Info(LogCategory::Modbus, "Manual output test: " + name + " (coil " +
                                                          std::to_string(address) + ") set to " +
                                                          (value ? "ON" : "OFF"));
      }
      return crow::response(ok ? 200 : 500,
                             json{{"ok", ok}, {"name", name}, {"address", address}, {"value", value}}.dump());
    });

    CROW_ROUTE(app, "/api/logs")([](const crow::request& req) {
      auto* categoryParam = req.url_params.get("category");
      auto* limitParam = req.url_params.get("limit");
      size_t limit = limitParam ? static_cast<size_t>(std::atoi(limitParam)) : 200;

      LogCategory category = LogCategory::System;
      const LogCategory* categoryPtr = nullptr;
      if (categoryParam && std::string(categoryParam) != "all") {
        category = Logger::CategoryFromString(categoryParam);
        categoryPtr = &category;
      }

      auto entries = Logger::Instance().Recent(categoryPtr, limit);
      json arr = json::array();
      for (const auto& e : entries) {
        arr.push_back({{"seq", e.seq},
                        {"timestamp", e.timestamp},
                        {"level", Logger::LevelToString(e.level)},
                        {"category", Logger::CategoryToString(e.category)},
                        {"message", e.message}});
      }
      return crow::response(200, arr.dump());
    });

    // --- structured logs (request/upgrade.md sections 13-16) ---------------
    //
    // Separate from /api/logs above, which serves the runtime tail from the
    // in-memory ring buffer. These read the SQLite store Log.Write() fills.

    // Log type definitions, for the type selector and to label the detail
    // view's fields in declaration order.
    CROW_ROUTE(app, "/api/logs/definitions")([] {
      json types = json::array();
      for (const auto& def : LogStore::Instance().Definitions()) {
        json fields = json::array();
        for (const auto& field : def.fields) {
          fields.push_back({{"name", field.name}, {"type", field.type}, {"required", field.required}});
        }
        types.push_back({{"type", def.type}, {"description", def.description}, {"fields", fields}});
      }
      // Script names come from the entries themselves rather than from the
      // definitions: section 14's Script filter should offer what has actually
      // logged, not every script file on disk.
      json scripts = json::array();
      for (const auto& script : LogStore::Instance().Scripts()) scripts.push_back(script);

      return crow::response(200, json{{"types", types},
                                       {"scripts", scripts},
                                       {"total_entries", LogStore::Instance().Count()},
                                       {"available", LogStore::Instance().IsOpen()}}
                                     .dump());
    });

    // Search. All filters optional; `type` may be repeated or comma-separated,
    // since section 13's selector is a multi-select.
    CROW_ROUTE(app, "/api/logs/structured")([](const crow::request& req) {
      LogQuery query;

      auto param = [&req](const char* key) -> std::string {
        auto* value = req.url_params.get(key);
        return value ? TrimWhitespace(value) : std::string();
      };

      // Crow returns repeated parameters via get_list; a single comma-joined
      // value is accepted too, because that is the easier thing to build from
      // JS and there is no legitimate comma inside a log type name.
      for (const std::string& value : req.url_params.get_list("type", false)) {
        std::istringstream parts(value);
        std::string one;
        while (std::getline(parts, one, ',')) {
          std::string trimmed = TrimWhitespace(one);
          if (!trimmed.empty() && trimmed != "all") query.types.push_back(trimmed);
        }
      }

      query.level = param("level");
      query.script = param("script");
      query.keyword = param("keyword");
      query.from = param("from");
      query.to = param("to");

      // The date pickers produce "2026-08-01T00:00"; SQLite's DATETIME text
      // (and this store's own timestamps) use a space. Normalising here means
      // the frontend can send its input value untouched.
      auto normalise = [](std::string& value) {
        auto t = value.find('T');
        if (t != std::string::npos) value[t] = ' ';
      };
      normalise(query.from);
      normalise(query.to);

      if (auto* limit = req.url_params.get("limit")) query.limit = std::atoi(limit);
      if (auto* offset = req.url_params.get("offset")) query.offset = std::atoi(offset);

      return crow::response(200, LogStore::Instance().Query(query).dump());
    });

    // One entry with every field, for the detail view (section 15).
    CROW_ROUTE(app, "/api/logs/structured/<int>")([](int id) {
      json entry = LogStore::Instance().Entry(id);
      if (entry.is_null()) return crow::response(404, R"({"error":"no such log entry"})");
      return crow::response(200, entry.dump());
    });

    // Clears the in-memory buffer the viewer reads. The log file on disk is
    // untouched -- see Logger::Clear.
    CROW_ROUTE(app, "/api/logs/clear").methods("POST"_method)([] {
      Logger::Instance().Clear();
      Logger::Instance().Info(LogCategory::System, "Log view cleared from the web UI");
      return crow::response(200, R"({"ok":true})");
    });

    CROW_ROUTE(app, "/api/lua/script").methods("GET"_method, "POST"_method)([](const crow::request& req) {
      std::string path = ConfigManager::Instance().GetLua().script_path;
      if (req.method == "POST"_method) {
        try {
          json body = json::parse(req.body);
          std::string code = body.value("code", "");
          std::ofstream out(path);
          if (!out.is_open()) return crow::response(500, R"({"error":"failed to write script file"})");
          out << code;
          return crow::response(200, R"({"ok":true})");
        } catch (const std::exception& e) {
          return crow::response(400, json{{"error", e.what()}}.dump());
        }
      }
      std::string code;
      if (!ReadFileToString(path, code)) return crow::response(404, R"({"error":"script file not found"})");
      return crow::response(200, json{{"code", code}}.dump());
    });

    // Multi-file script manager: lists/reads/writes/deletes/runs any .lua
    // file under the scripts directory, and can promote one to be the
    // gateway's startup script (config.lua.script_path). The single-file
    // /api/lua/script route above is left as-is for backward compatibility
    // — it always targets whatever the current startup script is.
    CROW_ROUTE(app, "/api/lua/scripts")([this] {
      namespace fs = std::filesystem;
      std::string dir = ScriptsDir();

      // Runtime state per script name, so the editor can show STOPPED /
      // RUNNING / ERROR against each file and pre-tick what is already running
      // (request/upgrade.md sections 21 and 26) in the same request that lists
      // the files.
      std::map<std::string, json> states;
      if (lua) {
        for (const auto& status : lua->Statuses()) {
          states[status.name] = {{"state", status.state},
                                  {"runtime_id", status.runtime_id},
                                  {"last_error", status.last_error}};
        }
      }

      // Relative to the scripts directory, not just the filename, so a
      // startup script inside a subfolder still matches the tree entry the
      // UI marks as default.
      std::string configured = ConfigManager::Instance().GetLua().script_path;
      std::string defaultName;
      if (!configured.empty()) {
        std::error_code relEc;
        fs::path rel = fs::relative(configured, dir, relEc);
        defaultName = relEc ? fs::path(configured).filename().string() : rel.generic_string();
      }

      json scripts = json::array();
      std::error_code ec;
      if (fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
        // Recursive, so scripts can be organised into folders
        // (request/updateUI.md section 13). Paths are reported relative to
        // the scripts directory with forward slashes, which is exactly what
        // the read/write/delete/run routes accept back.
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (!ec) {
          for (const auto& entry : it) {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc) continue;
            if (entry.path().extension() != ".lua") continue;

            std::error_code relEc;
            fs::path rel = fs::relative(entry.path(), dir, relEc);
            if (relEc) continue;

            std::error_code sizeEc;
            auto size = entry.file_size(sizeEc);

            std::string name = rel.generic_string();
            json script = {{"name", name}, {"size", sizeEc ? 0 : size}};

            auto stateIt = states.find(name);
            if (stateIt != states.end()) {
              script["state"] = stateIt->second["state"];
              script["runtime_id"] = stateIt->second["runtime_id"];
              script["last_error"] = stateIt->second["last_error"];
            } else {
              // A file that has never been run this session. Reported as
              // STOPPED rather than omitted, so the UI has one state vocabulary
              // for every row.
              script["state"] = "STOPPED";
              script["runtime_id"] = 0;
              script["last_error"] = "";
            }
            scripts.push_back(script);
          }
        }
      }
      return crow::response(200, json{{"scripts", scripts}, {"default_name", defaultName}}.dump());
    });

    // The script name travels as a ?name= query parameter rather than a path
    // segment. Crow's <string> capture matches ONE segment and stops at '/',
    // so "test/probe.lua" could never route through it, and pre-encoding the
    // slash as %2F doesn't help either -- Crow hands the capture over
    // undecoded, and IsValidScriptPath rejects '%' on purpose. Query
    // parameters are decoded, so a subfolder path arrives intact.
    CROW_ROUTE(app, "/api/lua/scripts/file")
        .methods("GET"_method, "POST"_method, "DELETE"_method)([](const crow::request& req) {
          auto* nameParam = req.url_params.get("name");
          std::string name = nameParam ? nameParam : "";
          std::string path;
          if (!ResolveScriptPath(name, path)) {
            return crow::response(400, R"({"error":"invalid script name"})");
          }

          if (req.method == "DELETE"_method) {
            std::error_code ec;
            bool removed = std::filesystem::remove(path, ec);
            if (!removed || ec) return crow::response(404, R"({"error":"script not found"})");
            return crow::response(200, R"({"ok":true})");
          }

          if (req.method == "POST"_method) {
            try {
              json body = json::parse(req.body);
              std::string code = body.value("code", "");

              // "card/issue.lua" has to be creatable before the card/ folder
              // exists, otherwise saving into a new folder fails with
              // nothing the UI can act on. IsValidScriptPath has already
              // ruled out escaping the scripts directory.
              std::error_code dirEc;
              std::filesystem::create_directories(std::filesystem::path(path).parent_path(), dirEc);

              std::ofstream out(path);
              if (!out.is_open()) return crow::response(500, R"({"error":"failed to write script file"})");
              out << code;
              return crow::response(200, R"({"ok":true})");
            } catch (const std::exception& e) {
              return crow::response(400, json{{"error", e.what()}}.dump());
            }
          }

          std::string code;
          if (!ReadFileToString(path, code)) return crow::response(404, R"({"error":"script not found"})");
          return crow::response(200, json{{"code", code}, {"name", name}}.dump());
        });

    CROW_ROUTE(app, "/api/lua/scripts/folder/delete").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = json::parse(req.body);
        const std::string name = body.value("name", "");
        if (name.empty() || name.find("..") != std::string::npos || name.front() == '/' ||
            name.find('\\') != std::string::npos || name.find("//") != std::string::npos) {
          return crow::response(400, R"({"error":"invalid folder name"})");
        }

        namespace fs = std::filesystem;
        const fs::path base = fs::weakly_canonical(ScriptsDir());
        const fs::path folder = fs::weakly_canonical(base / name);
        const std::string baseText = base.generic_string();
        const std::string folderText = folder.generic_string();
        if (folderText.size() <= baseText.size() || folderText.compare(0, baseText.size(), baseText) != 0 ||
            folderText[baseText.size()] != '/') {
          return crow::response(400, R"({"error":"folder is outside the scripts directory"})");
        }
        std::error_code ec;
        if (!fs::is_directory(folder, ec) || ec) return crow::response(404, R"({"error":"folder not found"})");
        const auto removed = fs::remove_all(folder, ec);
        if (ec) return crow::response(500, json{{"error", "failed to delete folder: " + ec.message()}}.dump());
        Logger::Instance().Info(LogCategory::Lua, "Deleted Lua script folder " + name);
        return crow::response(200, json{{"ok", true}, {"removed", removed}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/lua/scripts/import").methods("POST"_method)([](const crow::request& req) {
      try {
        crow::multipart::message msg(req);
        const std::string name = msg.get_part_by_name("name").body;
        const std::string code = msg.get_part_by_name("script").body;
        std::string path;
        if (!ResolveScriptPath(name, path) || std::filesystem::path(name).extension() != ".lua") {
          return crow::response(400, R"({"error":"invalid Lua script name"})");
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return crow::response(500, R"({"error":"failed to save script"})");
        out.write(code.data(), static_cast<std::streamsize>(code.size()));
        return crow::response(200, json{{"ok", true}, {"name", name}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"ok", false}, {"error", e.what()}}.dump());
      }
    });

    // Runs one script (?name=) or several at once (JSON body {"names":[...]}),
    // each in its own runtime -- request/upgrade.md sections 20 and 26's "Run
    // Selected". One script failing to start does not stop the others from
    // being started, so the response reports per-script results rather than a
    // single ok/error the UI couldn't attribute.
    CROW_ROUTE(app, "/api/lua/scripts/run").methods("POST"_method)([this](const crow::request& req) {
      std::vector<std::string> names = ScriptNamesFromRequest(req);
      if (names.empty()) return crow::response(400, R"({"error":"no script name given"})");
      if (!lua) return crow::response(500, R"({"error":"Lua runtime not available"})");

      json results = json::array();
      bool allOk = true;
      for (const std::string& name : names) {
        if (!IsValidScriptPath(name)) {
          results.push_back({{"name", name}, {"ok", false}, {"error", "invalid script name"}});
          allOk = false;
          continue;
        }

        std::string error;
        auto result = lua->StartScript(name, error);
        // "Already running" is reported as its own outcome, not as a failure:
        // section 27 wants the UI to say "Already Running", and a plain error
        // would read as though the script were broken.
        results.push_back({{"name", name},
                            {"ok", result == LuaRuntimeManager::StartResult::kStarted},
                            {"already_running", result == LuaRuntimeManager::StartResult::kAlreadyRunning},
                            {"error", error}});
        if (result == LuaRuntimeManager::StartResult::kFailed) allOk = false;
      }

      return crow::response(200, json{{"ok", allOk}, {"results", results}}.dump());
    });

    // Stops one script (?name=) or several ({"names":[...]}), independently of
    // every other running script (section 22).
    CROW_ROUTE(app, "/api/lua/scripts/stop").methods("POST"_method)([this](const crow::request& req) {
      std::vector<std::string> names = ScriptNamesFromRequest(req);
      if (names.empty()) return crow::response(400, R"({"error":"no script name given"})");
      if (!lua) return crow::response(500, R"({"error":"Lua runtime not available"})");

      json results = json::array();
      for (const std::string& name : names) {
        bool stopped = lua->Stop(name);
        results.push_back({{"name", name}, {"ok", stopped}, {"error", stopped ? "" : "not running"}});
      }
      return crow::response(200, json{{"ok", true}, {"results", results}}.dump());
    });

    // Rename / move (request/updateUI.md section 15). Both names go through
    // the same validator, so a rename can't be used to write outside the
    // scripts directory -- and moving into a folder that doesn't exist yet
    // creates it, matching the save behaviour.
    CROW_ROUTE(app, "/api/lua/scripts/rename").methods("POST"_method)([](const crow::request& req) {
      std::string from, to;
      try {
        json body = json::parse(req.body);
        from = body.value("from", "");
        to = body.value("to", "");
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }

      if (!IsValidScriptPath(from) || !IsValidScriptPath(to)) {
        return crow::response(400, R"({"error":"invalid script name"})");
      }
      if (from == to) return crow::response(200, R"({"ok":true})");

      namespace fs = std::filesystem;
      std::string dir = ScriptsDir();
      fs::path src = fs::path(dir) / from;
      fs::path dst = fs::path(dir) / to;

      std::error_code ec;
      if (!fs::exists(src, ec) || ec) {
        return crow::response(404, json{{"error", "script not found: " + from}}.dump());
      }
      // Checked rather than letting rename silently clobber -- overwriting
      // another script with no warning is not recoverable from the UI.
      if (fs::exists(dst, ec)) {
        return crow::response(409, json{{"error", "a script named '" + to + "' already exists"}}.dump());
      }

      fs::create_directories(dst.parent_path(), ec);
      fs::rename(src, dst, ec);
      if (ec) {
        return crow::response(500, json{{"error", "rename failed: " + ec.message()}}.dump());
      }

      // The startup script is stored as a full path, so renaming the file it
      // points at would otherwise leave auto-run aimed at something gone.
      LuaConfig cfg = ConfigManager::Instance().GetLua();
      std::error_code relEc;
      fs::path rel = fs::relative(cfg.script_path, dir, relEc);
      if (!relEc && rel.generic_string() == from) {
        cfg.script_path = dst.generic_string();
        ConfigManager::Instance().SetLua(cfg);
        ConfigManager::Instance().Save();
      }

      Logger::Instance().Info(LogCategory::Lua, "Renamed script " + from + " -> " + to);
      return crow::response(200, json{{"ok", true}, {"name", to}}.dump());
    });

    CROW_ROUTE(app, "/api/lua/scripts/set-default").methods("POST"_method)([](const crow::request& req) {
      auto* nameParam = req.url_params.get("name");
      std::string name = nameParam ? nameParam : "";
      std::string resolved;
      if (!ResolveScriptPath(name, resolved)) {
        return crow::response(400, R"({"error":"invalid script name"})");
      }
      LuaConfig config = ConfigManager::Instance().GetLua();
      config.script_path = resolved;
      ConfigManager::Instance().SetLua(config);
      ConfigManager::Instance().Save();
      return crow::response(200, R"({"ok":true})");
    });

    // Unchecking a script's "Auto-run on startup" box: no script runs
    // automatically until another one is checked. main.cpp's startup
    // thread logs and continues (doesn't crash) when this is empty.
    CROW_ROUTE(app, "/api/lua/clear-default").methods("POST"_method)([] {
      LuaConfig config = ConfigManager::Instance().GetLua();
      config.script_path = "";
      ConfigManager::Instance().SetLua(config);
      ConfigManager::Instance().Save();
      return crow::response(200, R"({"ok":true})");
    });

    CROW_ROUTE(app, "/api/lua/validate").methods("POST"_method)([](const crow::request& req) {
      try {
        json body = json::parse(req.body);
        std::string code = body.value("code", "");
        std::string error;
        // Compile-only, so this works even while every runtime is busy.
        bool ok = LuaRuntimeManager::Validate(code, error);
        return crow::response(200, json{{"ok", ok}, {"error", error}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // Runs the editor's buffer, which has no file behind it. Its own runtime,
    // listed as "(editor buffer)" -- so testing a snippet no longer stops
    // whatever scripts are running from disk.
    CROW_ROUTE(app, "/api/lua/run").methods("POST"_method)([this](const crow::request& req) {
      try {
        json body = json::parse(req.body);
        std::string code = body.value("code", "");
        std::string error;
        bool ok = lua && lua->StartSource(code, error) == LuaRuntimeManager::StartResult::kStarted;
        return crow::response(200, json{{"ok", ok}, {"error", error}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", e.what()}}.dump());
      }
    });

    // ?name= stops that one runtime; without it, every runtime. Kept
    // permissive because the editor's Stop button is the "make it all stop"
    // control an operator reaches for when something is misbehaving.
    CROW_ROUTE(app, "/api/lua/stop").methods("POST"_method)([this](const crow::request& req) {
      if (!lua) return crow::response(500, R"({"error":"Lua runtime not available"})");

      auto* nameParam = req.url_params.get("name");
      if (nameParam && *nameParam) {
        bool stopped = lua->Stop(nameParam);
        return crow::response(200, json{{"ok", stopped}, {"error", stopped ? "" : "not running"}}.dump());
      }

      lua->StopAll();
      return crow::response(200, R"({"ok":true})");
    });

    CROW_ROUTE(app, "/api/lua/restart").methods("POST"_method)([this](const crow::request& req) {
      if (!lua) return crow::response(500, R"({"error":"Lua runtime not available"})");

      auto* nameParam = req.url_params.get("name");
      std::string name = nameParam ? nameParam : LuaRuntimeManager::EditorBufferName();
      std::string error;
      bool ok = lua->Restart(name, error);
      return crow::response(200, json{{"ok", ok}, {"error", error}}.dump());
    });

    // Full runtime list (request/upgrade.md sections 21 and 24). Same array
    // /api/lua/runtime carries; separate route because this one is about the
    // runtimes themselves rather than the dashboard's aggregate status.
    CROW_ROUTE(app, "/api/lua/runtimes")([this] {
      json runtime = BuildLuaRuntimeJson();
      return crow::response(200, json{{"runtimes", runtime["scripts"]},
                                       {"running_count", runtime.value("running_count", 0)}}
                                     .dump());
    });

    // Forgets runtimes that have stopped or failed, so the list stops showing
    // them. The only way to clear an ERROR row short of re-running the script.
    CROW_ROUTE(app, "/api/lua/runtimes/prune").methods("POST"_method)([this] {
      int removed = lua ? lua->Prune() : 0;
      return crow::response(200, json{{"ok", true}, {"removed", removed}}.dump());
    });

    // --- routes served by a Lua script ------------------------------------
    //
    // Everything under /api/app/ is forwarded to whichever running script
    // registered that path with Http.Register. Crow builds its routing table at
    // startup, so a script cannot add a route of its own -- these three fixed
    // patterns (one, two and three path segments) are the door, and the path
    // below the prefix is what the script matches on.
    //
    // The prefix keeps script-served paths in their own namespace: no script can
    // shadow /api/config or /api/lua/*, whatever it registers.
    CROW_ROUTE(app, "/api/app/<string>")
        .methods("GET"_method, "POST"_method, "PUT"_method, "DELETE"_method, "PATCH"_method)(
            [this](const crow::request& req, const std::string& a) { return ServeLuaRoute(req, a); });

    CROW_ROUTE(app, "/api/app/<string>/<string>")
        .methods("GET"_method, "POST"_method, "PUT"_method, "DELETE"_method, "PATCH"_method)(
            [this](const crow::request& req, const std::string& a, const std::string& b) {
              return ServeLuaRoute(req, a + "/" + b);
            });

    CROW_ROUTE(app, "/api/app/<string>/<string>/<string>")
        .methods("GET"_method, "POST"_method, "PUT"_method, "DELETE"_method, "PATCH"_method)(
            [this](const crow::request& req, const std::string& a, const std::string& b,
                    const std::string& c) { return ServeLuaRoute(req, a + "/" + b + "/" + c); });

    // What is currently served under /api/app/, so a 404 there can be
    // diagnosed without reading the script.
    CROW_ROUTE(app, "/api/app")([this] {
      json routes = json::array();
      if (lua) {
        for (const std::string& route : lua->HttpRoutes()) routes.push_back(route);
      }
      return crow::response(200, json{{"prefix", "/api/app/"}, {"routes", routes}}.dump());
    });

    // --- over-the-air updates (request/CICD.md) ---------------------------
    //
    // Actions are REST, state is WebSocket. request/CICD.md sketched the
    // update_request going back up the socket, but /ws here has always been
    // push-only, and a POST gives the browser something the socket cannot: a
    // synchronous answer saying why a request was refused ("no manifest for
    // that version", "already in progress") instead of silence.
    CROW_ROUTE(app, "/api/update/status")([this] {
      if (!update) return crow::response(200, json{{"state", "disabled"}, {"enabled", false}}.dump());
      return crow::response(200, update->StatusJson().dump());
    });

    CROW_ROUTE(app, "/api/update/check").methods("POST"_method)([this] {
      if (!update) return crow::response(503, json{{"error", "update manager unavailable"}}.dump());
      update->CheckNow();
      return crow::response(202, json{{"ok", true}, {"status", update->StatusJson()}}.dump());
    });

    CROW_ROUTE(app, "/api/update/install").methods("POST"_method)([this](const crow::request& req) {
      if (!update) return crow::response(503, json{{"error", "update manager unavailable"}}.dump());
      json body = json::object();
      try {
        if (!req.body.empty()) body = json::parse(req.body);
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", std::string("invalid JSON: ") + e.what()}}.dump());
      }
      const std::string version = body.value("version", std::string());
      if (version.empty()) return crow::response(400, json{{"error", "version is required"}}.dump());

      std::string error;
      if (!update->Install(version, error)) {
        return crow::response(409, json{{"error", error}}.dump());
      }
      Logger::Instance().Info(LogCategory::System, "Update to " + version + " requested from the web UI");
      return crow::response(202, json{{"ok", true}, {"status", update->StatusJson()}}.dump());
    });

    CROW_ROUTE(app, "/api/update/dismiss").methods("POST"_method)([this](const crow::request& req) {
      if (!update) return crow::response(503, json{{"error", "update manager unavailable"}}.dump());
      json body = json::object();
      try {
        if (!req.body.empty()) body = json::parse(req.body);
      } catch (const std::exception& e) {
        return crow::response(400, json{{"error", std::string("invalid JSON: ") + e.what()}}.dump());
      }
      std::string error;
      if (!update->Dismiss(body.value("version", std::string()), error)) {
        return crow::response(409, json{{"error", error}}.dump());
      }
      return crow::response(200, json{{"ok", true}}.dump());
    });

    SetupAuthRoutes();
    SetupPackageRoutes();
    SetupPluginRoutes();
    SetupTestToolRoutes();

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        // The realtime channel carries system status, runtime variables and
        // the live log tail -- everything the dashboard shows. Leaving it open
        // while the REST API behind it is authenticated would be a hole the
        // size of the dashboard.
        //
        // The token arrives as a QUERY PARAMETER, not a header, because the
        // browser WebSocket API has no way to set one. That has a real cost:
        // query strings turn up in proxy logs in a way Authorization headers
        // do not, which is the same trade the WebSocket protocol forces on
        // everyone. It is mitigated by the token being short-lived and
        // revocable, and by the gateway not logging the query string of an
        // upgrade -- but it is the weakest link in this scheme and is called
        // out in docs/security.md rather than hidden.
        .onaccept([](const crow::request& req, void**) {
          const AuthConfig config = ConfigManager::Instance().GetAuth();
          if (!config.enabled) return true;

          std::string token;
          if (const char* fromQuery = req.url_params.get("token")) token = fromQuery;
          // A non-browser client (a test harness, curl) can still use the
          // header, and should.
          if (token.empty()) token = SecurityMiddleware::BearerToken(req);
          if (token.empty()) return false;

          auto session = SecurityStore::Instance().Authenticate(token);
          if (!session) return false;
          // Reading the dashboard stream is the same right as reading the
          // dashboard.
          return RoleHasPermission(session->role, Permission::kDeviceRead);
        })
        .onopen([this](crow::websocket::connection& conn) {
          std::lock_guard<std::mutex> lock(wsMutex);
          wsConnections.push_back(&conn);
        })
        .onclose([this](crow::websocket::connection& conn, const std::string&, uint16_t) {
          std::lock_guard<std::mutex> lock(wsMutex);
          wsConnections.erase(std::remove(wsConnections.begin(), wsConnections.end(), &conn), wsConnections.end());
        })
        .onmessage([](crow::websocket::connection&, const std::string&, bool) {
          // Realtime channel is currently push-only from server to client;
          // inbound messages are ignored.
        });
  }

  // --- production Lua packaging (request/AdvanceUpdate.md Phase 2) --------
  //
  // Compile, Build & Test, Deploy and Rollback. Each is gated by its own
  // permission in the policy table -- LUA_COMPILE is a build-machine activity
  // and LUA_DEPLOY changes what the cabinet runs, so an installation can grant
  // one without the other.
  void SetupPluginRoutes() {
    auto text = [](HSFStr value) {
      return value.ptr ? std::string(value.ptr, value.len) : std::string();
    };

    CROW_ROUTE(app, "/api/plugins")([this] {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      json result = plugins->ToJson();
      const json configured = ConfigManager::Instance().GetCategory("plugins");
      for (auto& plugin : result["plugins"]) {
        const std::string id = plugin.value("id", std::string());
        plugin["enabled_on_startup"] = configured.is_object() && configured.contains(id) &&
                                       configured[id].is_object() &&
                                       configured[id].value("enabled", false);
      }
      return crow::response(200, result.dump());
    });

    CROW_ROUTE(app, "/api/plugins/discover").methods("POST"_method)([this] {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      const size_t count = plugins->Discover();
      return crow::response(200, json{{"ok", true}, {"count", count}}.dump());
    });

    CROW_ROUTE(app, "/api/plugins/<string>/config").methods("GET"_method, "POST"_method)(
        [this](const crow::request& req, std::string id) {
          if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
          json all = ConfigManager::Instance().GetCategory("plugins");
          if (!all.is_object()) all = json::object();
          if (req.method == "POST"_method) {
            json body;
            try { body = json::parse(req.body); } catch (const std::exception& e) {
              return crow::response(400, json{{"error", std::string("invalid JSON: ") + e.what()}}.dump());
            }
            if (!body.is_object()) return crow::response(400, json{{"error", "configuration must be a JSON object"}}.dump());
            all[id] = body;
            if (!ConfigManager::Instance().ApplyJson(json{{"plugins", all}}) || !ConfigManager::Instance().Save()) {
              return crow::response(500, json{{"error", "could not save plugin configuration"}}.dump());
            }
            plugins->SetConfiguration(all);
            // Config is captured when the plugin instance is loaded. Unload
            // it so the next Enable reads the new section instead of silently
            // retaining the old transport/driver values.
            std::string unloadError;
            plugins->Unload(id, unloadError);
            plugins->Discover();
            if (body.value("enabled", false)) {
              std::string enableError;
              if (!plugins->Enable(id, enableError)) {
                return crow::response(400, json{{"error", "configuration saved but plugin could not start: " +
                                                          enableError}}.dump());
              }
            }
          }
          return crow::response(200, json{{"ok", true}, {"id", id}, {"config", all.value(id, json::object())}}.dump());
        });

    CROW_ROUTE(app, "/api/plugins/import").methods("POST"_method)([this](const crow::request& req) {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      try {
        crow::multipart::message msg(req);
        const std::string manifest = msg.get_part_by_name("manifest").body;
        const std::string binary = msg.get_part_by_name("plugin").body;
        std::vector<unsigned char> bytes(binary.begin(), binary.end());
        std::string id, error;
        if (!plugins->ImportUploaded(manifest, bytes, id, error)) return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
        return crow::response(200, json{{"ok", true}, {"id", id}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"ok", false}, {"error", e.what()}}.dump());
      }
    });

    auto packageOperation = [this](const crow::request& req, bool update) {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      json body;
      std::string parse_error;
      if (!Validate::JsonObject(req.body, 4096, body, parse_error)) {
        return crow::response(400, json{{"ok", false}, {"error", "invalid JSON"}}.dump());
      }
      std::string source_dir;
      Validate::Errors errors;
      Validate::StringField(body, "source_dir", 1, 512, "", source_dir, errors);
      if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());
      std::string id, error;
      if (!plugins->Install(source_dir, id, error)) {
        return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
      }
      return crow::response(200, json{{"ok", true}, {"operation", update ? "update" : "install"}, {"id", id}}.dump());
    };
    CROW_ROUTE(app, "/api/plugins/install").methods("POST"_method)(
        [packageOperation](const crow::request& req) { return packageOperation(req, false); });
    CROW_ROUTE(app, "/api/plugins/update").methods("POST"_method)(
        [packageOperation](const crow::request& req) { return packageOperation(req, true); });
    CROW_ROUTE(app, "/api/plugins/rollback/<string>").methods("POST"_method)([this](const crow::request&, std::string id) {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      std::string error;
      if (!plugins->Rollback(id, error)) return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
      return crow::response(200, json{{"ok", true}, {"id", id}}.dump());
    });
    CROW_ROUTE(app, "/api/plugins/<string>/delete").methods("POST"_method)([this](const crow::request&, std::string id) {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      std::string error;
      if (!plugins->Remove(id, error)) return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
      return crow::response(200, json{{"ok", true}, {"id", id}}.dump());
    });

    auto lifecycle = [this](const std::string& id, const std::string& operation) {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      std::string error;
      bool ok = false;
      if (operation == "enable") ok = plugins->Enable(id, error);
      else if (operation == "disable") ok = plugins->Disable(id, error);
      else if (operation == "unload") ok = plugins->Unload(id, error);
      if (!ok) return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
      if (operation == "enable" || operation == "disable") {
        json all = ConfigManager::Instance().GetCategory("plugins");
        if (!all.is_object()) all = json::object();
        if (!all.contains(id) || !all[id].is_object()) all[id] = json::object();
        all[id]["enabled"] = operation == "enable";
        if (!ConfigManager::Instance().ApplyJson(json{{"plugins", all}}) ||
            !ConfigManager::Instance().Save()) {
          return crow::response(500, json{{"ok", false},
                                           {"error", "plugin state changed but startup state could not be saved"}}
                                          .dump());
        }
        plugins->SetConfiguration(all);
      }
      PluginRecord record;
      plugins->Get(id, &record);
      return crow::response(200, json{{"ok", true}, {"plugin", record.ToJson()}}.dump());
    };

    CROW_ROUTE(app, "/api/plugins/<string>/enable").methods("POST"_method)(
        [lifecycle](const crow::request&, std::string id) { return lifecycle(id, "enable"); });
    CROW_ROUTE(app, "/api/plugins/<string>/disable").methods("POST"_method)(
        [lifecycle](const crow::request&, std::string id) { return lifecycle(id, "disable"); });
    CROW_ROUTE(app, "/api/plugins/<string>/unload").methods("POST"_method)(
        [lifecycle](const crow::request&, std::string id) { return lifecycle(id, "unload"); });

    CROW_ROUTE(app, "/api/plugins/<string>/test/health").methods("POST"_method)(
        [this](const crow::request&, std::string id) {
          if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
          std::string error;
          const bool ok = plugins->HealthCheck(id, error);
          return crow::response(ok ? 200 : 400, json{{"ok", ok}, {"error", error}}.dump());
        });

    CROW_ROUTE(app, "/api/plugins/<string>/test/discover")([this, text](const crow::request&, std::string id) {
      if (!plugins) return crow::response(503, json{{"error", "plugin manager unavailable"}}.dump());
      HSFDriverRef driver = plugins->Driver(id);
      if (!driver.vt || !driver.self || !driver.vt->discover) {
        return crow::response(400, json{{"ok", false}, {"error", "plugin has no discover capability"}}.dump());
      }
      size_t found = 0;
      HSFStatus status = driver.vt->discover(driver.self, nullptr, 0, &found);
      if (status < 0) return crow::response(400, json{{"ok", false}, {"error", hsf_status_name(status)}}.dump());
      std::vector<HSFPointInfo> points(found);
      if (found > 0) {
        status = driver.vt->discover(driver.self, points.data(), points.size(), &found);
        if (status < 0) return crow::response(400, json{{"ok", false}, {"error", hsf_status_name(status)}}.dump());
      }
      json result = json::array();
      for (size_t i = 0; i < found && i < points.size(); ++i) {
        result.push_back({{"name", text(points[i].point_name)},
                          {"location", text(points[i].location)},
                          {"unit", text(points[i].unit_text)},
                          {"encoding", points[i].encoding},
                          {"access", points[i].access}});
      }
      return crow::response(200, json{{"ok", true}, {"points", result}}.dump());
    });
  }

  void SetupPackageRoutes() {
    // Reused by every handler below: the request body, size-limited and
    // parsed, or a 400 the caller can return directly.
    auto readBody = [](const crow::request& req, json& body) -> bool {
      std::string error;
      return Validate::JsonObject(req.body, 16384, body, error);
    };

    CROW_ROUTE(app, "/api/lua/packages/import").methods("POST"_method)([this](const crow::request& req) {
      if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      try {
        crow::multipart::message msg(req);
        const std::string file = msg.get_part_by_name("filename").body;
        const std::string payload = msg.get_part_by_name("package").body;
        std::vector<unsigned char> bytes(payload.begin(), payload.end());
        std::string error;
        if (!packages->Import(file, bytes, error)) return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
        return crow::response(200, json{{"ok", true}, {"file", file}}.dump());
      } catch (const std::exception& e) {
        return crow::response(400, json{{"ok", false}, {"error", e.what()}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/lua/packages/import-path").methods("POST"_method)([this](const crow::request& req) {
      if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      json body;
      std::string parseError;
      if (!Validate::JsonObject(req.body, 4096, body, parseError)) {
        return crow::response(400, json{{"ok", false}, {"error", "invalid JSON"}}.dump());
      }
      const std::string sourcePath = body.value("path", std::string());
      if (sourcePath.empty()) {
        return crow::response(400, json{{"ok", false}, {"error", "path is required"}}.dump());
      }
      std::string importedFile;
      std::string error;
      if (!packages->ImportFromPath(sourcePath, importedFile, error)) {
        return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
      }
      return crow::response(200, json{{"ok", true}, {"file", importedFile}}.dump());
    });

    CROW_ROUTE(app, "/api/lua/packages")([this] {
      if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      json status = packages->StatusJson();
      std::vector<std::string> running;
      if (lua) {
        for (const LuaRuntimeStatus& runtime : lua->Statuses()) {
          if (runtime.state == "RUNNING") running.push_back(runtime.name);
        }
      }
      for (auto& item : status["packages"]) {
        const std::string file = item.value("file", std::string());
        item["running"] = std::find(running.begin(), running.end(), file) != running.end();
      }
      status["running_now"] = running;
      return crow::response(200, status.dump());
    });

    // Compile without producing an artifact: the editor's Compile button.
    // Everything the full build does except writing the .pkg, so a syntax
    // error or a runtime mismatch surfaces before anyone thinks about
    // deploying.
    CROW_ROUTE(app, "/api/lua/compile").methods("POST"_method)([this, readBody](const crow::request& req) {
      if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      json body;
      if (!readBody(req, body)) return crow::response(400, json{{"error", "INVALID_REQUEST"}}.dump());

      Validate::Errors errors;
      std::string appId, version, entry, directory;
      Validate::StringField(body, "app_id", 1, 64, "identifier", appId, errors);
      Validate::StringField(body, "version", 1, 64, "identifier", version, errors);
      Validate::StringField(body, "entry", 1, 256, entry, errors);
      Validate::StringField(body, "source_dir", 0, 256, directory, errors, false);
      if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

      std::string root;
      if (!ResolveApplicationDir(directory, root)) {
        return crow::response(400, json{{"error", "source_dir is outside the scripts directory"}}.dump());
      }

      auto& ctx = app.get_context<SecurityMiddleware>(req);
      PackageManager::BuildReport report =
          packages->BuildFromDirectory(root, ConfigManager::Instance().ScriptsDir(), appId, version, entry,
                                        ctx.session.username, "", false);
      return crow::response(report.ok ? 200 : 400, BuildReportJson(report).dump());
    });

    // Build & Test: compile, bundle, encrypt, sign, re-open, load every module
    // with the real interpreter, and only then write the artifact.
    CROW_ROUTE(app, "/api/lua/packages/build")
        .methods("POST"_method)([this, readBody](const crow::request& req) {
          if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
          json body;
          if (!readBody(req, body)) return crow::response(400, json{{"error", "INVALID_REQUEST"}}.dump());

          Validate::Errors errors;
          std::string appId, version, entry, directory, notes;
          Validate::StringField(body, "app_id", 1, 64, "identifier", appId, errors);
          Validate::StringField(body, "version", 1, 64, "identifier", version, errors);
          Validate::StringField(body, "entry", 1, 256, entry, errors);
          Validate::StringField(body, "source_dir", 0, 256, directory, errors, false);
          Validate::StringField(body, "notes", 0, 512, notes, errors, false);
          if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

          std::string root;
          if (!ResolveApplicationDir(directory, root)) {
            return crow::response(400, json{{"error", "source_dir is outside the scripts directory"}}.dump());
          }

          auto& ctx = app.get_context<SecurityMiddleware>(req);
          PackageManager::BuildReport report =
              packages->BuildFromDirectory(root, ConfigManager::Instance().ScriptsDir(), appId, version, entry,
                                            ctx.session.username, notes, true);
          if (report.ok) {
            SecurityStore::Instance().RecordAudit("LUA_BUILD", ctx.session.username, ctx.source_ip,
                                                   report.package_path, "OK",
                                                   std::to_string(report.modules_compiled) + " modules");
          }
          return crow::response(report.ok ? 200 : 400, BuildReportJson(report).dump());
        });

    CROW_ROUTE(app, "/api/lua/packages/deploy")
        .methods("POST"_method)([this, readBody](const crow::request& req) {
          if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
          json body;
          if (!readBody(req, body)) return crow::response(400, json{{"error", "INVALID_REQUEST"}}.dump());
          Validate::Errors errors;
          std::string file;
          Validate::StringField(body, "file", 1, 128, "identifier", file, errors);
          if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

          auto& ctx = app.get_context<SecurityMiddleware>(req);
          std::string error;
          if (!packages->Deploy(file, ctx.session.username, error)) {
            return crow::response(400, json{{"success", false}, {"error", error}}.dump());
          }
          SecurityStore::Instance().RecordAudit("LUA_DEPLOY", ctx.session.username, ctx.source_ip, file,
                                                 "OK", "");

          const bool restart = body.value("restart", true);
          json result = {{"success", true}, {"deployed", file}, {"restarted", false}};
          if (restart) {
            std::string startError;
            result["restarted"] = StartPackageFile(file, startError);
            if (!startError.empty()) result["start_error"] = startError;
          }
          return crow::response(200, result.dump());
        });

    CROW_ROUTE(app, "/api/lua/packages/start").methods("POST"_method)(
        [this, readBody](const crow::request& req) {
          if (!packages || !lua)
            return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
          json body;
          if (!readBody(req, body))
            return crow::response(400, json{{"error", "INVALID_REQUEST"}}.dump());
          const std::string file = body.value("file", packages->ActiveFile());
          if (file.empty())
            return crow::response(400, json{{"ok", false}, {"error", "file is required"}}.dump());
          auto& ctx = app.get_context<SecurityMiddleware>(req);
          std::string error;
          if (!packages->SetRunOnStartup(file, true, ctx.session.username, error))
            return crow::response(400, json{{"ok", false}, {"error", error}}.dump());
          const bool started = StartPackageFile(file, error);
          json result = {{"ok", started}, {"file", file}, {"run_on_startup", true}};
          if (!error.empty()) result["error"] = error;
          return crow::response(started ? 200 : 400, result.dump());
        });

    CROW_ROUTE(app, "/api/lua/packages/stop").methods("POST"_method)([this, readBody](const crow::request& req) {
      if (!packages || !lua) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      json body;
      if (!readBody(req, body)) return crow::response(400, json{{"error", "INVALID_REQUEST"}}.dump());
      const std::string file = body.value("file", packages->ActiveFile());
      if (file.empty()) return crow::response(400, json{{"ok", false}, {"error", "file is required"}}.dump());

      bool stopped = false;
      for (const LuaRuntimeStatus& status : lua->Statuses()) {
        if (status.name == file) stopped = lua->Stop(status.name) || stopped;
      }
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      std::string error;
      if (!packages->SetRunOnStartup(file, false, ctx.session.username, error))
        return crow::response(500, json{{"ok", false}, {"error", error}}.dump());
      return crow::response(200, json{{"ok", true}, {"file", file}, {"stopped", stopped},
                                       {"run_on_startup", false}}.dump());
    });

    CROW_ROUTE(app, "/api/lua/packages/rollback").methods("POST"_method)([this](const crow::request& req) {
      if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      std::string rolledBackTo;
      std::string error;
      if (!packages->Rollback(ctx.session.username, rolledBackTo, error)) {
        return crow::response(400, json{{"success", false}, {"error", error}}.dump());
      }
      SecurityStore::Instance().RecordAudit("LUA_ROLLBACK", ctx.session.username, ctx.source_ip,
                                             rolledBackTo, "OK", "");
      std::string startError;
      const bool restarted = StartPackageFile(rolledBackTo, startError);
      json result = {{"success", true}, {"active", rolledBackTo}, {"restarted", restarted}};
      if (!startError.empty()) result["start_error"] = startError;
      return crow::response(200, result.dump());
    });

    // Creates a signing keypair on this gateway, making it a machine that can
    // BUILD packages rather than only run them.
    //
    // Explicit rather than generated on the first build, because a keypair is
    // an identity: generating one silently would give every gateway in a fleet
    // a different one, and each would then reject the others' artifacts with
    // "signature does not verify" -- which reads exactly like an attack. It
    // also refuses to overwrite an existing key, since that would invalidate
    // every package already built with it.
    CROW_ROUTE(app, "/api/lua/packages/keys").methods("POST"_method)([this](const crow::request& req) {
      if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
      if (packages->Keys().HasSigningKey()) {
        return crow::response(409, json{{"success", false},
                                         {"error", "a signing key already exists"},
                                         {"fingerprint", packages->Keys().PublicKeyFingerprint()}}
                                        .dump());
      }
      std::string error;
      if (!packages->Initialise(/*wantSigningKey=*/true, error)) {
        return crow::response(500, json{{"success", false}, {"error", error}}.dump());
      }
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      const std::string fingerprint = packages->Keys().PublicKeyFingerprint();
      SecurityStore::Instance().RecordAudit("LUA_SIGNING_KEY_CREATED", ctx.session.username, ctx.source_ip,
                                             packages->Keys().PublicKeyPath(), "OK", fingerprint);
      Logger::Instance().Warning(LogCategory::Lua,
                                 "A Lua package signing key was generated by " + ctx.session.username);
      return crow::response(201, json{{"success", true}, {"fingerprint", fingerprint}}.dump());
    });

    CROW_ROUTE(app, "/api/lua/packages/delete")
        .methods("POST"_method)([this, readBody](const crow::request& req) {
          if (!packages) return crow::response(503, json{{"error", "packaging unavailable"}}.dump());
          json body;
          if (!readBody(req, body)) return crow::response(400, json{{"error", "INVALID_REQUEST"}}.dump());
          Validate::Errors errors;
          std::string file;
          Validate::StringField(body, "file", 1, 128, "identifier", file, errors);
          if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

          std::string error;
          if (!packages->Remove(file, error)) {
            return crow::response(400, json{{"success", false}, {"error", error}}.dump());
          }
          return crow::response(200, json{{"success", true}}.dump());
        });
  }

  // Application source directories are addressed relative to the scripts
  // directory, and the same containment rule applies as everywhere else a
  // request names a path: resolve it, then prove it did not leave.
  bool ResolveApplicationDir(const std::string& relative, std::string& resolved) const {
    const std::string base = ConfigManager::Instance().ScriptsDir();
    if (relative.empty() || relative == ".") {
      resolved = base;
      return true;
    }
    return Validate::PathStaysWithin(base, relative, resolved);
  }

  static json BuildReportJson(const PackageManager::BuildReport& report) {
    json result = {{"ok", report.ok},
                    {"stages_passed", report.stages_passed},
                    {"modules_compiled", report.modules_compiled},
                    {"source_bytes", report.source_bytes},
                    {"bytecode_bytes", report.bytecode_bytes},
                    {"package_bytes", report.package_bytes}};
    if (!report.ok) {
      result["failed_stage"] = report.failed_stage;
      result["error"] = report.error;
      if (report.error_line > 0) result["error_line"] = report.error_line;
      if (!report.error_module.empty()) result["error_module"] = report.error_module;
    } else {
      result["metadata"] = LuaPackage::ToJson(report.metadata);
      if (!report.package_path.empty()) result["file"] = report.package_path;
    }
    return result;
  }

  // Starts one package, replacing only another version of the same application.
  // Unrelated Lua applications keep running. False (with `error`) when the
  // selected package cannot be opened or started.
  bool StartPackageFile(const std::string& file, std::string& error) {
    if (!packages || !lua) {
      error = "packaging unavailable";
      return false;
    }
    if (file.empty()) {
      error = "package file is required";
      return false;
    }

    LuaBundle bundle;
    LuaPackage::Metadata metadata;
    if (!packages->OpenBundle(file, bundle, metadata, error)) return false;

    std::vector<std::pair<std::string, std::string>> modules;
    modules.reserve(bundle.Count());
    for (const LuaBundle::Module& module : bundle.Modules()) {
      modules.emplace_back(module.name,
                            std::string(reinterpret_cast<const char*>(module.bytecode.data()),
                                        module.bytecode.size()));
    }
    const std::vector<unsigned char>* entry = bundle.EntryBytecode();
    if (entry == nullptr) {
      error = "the package has no entry module";
      return false;
    }

    // Multiple applications may run together, but two versions of one app may
    // not. Replace only runtimes with the same app_id and leave unrelated
    // packages running.
    const std::vector<PackageManager::Entry> known = packages->List();
    for (const LuaRuntimeStatus& status : lua->Statuses()) {
      const bool sameApplication =
          std::any_of(known.begin(), known.end(), [&status, &metadata](const PackageManager::Entry& entry) {
            return entry.file == status.name && entry.metadata.app_id == metadata.app_id;
          });
      if (sameApplication) lua->Stop(status.name);
    }

    const std::string displayName = file;
    return lua->StartPackage(
               displayName, modules,
               std::string(reinterpret_cast<const char*>(entry->data()), entry->size()), error) ==
           LuaRuntimeManager::StartResult::kStarted;
  }

  // --- authentication, sessions, users and the audit trail ----------------
  //
  // request/AdvanceUpdate.md sections 1.1, 1.2 and 1.10. Note how little
  // guarding there is *in* these handlers: the middleware has already decided
  // whether the caller may be here, so what is left is the business logic.
  // That is the point of section 1.2's "reusable security layer".
  void SetupAuthRoutes() {
    // Lets the login page tell "auth is off, go straight in" from "auth is on,
    // show the form" without holding a token. Says nothing else.
    CROW_ROUTE(app, "/api/auth/mode")([] {
      const AuthConfig config = ConfigManager::Instance().GetAuth();
      return crow::response(200, json{{"enabled", config.enabled},
                                       {"token_lifetime_sec", config.token_lifetime_sec}}
                                     .dump());
    });

    CROW_ROUTE(app, "/api/auth/login").methods("POST"_method)([this](const crow::request& req) {
      const AuthConfig config = ConfigManager::Instance().GetAuth();
      const std::string ip = SecurityMiddleware::ClientIp(req);

      json body;
      std::string parseError;
      if (!Validate::JsonObject(req.body, static_cast<size_t>(std::max(1024, config.max_body_bytes)), body,
                                 parseError)) {
        return crow::response(400, json{{"success", false}, {"error", "INVALID_REQUEST"}}.dump());
      }

      Validate::Errors errors;
      std::string username;
      std::string password;
      // Length bounds only. A password may contain anything the user can type,
      // so a character allowlist here would reject good passwords for no gain
      // -- it never reaches a shell, a query or a path, only Argon2id.
      Validate::StringField(body, "username", 1, 64, "identifier", username, errors);
      Validate::StringField(body, "password", 1, 256, "", password, errors, true);
      if (!errors.Empty()) {
        // 401 and the same body as a wrong password -- NOT 400, and
        // deliberately not errors.ToJson(). Every way of failing to log in
        // gives one answer: a caller cannot tell a malformed username from a
        // valid one with the wrong password, which is the same principle that
        // makes "no such user" and "bad password" indistinguishable above.
        // A 400 here would quietly reintroduce an oracle, since only some
        // usernames are syntactically acceptable.
        return crow::response(401, json{{"success", false}, {"error", "INVALID_CREDENTIALS"}}.dump());
      }

      std::string token;
      Session session;
      int64_t lockoutRemaining = 0;
      const auto result = SecurityStore::Instance().Login(
          username, password, ip, config.max_failed_attempts, config.lockout_seconds,
          config.token_lifetime_sec, config.max_sessions_per_user, token, session, lockoutRemaining);

      auto audit = [&](const char* event, const std::string& outcome, const std::string& reason) {
        if (!config.audit_log) return;
        SecurityStore::Instance().RecordAudit(event, Validate::SanitiseForLog(username, 64),
                                               Validate::SanitiseForLog(ip, 64), "/api/auth/login", outcome,
                                               reason);
      };

      switch (result) {
        case SecurityStore::LoginResult::kOk:
          audit(audit::kLoginSuccess, "OK", "");
          Logger::Instance().Info(LogCategory::System,
                                   "Login: " + Validate::SanitiseForLog(username, 64) + " from " + ip);
          return crow::response(
              200, json{{"success", true},
                         {"token", token},
                         {"token_type", "Bearer"},
                         {"expires_at", session.expires_at},
                         {"expires_in", session.expires_at - session.issued_at},
                         {"username", session.username},
                         {"role", RoleName(session.role)},
                         {"permissions", PermissionNamesFor(session.role)}}
                        .dump());

        case SecurityStore::LoginResult::kLockedOut:
          audit(audit::kAccountLocked, "DENIED", "locked out");
          Logger::Instance().Warning(LogCategory::System, "Login refused (locked out): " +
                                                               Validate::SanitiseForLog(username, 64) +
                                                               " from " + ip);
          // 423 Locked, and the remaining time -- this one detail is worth
          // leaking, because it is the difference between a user waiting and a
          // user filing a fault report.
          return crow::response(423, json{{"success", false},
                                           {"error", "ACCOUNT_LOCKED"},
                                           {"retry_after_seconds", lockoutRemaining}}
                                          .dump());

        case SecurityStore::LoginResult::kInternalError:
          audit(audit::kLoginFailed, "ERROR", "internal");
          return crow::response(500, json{{"success", false}, {"error", "INTERNAL_ERROR"}}.dump());

        case SecurityStore::LoginResult::kDisabled:
        case SecurityStore::LoginResult::kInvalidCredentials:
        default:
          // One answer for "no such user", "wrong password" and "account
          // disabled" (section 1.1). The audit log records which it was; the
          // caller does not get to find out.
          audit(audit::kLoginFailed, "DENIED",
                result == SecurityStore::LoginResult::kDisabled ? "account disabled" : "bad credentials");
          Logger::Instance().Warning(LogCategory::System, "Failed login for " +
                                                               Validate::SanitiseForLog(username, 64) +
                                                               " from " + ip);
          return crow::response(401, json{{"success", false}, {"error", "INVALID_CREDENTIALS"}}.dump());
      }
    });

    CROW_ROUTE(app, "/api/auth/logout").methods("POST"_method)([this](const crow::request& req) {
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      SecurityStore::Instance().Revoke(SecurityMiddleware::BearerToken(req));
      const AuthConfig config = ConfigManager::Instance().GetAuth();
      if (config.audit_log) {
        SecurityStore::Instance().RecordAudit(audit::kLogout, ctx.session.username, ctx.source_ip,
                                               "/api/auth/logout", "OK", "");
      }
      return crow::response(200, json{{"success", true}}.dump());
    });

    CROW_ROUTE(app, "/api/auth/me")([this](const crow::request& req) {
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      if (!ctx.authenticated) {
        // Only reachable with auth.enabled off, where every route runs
        // unauthenticated. Say so plainly instead of inventing a user.
        return crow::response(200, json{{"authenticated", false}, {"auth_enabled", false}}.dump());
      }
      return crow::response(200, json{{"authenticated", true},
                                       {"auth_enabled", true},
                                       {"username", ctx.session.username},
                                       {"role", RoleName(ctx.session.role)},
                                       {"permissions", PermissionNamesFor(ctx.session.role)},
                                       {"expires_at", ctx.session.expires_at}}
                                     .dump());
    });

    // Changing your own password. Requires the current one even though the
    // caller already holds a valid token: a token left on an unlocked screen
    // should not be enough to lock the real owner out of their account.
    CROW_ROUTE(app, "/api/auth/password").methods("POST"_method)([this](const crow::request& req) {
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      const AuthConfig config = ConfigManager::Instance().GetAuth();
      json body;
      std::string parseError;
      if (!Validate::JsonObject(req.body, 4096, body, parseError)) {
        return crow::response(400, json{{"success", false}, {"error", "INVALID_REQUEST"}}.dump());
      }
      Validate::Errors errors;
      std::string current;
      std::string next;
      Validate::StringField(body, "current_password", 1, 256, "", current, errors);
      Validate::StringField(body, "new_password", 12, 256, "", next, errors);
      if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

      auto user = SecurityStore::Instance().FindUser(ctx.session.username);
      if (!user) return crow::response(401, json{{"success", false}, {"error", "UNAUTHENTICATED"}}.dump());

      std::string token;
      Session ignored;
      int64_t lockout = 0;
      // Re-authenticating through Login() reuses one code path for "is this
      // the right password", including its lockout accounting.
      if (SecurityStore::Instance().Login(user->username, current, ctx.source_ip, config.max_failed_attempts,
                                           config.lockout_seconds, 60, config.max_sessions_per_user, token,
                                           ignored, lockout) != SecurityStore::LoginResult::kOk) {
        return crow::response(401, json{{"success", false}, {"error", "INVALID_CREDENTIALS"}}.dump());
      }
      SecurityStore::Instance().Revoke(token);  // the throwaway session that check just issued

      std::string error;
      if (!SecurityStore::Instance().SetPassword(user->id, next, error)) {
        Logger::Instance().Error(LogCategory::System, "Password change failed: " + error);
        return crow::response(500, json{{"success", false}, {"error", "INTERNAL_ERROR"}}.dump());
      }
      if (config.audit_log) {
        SecurityStore::Instance().RecordAudit(audit::kPasswordChanged, user->username, ctx.source_ip,
                                               "/api/auth/password", "OK", "self-service");
      }
      // SetPassword revoked every session including this one, so the caller
      // must log in again -- which is the point.
      return crow::response(200, json{{"success", true}, {"reauthenticate", true}}.dump());
    });

    // --- user administration ------------------------------------------------
    CROW_ROUTE(app, "/api/users").methods("GET"_method, "POST"_method)([this](const crow::request& req) {
      auto& ctx = app.get_context<SecurityMiddleware>(req);
      const AuthConfig config = ConfigManager::Instance().GetAuth();

      if (req.method == crow::HTTPMethod::Get) {
        json users = json::array();
        for (const auto& user : SecurityStore::Instance().ListUsers()) {
          users.push_back(SecurityStore::ToJson(user));
        }
        return crow::response(200, json{{"users", users}}.dump());
      }

      json body;
      std::string parseError;
      if (!Validate::JsonObject(req.body, 8192, body, parseError)) {
        return crow::response(400, json{{"success", false}, {"error", "INVALID_REQUEST"}}.dump());
      }
      Validate::Errors errors;
      std::string username;
      std::string password;
      std::string roleName;
      std::string displayName;
      Validate::StringField(body, "username", 3, 32, "identifier", username, errors);
      Validate::StringField(body, "password", 12, 256, "", password, errors);
      Validate::EnumField(body, "role", {"ADMIN", "OPERATOR", "USER"}, roleName, errors);
      Validate::StringField(body, "display_name", 0, 64, displayName, errors, false);
      if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

      std::string error;
      auto created = SecurityStore::Instance().CreateUser(username, password, RoleFromName(roleName),
                                                           displayName, error);
      if (!created) return crow::response(400, json{{"success", false}, {"error", error}}.dump());

      if (config.audit_log) {
        SecurityStore::Instance().RecordAudit(audit::kUserCreated, ctx.session.username, ctx.source_ip,
                                               "/api/users", "OK", "created " + username + " as " + roleName);
      }
      return crow::response(201, SecurityStore::ToJson(*created).dump());
    });

    CROW_ROUTE(app, "/api/users/<int>")
        .methods("PATCH"_method, "DELETE"_method)([this](const crow::request& req, int id) {
          auto& ctx = app.get_context<SecurityMiddleware>(req);
          const AuthConfig config = ConfigManager::Instance().GetAuth();
          auto target = SecurityStore::Instance().FindUserById(id);
          if (!target) return crow::response(404, json{{"success", false}, {"error", "NOT_FOUND"}}.dump());

          if (req.method == crow::HTTPMethod::Delete) {
            // Two guards that exist because the alternative is a gateway
            // nobody can administer: you cannot delete yourself, and you
            // cannot remove the last enabled admin.
            if (target->username == ctx.session.username) {
              return crow::response(400, json{{"success", false}, {"error", "CANNOT_DELETE_SELF"}}.dump());
            }
            if (target->role == Role::kAdmin && SecurityStore::Instance().CountAdmins() <= 1) {
              return crow::response(400, json{{"success", false}, {"error", "LAST_ADMIN"}}.dump());
            }
            std::string error;
            if (!SecurityStore::Instance().DeleteUser(target->id, error)) {
              return crow::response(500, json{{"success", false}, {"error", "INTERNAL_ERROR"}}.dump());
            }
            SecurityStore::Instance().RevokeAllForUser(target->id);
            if (config.audit_log) {
              SecurityStore::Instance().RecordAudit(audit::kUserChanged, ctx.session.username, ctx.source_ip,
                                                     "/api/users", "OK", "deleted " + target->username);
            }
            return crow::response(200, json{{"success", true}}.dump());
          }

          json body;
          std::string parseError;
          if (!Validate::JsonObject(req.body, 8192, body, parseError)) {
            return crow::response(400, json{{"success", false}, {"error", "INVALID_REQUEST"}}.dump());
          }
          Validate::Errors errors;
          std::string changes;

          if (body.contains("role")) {
            std::string roleName;
            if (Validate::EnumField(body, "role", {"ADMIN", "OPERATOR", "USER"}, roleName, errors)) {
              if (target->role == Role::kAdmin && RoleFromName(roleName) != Role::kAdmin &&
                  SecurityStore::Instance().CountAdmins() <= 1) {
                return crow::response(400, json{{"success", false}, {"error", "LAST_ADMIN"}}.dump());
              }
              SecurityStore::Instance().SetRole(target->id, RoleFromName(roleName));
              changes += "role=" + roleName + " ";
            }
          }
          if (body.contains("enabled")) {
            bool enabled = true;
            if (Validate::BoolField(body, "enabled", enabled, errors)) {
              if (!enabled && target->role == Role::kAdmin && SecurityStore::Instance().CountAdmins() <= 1) {
                return crow::response(400, json{{"success", false}, {"error", "LAST_ADMIN"}}.dump());
              }
              SecurityStore::Instance().SetEnabled(target->id, enabled);
              changes += std::string("enabled=") + (enabled ? "true " : "false ");
            }
          }
          if (body.contains("password")) {
            // Not for your own account. This route is "an admin resets
            // someone else's password" and deliberately does not ask for the
            // current one; allowing it against yourself would be a way to
            // change your own password without proving you know it, which is
            // exactly what POST /api/auth/password exists to require. The UI
            // greys the button out, but a disabled button is not a control --
            // the check has to be here.
            if (target->username == ctx.session.username) {
              return crow::response(
                  400, json{{"success", false},
                             {"error", "USE_SELF_SERVICE_PASSWORD_CHANGE"},
                             {"detail", "Change your own password with POST /api/auth/password, which "
                                        "verifies the current one."}}
                           .dump());
            }
            std::string password;
            if (Validate::StringField(body, "password", 12, 256, "", password, errors)) {
              std::string error;
              if (!SecurityStore::Instance().SetPassword(target->id, password, error)) {
                return crow::response(500, json{{"success", false}, {"error", "INTERNAL_ERROR"}}.dump());
              }
              changes += "password ";
            }
          }
          if (!errors.Empty()) return crow::response(400, errors.ToJson().dump());

          if (config.audit_log && !changes.empty()) {
            SecurityStore::Instance().RecordAudit(audit::kUserChanged, ctx.session.username, ctx.source_ip,
                                                   "/api/users", "OK", target->username + ": " + changes);
          }
          auto updated = SecurityStore::Instance().FindUserById(id);
          return crow::response(200, SecurityStore::ToJson(*updated).dump());
        });

    // --- the audit trail ------------------------------------------------------
    CROW_ROUTE(app, "/api/audit")([](const crow::request& req) {
      const std::string event = req.url_params.get("event") ? req.url_params.get("event") : "";
      const std::string user = req.url_params.get("user") ? req.url_params.get("user") : "";
      int limit = req.url_params.get("limit") ? std::atoi(req.url_params.get("limit")) : 100;
      int offset = req.url_params.get("offset") ? std::atoi(req.url_params.get("offset")) : 0;
      // Clamped rather than trusted: an unbounded limit is a way to make the
      // gateway serialise the whole table into memory on request.
      limit = std::max(1, std::min(limit, 500));
      offset = std::max(0, offset);

      json rows = json::array();
      for (const auto& entry : SecurityStore::Instance().QueryAudit(event, user, limit, offset)) {
        rows.push_back(SecurityStore::ToJson(entry));
      }
      return crow::response(200, json{{"entries", rows},
                                       {"total", SecurityStore::Instance().CountAudit(event, user)},
                                       {"limit", limit},
                                       {"offset", offset}}
                                     .dump());
    });
  }

  // --- Test Tool routes (plan sections 43-48 and 57) ----------------------
  //
  // Every one of these calls the EXISTING driver (ZkController) and adds no
  // protocol of its own -- section 41's rule, and the reason the ZK sections
  // were mostly a wiring job: the driver already spoke all of this, nothing
  // had exposed it over HTTP.
  //
  // A NOTE ON PERMISSION. Plan section 59 asked for these to be admin-only,
  // and for a long time the honest answer was that they could not be: the
  // UI's login was web/js/auth.js, localStorage that said of itself "THIS IS
  // NOT SECURITY", so anyone who could reach the port could drive a relay with
  // curl. That is fixed. These routes now sit behind RELAY_CONTROL and
  // ZK_PROTOCOL_TEST in the policy table (src/security/Permissions.cpp),
  // enforced by the middleware before any handler here runs -- the relay and
  // door endpoints separately from the read-only ZK ones, because those open a
  // physical door.
  void SetupTestToolRoutes() {
    // Shared shape for everything below: run `work`, time it, log it, answer
    // {ok, error, duration_ms, ...}. Written once so no route can forget the
    // logging half.
    auto run = [this](const std::string& module, const std::string& operation, const json& params,
                       const std::function<bool(std::string&)>& work) {
      auto started = std::chrono::steady_clock::now();
      std::string error;
      bool ok = false;

      try {
        ok = work(error);
      } catch (const std::exception& ex) {
        error = ex.what();
      }

      int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();

      json row = LogTestOp(module, operation, params, ok, error, ms);

      return crow::response(200, json{{"ok", ok}, {"error", error}, {"duration_ms", ms},
                                       {"log", row}}
                                      .dump());
    };

    // Guard for every route that needs the controller. Answered as a failed
    // operation rather than a 500: "there is no ZK support in this build" is a
    // diagnostic result, and the page should show it as one.
    auto requireZk = [this](std::string& error) {
      if (!zk) {
        error = "the ZK controller is not available in this build";
        return false;
      }
      return true;
    };

    auto intParam = [](const json& body, const char* name, int fallback) {
      if (!body.contains(name)) return fallback;
      const json& value = body[name];
      if (value.is_number_integer()) return value.get<int>();
      if (value.is_string()) {
        try {
          return std::stoi(value.get<std::string>());
        } catch (const std::exception&) {
          return fallback;
        }
      }
      return fallback;
    };

    auto parseBody = [](const crow::request& req) {
      json body = json::parse(req.body, nullptr, false);
      return body.is_discarded() ? json::object() : body;
    };

    // --- status -----------------------------------------------------------

    CROW_ROUTE(app, "/api/test/zk/status")([this] {
      if (!zk) {
        return crow::response(200, json{{"available", false},
                                         {"detail", "no ZK controller in this build"}}
                                        .dump());
      }

      ZkIoState io = zk->IoState();

      json auxInputs = json::array();
      for (const auto& entry : io.aux_inputs) {
        auxInputs.push_back(json{{"input", entry.first},
                                  {"shorted", entry.second},
                                  {"state", entry.second ? "HIGH" : "LOW"}});
      }

      json doors = json::array();
      for (size_t i = 0; i < io.doors.size(); ++i) {
        doors.push_back(json{{"door", static_cast<int>(i) + 1}, {"sensor", io.doors[i]}});
      }

      return crow::response(200, json{{"available", true},
                                       {"connected", zk->IsConnected()},
                                       {"reconnecting", zk->IsReconnecting()},
                                       {"rtlog_running", zk->IsRTLogRunning()},
                                       {"last_error", zk->LastError()},
                                       {"counts", {{"lock", io.lock_count},
                                                   {"aux_out", io.aux_out_count},
                                                   {"aux_in", io.aux_in_count},
                                                   {"reader", io.reader_count}}},
                                       {"doors", doors},
                                       {"door_status_time", io.status_time},
                                       {"door_status_seen", io.status_seen},
                                       {"aux_inputs", auxInputs},
                                       {"aux_input_seen", io.aux_input_seen}}
                                      .dump());
    });

    // --- connection -------------------------------------------------------

    CROW_ROUTE(app, "/api/test/zk/connect")
        .methods("POST"_method)([this, run, requireZk, parseBody, intParam](const crow::request& req) {
          json body = parseBody(req);

          // Defaults come from the gateway's own configuration, so the common
          // case is pressing Connect with the form untouched.
          ZkConfig configured = ConfigManager::Instance().GetZk();
          std::string ip = body.value("ip", configured.ip);
          int port = intParam(body, "port", configured.port > 0 ? configured.port : 4370);
          int timeoutMs = intParam(body, "timeout_ms", 3000);
          std::string password = body.value("password", configured.password);

          return run("ZK", "Connect", json{{"ip", ip}, {"port", port}, {"timeout_ms", timeoutMs}},
                      [&](std::string& error) {
                        if (!requireZk(error)) return false;
                        if (zk->Connect(ip, port, timeoutMs, password)) return true;
                        error = zk->LastError();
                        return false;
                      });
        });

    CROW_ROUTE(app, "/api/test/zk/disconnect")
        .methods("POST"_method)([this, run, requireZk] {
          return run("ZK", "Disconnect", json::object(), [&](std::string& error) {
            if (!requireZk(error)) return false;
            zk->Disconnect();
            return true;
          });
        });

    // --- RTLog ------------------------------------------------------------

    CROW_ROUTE(app, "/api/test/zk/rtlog/start")
        .methods("POST"_method)([this, run, requireZk] {
          return run("ZK RTLog", "Start RTLog", json::object(), [&](std::string& error) {
            if (!requireZk(error)) return false;
            if (!zk->IsConnected()) {
              error = "not connected to the controller";
              return false;
            }
            zk->StartRTLog();
            return true;
          });
        });

    CROW_ROUTE(app, "/api/test/zk/rtlog/stop")
        .methods("POST"_method)([this, run, requireZk] {
          return run("ZK RTLog", "Stop RTLog", json::object(), [&](std::string& error) {
            if (!requireZk(error)) return false;
            zk->StopRTLog();
            return true;
          });
        });

    // The tail the page shows on load. Live records arrive as "zk.rtlog"
    // frames on the dashboard WebSocket; this is only what was missed.
    CROW_ROUTE(app, "/api/test/zk/rtlog")([this](const crow::request& req) {
      int limit = 200;
      auto query = ParseQueryString(req.raw_url);
      auto it = query.find("limit");
      if (it != query.end()) {
        try {
          limit = std::max(1, std::min(500, std::stoi(it->second)));
        } catch (const std::exception&) {
        }
      }

      json rows = json::array();
      {
        std::lock_guard<std::mutex> lock(zkEventsMutex);
        auto start = zkEvents.size() > static_cast<size_t>(limit) ? zkEvents.size() - limit : 0;
        for (size_t i = start; i < zkEvents.size(); ++i) rows.push_back(zkEvents[i]);
      }

      return crow::response(200, json{{"ok", true},
                                       {"running", zk && zk->IsRTLogRunning()},
                                       {"rows", rows}}
                                      .dump());
    });

    CROW_ROUTE(app, "/api/test/zk/rtlog/clear").methods("POST"_method)([this] {
      size_t removed = 0;
      {
        std::lock_guard<std::mutex> lock(zkEventsMutex);
        removed = zkEvents.size();
        zkEvents.clear();
      }
      return crow::response(200, json{{"ok", true}, {"removed", removed}}.dump());
    });

    // --- device control (section 44) --------------------------------------

    CROW_ROUTE(app, "/api/test/zk/device/control")
        .methods("POST"_method)([this, run, requireZk, parseBody, intParam](const crow::request& req) {
          json body = parseBody(req);

          int operation = intParam(body, "operation", 0);
          int p1 = intParam(body, "param1", 0);
          int p2 = intParam(body, "param2", 0);
          int p3 = intParam(body, "param3", 0);
          int p4 = intParam(body, "param4", 0);

          json params{{"operation", operation}, {"param1", p1}, {"param2", p2},
                       {"param3", p3}, {"param4", p4}};

          return run("ZK Device", "ControlDevice", params, [&](std::string& error) {
            if (!requireZk(error)) return false;
            if (zk->ControlDevice(operation, p1, p2, p3, p4)) return true;
            error = zk->LastError();
            return false;
          });
        });

    CROW_ROUTE(app, "/api/test/zk/device/params")([this](const crow::request& req) {
      if (!zk) return crow::response(200, json{{"ok", false}, {"error", "no ZK controller"}}.dump());

      auto query = ParseQueryString(req.raw_url);
      auto it = query.find("items");
      std::string items = it != query.end() && !it->second.empty()
                               ? it->second
                               : "LockCount,AuxOutCount,AuxInCount,ReaderCount";

      std::map<std::string, std::string> values = zk->GetParams(items);

      json out = json::object();
      for (const auto& entry : values) out[entry.first] = entry.second;

      return crow::response(200, json{{"ok", !values.empty()},
                                       {"items", items},
                                       {"values", out},
                                       {"error", values.empty() ? zk->LastError() : ""}}
                                      .dump());
    });

    CROW_ROUTE(app, "/api/test/zk/door")
        .methods("POST"_method)([this, run, requireZk, parseBody, intParam](const crow::request& req) {
          json body = parseBody(req);
          int door = intParam(body, "door", 1);
          int seconds = std::max(0, std::min(60, intParam(body, "seconds", 3)));

          return run("ZK Device", "Open Door", json{{"door", door}, {"seconds", seconds}},
                      [&](std::string& error) {
                        if (!requireZk(error)) return false;
                        if (zk->OpenDoor(door, seconds)) return true;
                        error = zk->LastError();
                        return false;
                      });
        });

    // --- relays (sections 45, 46 and 62) ----------------------------------
    //
    // One handler for both kinds, because they ARE one command with a
    // different address type: 1 is a lock/user relay, 2 an auxiliary output.
    // The plan lists them as two sections; the driver has always had one call,
    // and duplicating it here would be exactly the copy section 41 forbids.
    auto relayRoute = [this, run, requireZk, parseBody, intParam](const std::string& module,
                                                                   bool auxiliary,
                                                                   const std::string& action) {
      return [this, run, requireZk, parseBody, intParam, module, auxiliary,
              action](const crow::request& req) {
        json body = parseBody(req);
        int number = intParam(body, "relay", intParam(body, "number", 1));
        // Capped: PulseOutput sleeps for the duration on this thread, so an
        // unbounded value would hold a web worker (and a latched relay) for as
        // long as the caller felt like typing.
        int durationMs = std::max(1, std::min(10000, intParam(body, "duration_ms", 200)));

        json params{{"relay", number}, {"auxiliary", auxiliary}};
        if (action == "PULSE") params["duration_ms"] = durationMs;

        return run(module, action, params, [&](std::string& error) {
          if (!requireZk(error)) return false;
          if (number < 1) {
            error = "relay number must be 1 or more";
            return false;
          }

          bool ok = action == "PULSE" ? zk->PulseOutput(number, auxiliary, durationMs)
                                      : zk->SetOutput(number, auxiliary, action == "ON");
          if (!ok) error = zk->LastError();
          return ok;
        });
      };
    };

    CROW_ROUTE(app, "/api/test/relay/on").methods("POST"_method)(relayRoute("Relay", false, "ON"));
    CROW_ROUTE(app, "/api/test/relay/off").methods("POST"_method)(relayRoute("Relay", false, "OFF"));
    CROW_ROUTE(app, "/api/test/relay/pulse").methods("POST"_method)(relayRoute("Relay", false, "PULSE"));

    CROW_ROUTE(app, "/api/test/aux-relay/on")
        .methods("POST"_method)(relayRoute("AUX Relay", true, "ON"));
    CROW_ROUTE(app, "/api/test/aux-relay/off")
        .methods("POST"_method)(relayRoute("AUX Relay", true, "OFF"));
    CROW_ROUTE(app, "/api/test/aux-relay/pulse")
        .methods("POST"_method)(relayRoute("AUX Relay", true, "PULSE"));

    // How many of each the panel says it has, so the UI can offer real choices
    // instead of a guessed list.
    CROW_ROUTE(app, "/api/test/relay/list")([this] {
      if (!zk) return crow::response(200, json{{"ok", false}, {"error", "no ZK controller"}}.dump());

      ZkIoState io = zk->IoState();
      return crow::response(200, json{{"ok", true},
                                       {"relays", io.lock_count},
                                       {"aux_relays", io.aux_out_count},
                                       {"aux_inputs", io.aux_in_count},
                                       {"readers", io.reader_count},
                                       {"note", "0 means the panel has not been asked yet -- "
                                                "the counts are read once per successful connect"}}
                                      .dump());
    });

    // --- beeper (section 62) ----------------------------------------------
    //
    // There is no beeper command in the PullSDK -- nothing in plcommpro.dll
    // addresses the reader's sounder, and no device parameter configures it.
    // A beep is therefore whatever relay the sounder is wired to, pulsed; this
    // route exists to find out WHICH relay that is.
    CROW_ROUTE(app, "/api/test/beeper")
        .methods("POST"_method)([this, run, requireZk, parseBody, intParam](const crow::request& req) {
          json body = parseBody(req);

          int number = intParam(body, "relay", 1);
          bool auxiliary = body.value("auxiliary", false);
          int count = std::max(1, std::min(10, intParam(body, "count", 1)));
          int onMs = std::max(1, std::min(5000, intParam(body, "on_ms", 150)));
          int gapMs = std::max(0, std::min(5000, intParam(body, "gap_ms", 150)));

          json params{{"relay", number}, {"auxiliary", auxiliary}, {"count", count},
                       {"on_ms", onMs}, {"gap_ms", gapMs}};

          return run("Beeper", "Beep", params, [&](std::string& error) {
            if (!requireZk(error)) return false;
            if (number < 1) {
              error = "relay number must be 1 or more";
              return false;
            }
            if (zk->Beep(number, auxiliary, count, onMs, gapMs)) return true;
            error = zk->LastError();
            return false;
          });
        });

    // --- inputs (sections 47 and 48) --------------------------------------

    CROW_ROUTE(app, "/api/test/aux-input/status")([this] {
      if (!zk) return crow::response(200, json{{"ok", false}, {"error", "no ZK controller"}}.dump());

      ZkIoState io = zk->IoState();

      json rows = json::array();
      for (const auto& entry : io.aux_inputs) {
        int64_t at = 0;
        auto stamp = io.aux_input_at.find(entry.first);
        if (stamp != io.aux_input_at.end()) at = stamp->second;

        rows.push_back(json{{"input", entry.first},
                             {"shorted", entry.second},
                             {"state", entry.second ? "HIGH" : "LOW"},
                             {"changed_at", at}});
      }

      // Why an input can be missing rather than false: the SDK has no
      // read-input call at all. An auxiliary input announces itself as RTLog
      // event 220/221 and is silent otherwise, so an input nobody has triggered
      // since start-up has no state to report -- and reporting LOW would be an
      // invention.
      return crow::response(200, json{{"ok", true},
                                       {"configured", io.aux_in_count},
                                       {"seen", io.aux_input_seen},
                                       {"inputs", rows},
                                       {"note", "inputs are event-driven (RTLog 220/221); one that "
                                                "has not changed since start-up is not listed"}}
                                      .dump());
    });

    // Section 48 asks for a Button Input panel. The ZK driver has no button
    // concept: exit-button presses arrive as RTLog records like anything else,
    // and there is no state to poll and no debounce setting to expose. Rather
    // than draw a panel of invented RELEASED rows, this says so and points at
    // the stream that does carry them.
    CROW_ROUTE(app, "/api/test/button/status")([this] {
      return crow::response(200, json{{"ok", true},
                                       {"supported", false},
                                       {"inputs", json::array()},
                                       {"note", "the PullSDK exposes no button state and no debounce "
                                                "setting; button presses appear in the RTLog stream "
                                                "as events -- watch the ZK RTLog panel"}}
                                      .dump());
    });

    // --- RabbitMQ (sections 49-54) ----------------------------------------
    //
    // These run on THEIR OWN short-lived connection, not the gateway's. That is
    // the important difference from the ZK panels above: an ad-hoc consumer on
    // the live channel would eat messages the SmartLocker script is waiting
    // for. Publishing from here and watching the live client receive it is
    // exactly how section 55's scenario is meant to be checked.
    //
    // The connection form defaults to the gateway's configured broker, so the
    // common case needs no typing -- and so nobody points a topology-creating
    // button at a broker by accident.
    auto mqConfigFrom = [](const json& body) {
      MqConfig config = ConfigManager::Instance().GetMq();

      if (body.contains("host") && body["host"].is_string() && !body["host"].get<std::string>().empty()) {
        config.host = body["host"].get<std::string>();
      }
      if (body.contains("port") && body["port"].is_number_integer()) {
        config.port = body["port"].get<int>();
      }
      if (body.contains("user") && body["user"].is_string() && !body["user"].get<std::string>().empty()) {
        config.user = body["user"].get<std::string>();
      }
      if (body.contains("password") && body["password"].is_string()) {
        config.password = body["password"].get<std::string>();
      }
      if (body.contains("vhost") && body["vhost"].is_string() && !body["vhost"].get<std::string>().empty()) {
        config.vhost = body["vhost"].get<std::string>();
      }

      return config;
    };

    auto adHocFrom = [&intParam](const json& body) {
      MqClient::AdHoc options;
      options.exchange = body.value("exchange", "");
      options.exchange_type = body.value("exchange_type", std::string("topic"));
      options.queue = body.value("queue", "");
      options.routing_key = body.value("routing_key", "");
      options.body = body.value("body", "");
      options.durable = body.value("durable", true);
      options.auto_delete = body.value("auto_delete", false);
      options.exclusive = body.value("exclusive", false);
      options.ack = body.value("ack", true);
      options.max_messages = std::max(1, std::min(100, intParam(body, "max_messages", 10)));
      options.timeout_ms = std::max(500, std::min(15000, intParam(body, "timeout_ms", 3000)));
      return options;
    };

    // Section 49. The live client's own state is reported alongside the probe,
    // because "can I reach the broker" and "is the gateway consuming from it"
    // are different questions and an operator needs both.
    CROW_ROUTE(app, "/api/test/rabbitmq/status")([this] {
      MqConfig config = ConfigManager::Instance().GetMq();

      json live{{"configured", config.enabled}};
      if (mq) {
        MqStatus status = mq->Status();
        live["connected"] = mq->IsConnected();
        live["available"] = static_cast<int>(status.available);
        live["paused"] = mq->IsPaused();
      } else {
        live["connected"] = false;
        live["detail"] = "no MQ client in this build";
      }

      return crow::response(200, json{{"ok", true},
                                       {"broker", {{"host", config.host},
                                                   {"port", config.port},
                                                   {"vhost", config.vhost},
                                                   {"user", config.user}}},
                                       {"topology", {{"exchange", config.exchange},
                                                     {"exchange_type", config.exchange_type},
                                                     {"queue", config.queue},
                                                     {"routing_key", config.routing_key}}},
                                       {"live", live}}
                                      .dump());
    });

    CROW_ROUTE(app, "/api/test/rabbitmq/connect")
        .methods("POST"_method)([run, parseBody, mqConfigFrom](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);

          // No password in the logged parameters, here or anywhere else: the
          // operation log is readable from the web UI.
          json params{{"host", config.host}, {"port", config.port},
                       {"vhost", config.vhost}, {"user", config.user}};

          return run("RabbitMQ", "Connect", params, [&](std::string& error) {
            return MqClient::TestConnect(config, error);
          });
        });

    CROW_ROUTE(app, "/api/test/rabbitmq/exchange/create")
        .methods("POST"_method)([run, parseBody, mqConfigFrom, adHocFrom](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);
          MqClient::AdHoc options = adHocFrom(body);

          json params{{"exchange", options.exchange}, {"type", options.exchange_type},
                       {"durable", options.durable}, {"auto_delete", options.auto_delete}};

          return run("RabbitMQ", "Create Exchange", params, [&](std::string& error) {
            return MqClient::DeclareExchange(config, options, error);
          });
        });

    CROW_ROUTE(app, "/api/test/rabbitmq/queue/create")
        .methods("POST"_method)([this, parseBody, mqConfigFrom, adHocFrom](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);
          MqClient::AdHoc options = adHocFrom(body);

          auto started = std::chrono::steady_clock::now();
          uint32_t messages = 0;
          uint32_t consumers = 0;
          std::string error;
          bool ok = MqClient::DeclareQueue(config, options, messages, consumers, error);

          int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();

          json params{{"queue", options.queue}, {"durable", options.durable},
                       {"exclusive", options.exclusive}, {"auto_delete", options.auto_delete}};

          json row = LogTestOp("RabbitMQ", "Create Queue", params, ok, error, ms);

          return crow::response(200, json{{"ok", ok}, {"error", error}, {"duration_ms", ms},
                                           {"messages", messages}, {"consumers", consumers},
                                           {"log", row}}
                                          .dump());
        });

    CROW_ROUTE(app, "/api/test/rabbitmq/bind")
        .methods("POST"_method)([run, parseBody, mqConfigFrom, adHocFrom](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);
          MqClient::AdHoc options = adHocFrom(body);

          json params{{"exchange", options.exchange}, {"queue", options.queue},
                       {"routing_key", options.routing_key}};

          return run("RabbitMQ", "Bind", params, [&](std::string& error) {
            return MqClient::BindQueue(config, options, error);
          });
        });

    CROW_ROUTE(app, "/api/test/rabbitmq/publish")
        .methods("POST"_method)([run, parseBody, mqConfigFrom, adHocFrom](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);
          MqClient::AdHoc options = adHocFrom(body);

          json params{{"exchange", options.exchange}, {"routing_key", options.routing_key},
                       {"bytes", static_cast<int>(options.body.size())}};

          return run("RabbitMQ", "Publish", params, [&](std::string& error) {
            return MqClient::PublishOnce(config, options, error);
          });
        });

    CROW_ROUTE(app, "/api/test/rabbitmq/consume")
        .methods("POST"_method)([this, parseBody, mqConfigFrom, adHocFrom](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);
          MqClient::AdHoc options = adHocFrom(body);

          auto started = std::chrono::steady_clock::now();
          std::vector<MqClient::AdHocMessage> received;
          std::string error;
          bool ok = MqClient::ConsumeOnce(config, options, received, error);

          int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();

          json rows = json::array();
          for (const auto& message : received) {
            rows.push_back(json{{"body", message.body},
                                 {"exchange", message.exchange},
                                 {"routing_key", message.routing_key},
                                 {"message_id", message.message_id},
                                 {"delivery_tag", message.delivery_tag},
                                 {"redelivered", message.redelivered}});
          }

          json params{{"queue", options.queue}, {"ack", options.ack},
                       {"max_messages", options.max_messages}, {"received", rows.size()}};

          json row = LogTestOp("RabbitMQ", options.ack ? "Consume (ack)" : "Consume (peek)", params,
                                ok, error, ms);

          return crow::response(200, json{{"ok", ok}, {"error", error}, {"duration_ms", ms},
                                           {"messages", rows}, {"log", row}}
                                          .dump());
        });

    // --- SmartLocker scenario (sections 55 and 60, Phase 4) ---------------
    //
    // Publishing is only half of the test. The plan's checklist asks whether
    // the GATEWAY received it, whether SQLite changed, whether a locker was
    // assigned and whether the frontend can see it -- so this reads the
    // SmartLocker database back and answers each of those separately.
    //
    // Read straight from the locker database, on this port, rather than asking
    // the locker UI's own port: that port is optional (web.locker_ui_enabled)
    // and a diagnostic that only works when the wall display is switched on
    // would be a poor diagnostic.
    auto verifyCard = [this](const std::string& card) {
      json employees = LockerQuery(
          "SELECT id, username, card_code, role, gender, expire_at, active, locker_id "
          "  FROM employees WHERE card_code = ?",
          {SqlValue::Text(card)});

      json lockers = LockerQuery(
          "SELECT id, block_id, locker_number, locker_type, status, card_code, assigned_at "
          "  FROM lockers WHERE card_code = ?",
          {SqlValue::Text(card)});

      json events = LockerQuery(
          "SELECT event, result, reason, created_at FROM locker_logs "
          " WHERE card_code = ? ORDER BY id DESC LIMIT 10",
          {SqlValue::Text(card)});

      const bool known = !employees.empty();
      const bool assigned = !lockers.empty();

      return json{{"card_code", card},
                   {"steps", {{"received", known},
                              {"stored", known},
                              {"assigned", assigned}}},
                   {"employee", known ? employees[0] : json(nullptr)},
                   {"locker", assigned ? lockers[0] : json(nullptr)},
                   {"events", events}};
    };

    CROW_ROUTE(app, "/api/test/scenario/verify")([this, verifyCard](const crow::request& req) {
      auto query = ParseQueryString(req.raw_url);
      auto it = query.find("card");

      if (it == query.end() || it->second.empty()) {
        return crow::response(400, json{{"ok", false}, {"error", "card is required"}}.dump());
      }

      json result = verifyCard(it->second);
      result["ok"] = true;
      return crow::response(200, result.dump());
    });

    // Publish, then WATCH FOR IT TO LAND. The wait is what makes this a test of
    // the chain rather than of the publish: the broker committing the message
    // says nothing about whether this gateway's consumer decoded it, upserted
    // the card and assigned a locker.
    CROW_ROUTE(app, "/api/test/scenario/run")
        .methods("POST"_method)([this, parseBody, mqConfigFrom, adHocFrom, intParam,
                                  verifyCard](const crow::request& req) {
          json body = parseBody(req);
          MqConfig config = mqConfigFrom(body);
          MqClient::AdHoc options = adHocFrom(body);

          std::string card = body.value("card_code", "");
          if (card.empty()) {
            return crow::response(400, json{{"ok", false}, {"error", "card_code is required"}}.dump());
          }

          // Long enough for a broker round trip plus the script's next loop
          // pass; capped so a web worker is never held for minutes.
          const int waitMs = std::max(500, std::min(30000, intParam(body, "wait_ms", 8000)));

          auto started = std::chrono::steady_clock::now();

          json before = verifyCard(card);

          std::string error;
          const bool published = MqClient::PublishOnce(config, options, error);

          json steps = json::array();
          steps.push_back(json{{"step", "Publish"}, {"ok", published}, {"detail", error}});

          json after = before;
          bool received = false;
          bool assigned = false;

          if (published) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);

            while (std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(250));
              after = verifyCard(card);
              received = after["steps"]["received"].get<bool>();
              assigned = after["steps"]["assigned"].get<bool>();
              if (received && assigned) break;
            }
          }

          steps.push_back(json{{"step", "Gateway received"}, {"ok", received},
                                {"detail", received ? "the card is in the local database"
                                                    : "no card row appeared before the timeout"}});
          steps.push_back(json{{"step", "SQLite updated"}, {"ok", received},
                                {"detail", received ? "employees row written" : ""}});
          steps.push_back(json{{"step", "Locker assigned"}, {"ok", assigned},
                                {"detail", assigned
                                               ? "locker " + std::to_string(after["locker"].value("locker_number", 0))
                                               : "no locker holds this card"}});

          int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();

          const bool ok = published && received && assigned;

          json params{{"card_code", card},
                       {"exchange", options.exchange},
                       {"routing_key", options.routing_key},
                       {"existed_before", before["steps"]["received"]}};

          json row = LogTestOp("SmartLocker", "Scenario", params, ok,
                                ok ? "" : (published ? "published, but the chain did not complete"
                                                     : error),
                                ms);

          return crow::response(200, json{{"ok", ok},
                                           {"duration_ms", ms},
                                           {"steps", steps},
                                           {"before", before},
                                           {"after", after},
                                           {"log", row}}
                                          .dump());
        });

    // --- the operation log (section 56) -----------------------------------

    CROW_ROUTE(app, "/api/test/log")([this] {
      json rows = json::array();
      {
        std::lock_guard<std::mutex> lock(testLogMutex);
        for (const auto& row : testLog) rows.push_back(row);
      }
      return crow::response(200, json{{"ok", true}, {"rows", rows}}.dump());
    });

    CROW_ROUTE(app, "/api/test/log").methods("DELETE"_method)([this] {
      std::lock_guard<std::mutex> lock(testLogMutex);
      testLog.clear();
      return crow::response(200, json{{"ok", true}}.dump());
    });
  }

  // --- locker UI: routes --------------------------------------------------

  void SetupLockerRoutes() {
    const std::string root = "locker";

    CROW_ROUTE(lockerApp, "/")([this, root] {
      crow::response res;
      ServeStatic(res, root + "/index.html");
      return res;
    });

    // The page's two assets, named individually, and a subdirectory for
    // anything else (a logo, an icon).
    //
    // NOT a "/<string>" catch-all, however tempting: that pattern also matches
    // "/ws", and a rule that matches wins before the WebSocket rule is ever
    // considered — the upgrade request then lands on a plain HTTP handler and
    // Crow drops the connection with no response at all. It cost an afternoon
    // once; a static route per file is the cheap price of not repeating it.
    CROW_ROUTE(lockerApp, "/style.css")([this, root] {
      crow::response res;
      ServeStatic(res, root + "/style.css");
      return res;
    });

    CROW_ROUTE(lockerApp, "/app.js")([this, root] {
      crow::response res;
      ServeStatic(res, root + "/app.js");
      return res;
    });

    // Images and anything else the page grows later. The name is checked rather
    // than trusted: it arrives from a URL.
    CROW_ROUTE(lockerApp, "/assets/<string>")([this, root](const std::string& file) {
      crow::response res;
      if (file.find("..") != std::string::npos || file.find('/') != std::string::npos ||
          file.find('\\') != std::string::npos) {
        res.code = 400;
        res.body = "bad path";
        return res;
      }
      ServeStatic(res, root + "/assets/" + file);
      return res;
    });

    // The whole model in one response -- what the page fetches on load, before
    // the WebSocket takes over.
    CROW_ROUTE(lockerApp, "/api/locker/state")([this](const crow::request& req) {
      int limit = 40;
      auto query = ParseQueryString(req.raw_url);
      auto it = query.find("events");
      if (it != query.end()) {
        try {
          limit = std::max(0, std::min(500, std::stoi(it->second)));
        } catch (const std::exception&) {
          // Left at the default: a junk ?events= is not worth a 400.
        }
      }
      return crow::response(200, BuildLockerStateJson(limit).dump());
    });

    CROW_ROUTE(lockerApp, "/api/locker/lockers")([this] {
      return crow::response(200, BuildLockerStateJson(0)["lockers"].dump());
    });

    CROW_ROUTE(lockerApp, "/api/locker/lockers/<int>")([this](int id) {
      json rows = LockerQuery(
          "SELECT l.*, e.username, e.gender, e.role, e.expire_at AS card_expire_at "
          "  FROM lockers l LEFT JOIN employees e ON e.card_code = l.card_code WHERE l.id = ?",
          {SqlValue::Int(id)});

      if (rows.empty()) {
        return crow::response(404, json{{"ok", false}, {"error", "no such locker"}}.dump());
      }

      json logs = LockerQuery(
          "SELECT id, event, result, reason, card_code, created_at FROM locker_logs "
          " WHERE locker_id = ? ORDER BY id DESC LIMIT 50",
          {SqlValue::Int(id)});

      json body = rows[0];
      body["logs"] = logs;
      return crow::response(200, body.dump());
    });

    CROW_ROUTE(lockerApp, "/api/locker/employees")([this] {
      return crow::response(
          200, LockerQuery("SELECT id, username, card_code, role, gender, expire_at, active, locker_id "
                            "  FROM employees ORDER BY (active = 0), username")
                    .dump());
    });

    CROW_ROUTE(lockerApp, "/api/locker/logs")([this](const crow::request& req) {
      int limit = 100;
      auto query = ParseQueryString(req.raw_url);
      auto it = query.find("limit");
      if (it != query.end()) {
        try {
          limit = std::max(1, std::min(1000, std::stoi(it->second)));
        } catch (const std::exception&) {
        }
      }
      return crow::response(200, LockerQuery("SELECT id, locker_id, card_code, event, result, reason, "
                                              "       created_at FROM locker_logs ORDER BY id DESC LIMIT ?",
                                              {SqlValue::Int(limit)})
                                     .dump());
    });

    // The full audit trail, filtered and paged BY SQLITE (the locker UI's
    // history modal).
    //
    // /api/locker/logs above answers "the most recent N rows" and is what the
    // dashboard's live strip uses. This one exists because the modal has to
    // search a whole retention window -- 180 days of a busy cabinet is tens of
    // thousands of rows, and shipping them to a browser to be filtered there
    // would be slow, would silently truncate at whatever limit was chosen, and
    // would make the record count a lie.
    //
    //   from    YYYY-MM-DD, inclusive, from 00:00:00 of that day
    //   to      YYYY-MM-DD, inclusive, to 23:59:59 of that day
    //   event   comma-separated event codes (the UI's filter chips)
    //   locker  one locker id
    //   block   one cabinet
    //   q       free text over the event, its reason, the card and the person
    //   limit   1..1000, default 200
    //   offset  0-based
    //
    // `total` is the count matching the filter BEFORE limit/offset, so the page
    // can say how many rows there are rather than how many it was handed.
    CROW_ROUTE(lockerApp, "/api/locker/history")([this](const crow::request& req) {
      auto query = ParseQueryString(req.raw_url);

      auto param = [&query](const char* name) -> std::string {
        auto it = query.find(name);
        return it == query.end() ? std::string() : it->second;
      };

      auto number = [&param](const char* name, int fallback, int low, int high) {
        std::string raw = param(name);
        if (raw.empty()) return fallback;
        try {
          return std::max(low, std::min(high, std::stoi(raw)));
        } catch (const std::exception&) {
          return fallback;
        }
      };

      // A date is validated rather than tolerated. An unreadable `limit` can
      // fall back to its default harmlessly, but an unreadable `from` that was
      // quietly dropped would widen the filter and hand back rows the operator
      // did not ask for -- while the page still showed their date range.
      auto isDate = [](const std::string& value) {
        if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
        for (size_t i = 0; i < value.size(); ++i) {
          if (i == 4 || i == 7) continue;
          if (value[i] < '0' || value[i] > '9') return false;
        }
        return true;
      };

      std::string from = param("from");
      std::string to = param("to");

      if ((!from.empty() && !isDate(from)) || (!to.empty() && !isDate(to))) {
        return crow::response(
            400, json{{"ok", false}, {"error", "from/to must be YYYY-MM-DD"}}.dump());
      }

      std::string where = " WHERE 1=1";
      std::vector<SqlValue> params;

      if (!from.empty()) {
        where += " AND l.created_at >= ?";
        params.push_back(SqlValue::Text(from + " 00:00:00"));
      }

      if (!to.empty()) {
        where += " AND l.created_at <= ?";
        params.push_back(SqlValue::Text(to + " 23:59:59"));
      }

      std::string lockerId = param("locker");
      if (!lockerId.empty()) {
        try {
          where += " AND l.locker_id = ?";
          params.push_back(SqlValue::Int(std::stoll(lockerId)));
        } catch (const std::exception&) {
          return crow::response(
              400, json{{"ok", false}, {"error", "locker must be a number"}}.dump());
        }
      }

      // Whole cabinet. Resolved through the lockers table rather than through a
      // block_id on the log row, because a log row belongs to a door and the
      // door is what belongs to a cabinet -- and re-wiring a cabinet must not
      // rewrite history.
      std::string blockId = param("block");
      if (!blockId.empty()) {
        try {
          where += " AND k.block_id = ?";
          params.push_back(SqlValue::Int(std::stoll(blockId)));
        } catch (const std::exception&) {
          return crow::response(
              400, json{{"ok", false}, {"error", "block must be a number"}}.dump());
        }
      }

      // Event codes are an enum in everything but name, so anything that is not
      // one is rejected instead of being bound and matching nothing -- a typo
      // should say so, not return an empty history.
      std::string events = param("event");
      if (!events.empty()) {
        std::vector<std::string> codes;
        size_t position = 0;

        while (position <= events.size() && codes.size() <= 64) {
          size_t comma = events.find(',', position);
          std::string code = events.substr(
              position, comma == std::string::npos ? std::string::npos : comma - position);
          position = comma == std::string::npos ? events.size() + 1 : comma + 1;

          if (code.empty()) continue;

          for (char c : code) {
            if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
              code.clear();
              break;
            }
          }

          if (code.empty()) {
            return crow::response(
                400, json{{"ok", false}, {"error", "event codes are A-Z, 0-9 and _"}}.dump());
          }

          codes.push_back(code);
        }

        if (!codes.empty()) {
          where += " AND l.event IN (";
          for (size_t i = 0; i < codes.size(); ++i) {
            where += (i ? ", ?" : "?");
            params.push_back(SqlValue::Text(codes[i]));
          }
          where += ")";
        }
      }

      // The person's name is worth searching even though it lives in another
      // table: "who opened D07 last Tuesday" is the question this modal is for.
      std::string text = param("q");
      if (!text.empty()) {
        where +=
            " AND (l.event LIKE ? OR l.reason LIKE ? OR l.result LIKE ?"
            "      OR l.card_code LIKE ? OR e.username LIKE ?)";
        std::string like = "%" + text + "%";
        for (int i = 0; i < 5; ++i) params.push_back(SqlValue::Text(like));
      }

      const std::string joins =
          "  FROM locker_logs l "
          "  LEFT JOIN lockers k ON k.id = l.locker_id "
          "  LEFT JOIN employees e ON e.card_code = l.card_code";

      json totals = LockerQuery("SELECT COUNT(*) AS total" + joins + where, params);
      int64_t total = totals.empty() ? 0 : totals[0].value("total", 0);

      int limit = number("limit", 200, 1, 1000);
      int offset = number("offset", 0, 0, 1000000);

      std::vector<SqlValue> pageParams = params;
      pageParams.push_back(SqlValue::Int(limit));
      pageParams.push_back(SqlValue::Int(offset));

      json rows = LockerQuery(
          "SELECT l.id, l.locker_id, l.card_code, l.event, l.result, l.reason, l.created_at, "
          "       k.locker_number, k.block_id, k.block_name, e.username" +
              joins + where + " ORDER BY l.id DESC LIMIT ? OFFSET ?",
          pageParams);

      return crow::response(200, json{{"ok", lockerDb.IsOpen()},
                                       {"total", total},
                                       {"limit", limit},
                                       {"offset", offset},
                                       {"rows", rows}}
                                     .dump());
    });

    // --- actions -----------------------------------------------------------
    //
    // These are the only two routes that CHANGE anything, and neither of them
    // touches the database: they are forwarded to the SmartLocker script, which
    // owns the PLC, the state machine and the audit log. A web thread writing
    // "status = OPEN" itself would be inventing a state no door is actually in.
    CROW_ROUTE(lockerApp, "/api/locker/lockers/<int>/unlock")
        .methods("POST"_method)([this](const crow::request& req, int id) {
          auto request = std::make_shared<LuaHttpRequest>();
          request->method = "POST";
          request->path = "lockers/" + std::to_string(id) + "/unlock";
          request->body = req.body;
          request->content_type = req.get_header_value("Content-Type");
          request->query["source"] = "locker-ui";
          return ForwardToLua(request);
        });

    CROW_ROUTE(lockerApp, "/api/locker/lockers/<int>/release")
        .methods("POST"_method)([this](const crow::request& req, int id) {
          auto request = std::make_shared<LuaHttpRequest>();
          request->method = "POST";
          request->path = "lockers/" + std::to_string(id) + "/release";
          request->body = req.body;
          request->content_type = req.get_header_value("Content-Type");
          request->query["source"] = "locker-ui";
          return ForwardToLua(request);
        });

    CROW_ROUTE(lockerApp, "/api/locker/sync").methods("POST"_method)([this](const crow::request& req) {
      auto request = std::make_shared<LuaHttpRequest>();
      request->method = "POST";
      request->path = "sync";
      request->body = req.body;
      request->content_type = req.get_header_value("Content-Type");
      request->query["source"] = "locker-ui";
      return ForwardToLua(request);
    });

    CROW_WEBSOCKET_ROUTE(lockerApp, "/ws")
        .onopen([this](crow::websocket::connection& conn) {
          {
            std::lock_guard<std::mutex> lock(lockerWsMutex);
            lockerWsConnections.push_back(&conn);
          }
          // The full state immediately, so a page that just connected does not
          // sit blank until something changes.
          json payload = BuildLockerStateJson();
          payload["type"] = "state";
          conn.send_text(payload.dump());
        })
        .onclose([this](crow::websocket::connection& conn, const std::string&, uint16_t) {
          std::lock_guard<std::mutex> lock(lockerWsMutex);
          lockerWsConnections.erase(
              std::remove(lockerWsConnections.begin(), lockerWsConnections.end(), &conn),
              lockerWsConnections.end());
        })
        .onmessage([this](crow::websocket::connection& conn, const std::string& message, bool) {
          // The one thing a client may ask for: a fresh snapshot, for a page
          // that woke from sleep and does not trust what it has.
          if (message == "refresh" || message == "state") {
            json payload = BuildLockerStateJson();
            payload["type"] = "state";
            conn.send_text(payload.dump());
          }
        });
  }

  // Pushes the locker state to every connected page. Sent only when it has
  // actually changed, plus a heartbeat every 10 seconds so a client can tell a
  // quiet system from a dead connection -- a wall display refreshing 18 cards a
  // second for no reason is how a browser tab ends up at 100% CPU all shift.
  void LockerBroadcastLoop() {
    std::string previous;
    int quietTicks = 0;

    while (running.load()) {
      json payload = BuildLockerStateJson();
      payload["type"] = "state";

      // generated_at changes every tick by definition, so the comparison is
      // made against the payload WITHOUT it.
      json comparable = payload;
      comparable.erase("generated_at");
      std::string fingerprint = comparable.dump();

      bool changed = fingerprint != previous;
      ++quietTicks;

      if (changed || quietTicks >= 10) {
        previous = fingerprint;
        quietTicks = 0;

        std::string message = payload.dump();
        std::lock_guard<std::mutex> lock(lockerWsMutex);
        for (auto* conn : lockerWsConnections) conn->send_text(message);
      }

      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  void BroadcastLoop() {
    // Start from "now" rather than 0, so a freshly started gateway doesn't
    // blast the entire backlog into the first tick. The viewer fetches
    // history over REST when it loads and then follows this stream.
    uint64_t lastLogSeq = Logger::Instance().LatestSeq();

    while (running.load()) {
      // Only entries created since the previous tick, capped so a burst of
      // logging can't produce an enormous frame.
      json logs = json::array();
      for (const auto& e : Logger::Instance().Since(lastLogSeq, 200)) {
        logs.push_back({{"seq", e.seq},
                         {"timestamp", e.timestamp},
                         {"level", Logger::LevelToString(e.level)},
                         {"category", Logger::CategoryToString(e.category)},
                         {"message", e.message}});
        lastLogSeq = e.seq > lastLogSeq ? e.seq : lastLogSeq;
      }

      json luaRuntime = BuildLuaRuntimeJson();

      json payload = {{"type", "status"},
                       {"status", BuildStatusJson()},
                       {"variables", RuntimeVariables::Instance().ToJson()},
                       {"modbus_io", ModbusRegistry::Instance().ToJson()},
                       {"lua_runtime", luaRuntime},
                       {"logs", logs}};
      std::string message = payload.dump();

      // Per-runtime frames in the exact shape request/upgrade.md section 25
      // defines, alongside the aggregate carried in the status frame above.
      // Two representations of the same data, deliberately: the dashboard
      // renders the whole table from one frame, while section 25's message is
      // what a client watching a single script's state expects to receive.
      std::vector<std::string> runtimeMessages;
      for (const auto& script : luaRuntime["scripts"]) {
        json frame = {{"type", "lua_runtime"},
                       {"runtime_id", script.value("runtime_id", 0)},
                       {"script", script.value("name", "")},
                       {"state", script.value("state", "")},
                       {"cpu_percent", script.value("cpu_percent", 0.0)},
                       {"memory_mb", script.value("memory_kb", 0.0) / 1024.0},
                       {"cpu_core", script.value("cpu_core", -1)},
                       {"timestamp", NowUnixSeconds()}};
        std::string error = script.value("last_error", "");
        if (!error.empty()) {
          frame["error"] = error;
          frame["error_line"] = script.value("error_line", 0);
        }
        runtimeMessages.push_back(frame.dump());
      }

      // The latest OTA frame, if the update thread left one since the last
      // tick. Sent as its own message rather than folded into the status
      // payload above: a page that only cares about updates (every page, via
      // update.js) then has one frame type to match on, and the frame is
      // absent entirely on a gateway with updates switched off.
      std::string updateMessage;
      {
        std::lock_guard<std::mutex> lock(updateFrameMutex);
        if (haveUpdateFrame) {
          updateMessage = pendingUpdateFrame.dump();
          haveUpdateFrame = false;
        }
      }

      {
        std::lock_guard<std::mutex> lock(wsMutex);
        for (auto* conn : wsConnections) {
          conn->send_text(message);
          for (const std::string& frame : runtimeMessages) conn->send_text(frame);
          if (!updateMessage.empty()) conn->send_text(updateMessage);
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
};

WebServer::WebServer() : impl_(std::make_unique<Impl>()) {}

WebServer::~WebServer() { Stop(); }

void WebServer::Configure(const WebConfig& config, const std::string& webRoot) {
  impl_->config = config;
  impl_->webRoot = webRoot;
}

void WebServer::SetServices(ServiceRegistry* services) {
  impl_->services = services;
  impl_->rest = services ? services->Get<RestClient>(ServiceNames::kRest) : nullptr;
  impl_->serial = services ? services->Get<SerialPort>(ServiceNames::kSerial) : nullptr;
  impl_->serial2 = services ? services->Get<SerialPort>(ServiceNames::kSerial2) : nullptr;
  impl_->modbus = services ? services->Get<ModbusClient>(ServiceNames::kModbus) : nullptr;
  impl_->rfid = services ? services->Get<RfidClient>(ServiceNames::kRfid) : nullptr;
  impl_->lua = services ? services->Get<LuaRuntimeManager>(ServiceNames::kLua) : nullptr;
  impl_->mq = services ? services->Get<MqClient>(ServiceNames::kMq) : nullptr;
  impl_->zk = services ? services->Get<ZkController>(ServiceNames::kZk) : nullptr;
  impl_->update = services ? services->Get<UpdateManager>(ServiceNames::kUpdate) : nullptr;
  impl_->packages = services ? services->Get<PackageManager>(ServiceNames::kPackages) : nullptr;
  impl_->plugins = services ? services->Get<PluginManager>(ServiceNames::kPlugins) : nullptr;
  if (impl_->update) {
    Impl* impl = impl_.get();
    impl_->update->SetStateCallback([impl](const nlohmann::json& frame) {
      std::lock_guard<std::mutex> lock(impl->updateFrameMutex);
      impl->pendingUpdateFrame = frame;
      impl->haveUpdateFrame = true;
    });
  }
  impl_->AttachZkListeners();
}

void WebServer::Start() {
  if (impl_->running.load()) return;
  impl_->running.store(true);

  impl_->SetupRoutes();

  // Crow stamps its own log lines in UTC, so the console and the log file
  // carried two clocks seven hours apart -- "(2026-08-15 03:10:38) Crow/master
  // server is running" directly under a gateway line reading 10:10. Routing
  // Crow's output through our Logger gives every line one timestamp format in
  // local time, and puts Crow's messages in the Log Viewer with everything
  // else. Static so the handler outlives the app it is installed on.
  static CrowLogBridge crowLogBridge;
  crow::logger::setHandler(&crowLogBridge);

  // signal_clear() BEFORE run(), and this is not optional.
  //
  // Crow defaults to `signals_{SIGINT, SIGTERM}` and installs its own handlers
  // inside run(), replacing the ones main() installed. The result on Linux was
  // that SIGTERM stopped the HTTP server and nothing else: g_running was never
  // cleared, the main loop kept polling, and the process ran forever. systemd
  // would have waited out its 90s TimeoutStopSec on every single restart and
  // then SIGKILLed -- so an OTA update or a deployment would "work" while
  // never shutting anything down cleanly, with scripts killed mid-transaction
  // and databases closed by the kernel rather than by us.
  //
  // It never showed on Windows, where every test killed the process outright
  // rather than signalling it. Found by the first real Linux run.
  impl_->app.signal_clear();

  impl_->serverThread = std::thread([this] {
    impl_->app.bindaddr(impl_->config.bind_address).port(impl_->config.port).multithreaded().run();
  });
  impl_->broadcastThread = std::thread([this] { impl_->BroadcastLoop(); });

  Logger::Instance().Info(LogCategory::System, "Web server listening on " + impl_->config.bind_address + ":" +
                                                    std::to_string(impl_->config.port));

  // The locker floor plan, on its own port. Separate thread, separate app, same
  // process -- and only when it is switched on, so a gateway that is not a
  // locker cabinet never opens the second listener.
  if (impl_->config.locker_ui_enabled) {
    impl_->SetupLockerRoutes();

    // Same reason as the configuration app above -- this is a second Crow app
    // and it would install the same handlers, so clearing one and not the
    // other would leave the bug in place whenever the locker UI is enabled.
    impl_->lockerApp.signal_clear();

    impl_->lockerServerThread = std::thread([this] {
      impl_->lockerApp.bindaddr(impl_->config.bind_address)
          .port(impl_->config.locker_ui_port)
          .multithreaded()
          .run();
    });
    impl_->lockerBroadcastThread = std::thread([this] { impl_->LockerBroadcastLoop(); });

    Logger::Instance().Info(LogCategory::System,
                             "SmartLocker UI listening on " + impl_->config.bind_address + ":" +
                                 std::to_string(impl_->config.locker_ui_port) + " (reading " +
                                 impl_->config.locker_db_path + ")");
  }
}

void WebServer::Stop() {
  // Detached FIRST, and before the running check: the callback holds `impl_`,
  // the controller outlives this object (both are main()'s locals, destroyed in
  // reverse order), and an RTLog record arriving after this point would call
  // into freed memory. A Stop() on a server that was never started still has to
  // undo what SetModules() wired up.
  impl_->DetachZkListeners();

  // Same reasoning, same ordering: UpdateManager's thread holds a raw pointer
  // to impl_ through the state callback, and it outlives this object only if
  // main() declared it first. Clearing the slot here means it does not matter.
  if (impl_->update) {
    impl_->update->SetStateCallback(nullptr);
    impl_->update = nullptr;
  }

  if (!impl_->running.load()) return;
  impl_->running.store(false);

  impl_->app.stop();
  impl_->lockerApp.stop();

  if (impl_->serverThread.joinable()) impl_->serverThread.join();
  if (impl_->broadcastThread.joinable()) impl_->broadcastThread.join();
  if (impl_->lockerServerThread.joinable()) impl_->lockerServerThread.join();
  if (impl_->lockerBroadcastThread.joinable()) impl_->lockerBroadcastThread.join();

  impl_->lockerDb.Close();
}

}  // namespace hsf
