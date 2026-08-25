#include "hsf/security/Permissions.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace hsf {
namespace {

struct NamedPermission {
  Permission permission;
  const char* name;
};

// The wire names. These appear in the audit log, in /api/auth/me, and in the
// 403 body, so they are part of the API and should not be renamed casually.
constexpr NamedPermission kPermissionNames[] = {
    {Permission::kSystemConfigRead, "SYSTEM_CONFIG_READ"},
    {Permission::kSystemConfigWrite, "SYSTEM_CONFIG_WRITE"},
    {Permission::kConfigExport, "CONFIG_EXPORT"},
    {Permission::kConfigImport, "CONFIG_IMPORT"},
    {Permission::kDeviceRead, "DEVICE_READ"},
    {Permission::kDeviceControl, "DEVICE_CONTROL"},
    {Permission::kRelayControl, "RELAY_CONTROL"},
    {Permission::kZkProtocolTest, "ZK_PROTOCOL_TEST"},
    {Permission::kRabbitmqTest, "RABBITMQ_TEST"},
    {Permission::kLuaRead, "LUA_READ"},
    {Permission::kLuaWrite, "LUA_WRITE"},
    {Permission::kLuaRun, "LUA_RUN"},
    {Permission::kLuaCompile, "LUA_COMPILE"},
    {Permission::kLuaDeploy, "LUA_DEPLOY"},
    {Permission::kLuaRollback, "LUA_ROLLBACK"},
    {Permission::kLogRead, "LOG_READ"},
    {Permission::kLogManage, "LOG_MANAGE"},
    {Permission::kCardClientRead, "CARD_CLIENT_READ"},
    {Permission::kCardClientWrite, "CARD_CLIENT_WRITE"},
    {Permission::kUpdateRead, "UPDATE_READ"},
    {Permission::kUpdateInstall, "UPDATE_INSTALL"},
    {Permission::kUserRead, "USER_READ"},
    {Permission::kUserWrite, "USER_WRITE"},
};
static_assert(sizeof(kPermissionNames) / sizeof(kPermissionNames[0]) ==
                  static_cast<size_t>(Permission::kCount),
              "every Permission needs a wire name -- a missing one would make the audit log lie");

// --- role -> permissions --------------------------------------------------
//
// OPERATOR is the interesting one. It is the account a shift supervisor uses:
// it can open doors, drive relays and exercise the hardware tests, because
// that is what recovering a stuck machine takes. It cannot change
// configuration, touch Lua, manage users or install an update -- everything
// that changes what the machine IS, as opposed to what it is doing right now.
constexpr Permission kOperatorPermissions[] = {
    Permission::kSystemConfigRead, Permission::kDeviceRead,      Permission::kDeviceControl,
    Permission::kRelayControl,     Permission::kZkProtocolTest,  Permission::kLuaRead,
    Permission::kLogRead,          Permission::kCardClientRead,  Permission::kUpdateRead,
};

// USER is a viewer: the dashboard, the logs, and nothing that writes.
constexpr Permission kUserPermissions[] = {
    Permission::kDeviceRead,
    Permission::kLuaRead,
    Permission::kLogRead,
    Permission::kUpdateRead,
    Permission::kConfigExport, Permission::kConfigImport,
};

}  // namespace

std::string PermissionName(Permission permission) {
  for (const auto& entry : kPermissionNames) {
    if (entry.permission == permission) return entry.name;
  }
  return "UNKNOWN";
}

Permission PermissionFromName(const std::string& name) {
  for (const auto& entry : kPermissionNames) {
    if (name == entry.name) return entry.permission;
  }
  return Permission::kCount;
}

std::string RoleName(Role role) {
  switch (role) {
    case Role::kAdmin: return "ADMIN";
    case Role::kOperator: return "OPERATOR";
    case Role::kUser: return "USER";
    case Role::kCount: break;
  }
  return "UNKNOWN";
}

Role RoleFromName(const std::string& name) {
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(::toupper(c)); });
  if (upper == "ADMIN") return Role::kAdmin;
  if (upper == "OPERATOR") return Role::kOperator;
  if (upper == "USER") return Role::kUser;
  // The pre-existing mock frontend called the non-admin role "client"; accept
  // it so an old localStorage session name does not resolve to nothing.
  if (upper == "CLIENT") return Role::kUser;
  return Role::kCount;
}

bool RoleHasPermission(Role role, Permission permission) {
  switch (role) {
    case Role::kAdmin:
      return true;
    case Role::kOperator:
      for (Permission p : kOperatorPermissions) {
        if (p == permission) return true;
      }
      return false;
    case Role::kUser:
      for (Permission p : kUserPermissions) {
        if (p == permission) return true;
      }
      return false;
    case Role::kCount:
      break;
  }
  return false;
}

std::vector<std::string> PermissionNamesFor(Role role) {
  std::vector<std::string> names;
  for (const auto& entry : kPermissionNames) {
    if (RoleHasPermission(role, entry.permission)) names.push_back(entry.name);
  }
  return names;
}

// --- the route policy table -----------------------------------------------
//
// Read this top to bottom: it is the whole authorization policy for the
// configuration API. First match wins, so specific paths precede prefixes.
const std::vector<RoutePolicy>& RoutePolicies() {
  static const std::vector<RoutePolicy> kPolicies = {
      // -- public ---------------------------------------------------------
      // The login page and its assets have to load before anyone has a token,
      // and the whole SPA is served from these three routes.
      {"/", "", Guard::kPublic, Permission::kCount},
      {"/css/*", "", Guard::kPublic, Permission::kCount},
      {"/js/*", "", Guard::kPublic, Permission::kCount},
      // Every page shell, login.html most of all. These are static markup with
      // no data in them -- each one fetches what it displays over /api/, and
      // that is where the actual guard is. Leaving them to the
      // authenticate-by-default rule made the gateway unusable: /login.html
      // itself answered 401, so there was no way to obtain the token needed to
      // load /login.html.
      {"*.html", "", Guard::kPublic, Permission::kCount},
      {"/favicon.ico", "", Guard::kPublic, Permission::kCount},
      {"/api/auth/login", "POST", Guard::kPublic, Permission::kCount},
      // Answering "is auth even on?" must not require being authenticated --
      // the login page reads it to decide whether to show itself at all.
      {"/api/auth/mode", "GET", Guard::kPublic, Permission::kCount},
      // Deployment health probe. Public because a deploy script and a systemd
      // check both run before any credential exists on the machine; safe to be
      // public because it answers liveness, version and uptime and nothing
      // else. /api/status keeps the detail and keeps DEVICE_READ.
      {"/api/health", "GET", Guard::kPublic, Permission::kCount},

      // -- its own scheme ---------------------------------------------------
      // External card readers authenticate with a per-client API-Key
      // (CardClientManager). Layering a bearer token on top would break every
      // deployed reader and buy nothing -- it is already authenticated.
      {"/api/card/input", "POST", Guard::kApiKey, Permission::kCount},

      // -- session management ----------------------------------------------
      {"/api/auth/logout", "", Guard::kAuthenticated, Permission::kCount},
      {"/api/auth/me", "", Guard::kAuthenticated, Permission::kCount},
      {"/api/auth/password", "POST", Guard::kAuthenticated, Permission::kCount},

      // -- users -------------------------------------------------------------
      {"/api/users", "GET", Guard::kPermission, Permission::kUserRead},
      {"/api/users*", "", Guard::kPermission, Permission::kUserWrite},

      // The security audit trail. USER_READ, which only ADMIN holds -- not
      // LOG_READ, which an operator and a viewer both have. This log records
      // who failed to log in, from where, and how often; it is a map of the
      // attack surface, not an operational log.
      {"/api/audit", "", Guard::kPermission, Permission::kUserRead},

      // -- configuration -----------------------------------------------------
      // The split that motivates per-method policy: a viewer may read the
      // configuration page, only an admin may PATCH it.
      {"/api/config", "GET", Guard::kPermission, Permission::kSystemConfigRead},
      {"/api/config", "POST", Guard::kPermission, Permission::kSystemConfigWrite},
      {"/api/config/export", "GET", Guard::kPermission, Permission::kConfigExport},
      {"/api/config/import", "POST", Guard::kPermission, Permission::kConfigImport},
      {"/api/status", "", Guard::kPermission, Permission::kDeviceRead},
      {"/api/variables", "", Guard::kPermission, Permission::kDeviceRead},
      {"/api/serial/ports", "", Guard::kPermission, Permission::kSystemConfigRead},

      // -- logs ---------------------------------------------------------------
      {"/api/logs/clear", "POST", Guard::kPermission, Permission::kLogManage},
      {"/api/logs*", "", Guard::kPermission, Permission::kLogRead},

      // -- Lua -----------------------------------------------------------------
      // Reading a script is LUA_READ; everything that changes one, or starts
      // and stops one, is a write or a run.
      {"/api/lua/scripts/file", "GET", Guard::kPermission, Permission::kLuaRead},
      {"/api/lua/scripts/file", "", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/scripts/import", "POST", Guard::kPermission, Permission::kConfigImport},
      {"/api/lua/script", "GET", Guard::kPermission, Permission::kLuaRead},
      {"/api/lua/script", "POST", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/scripts-dir", "GET", Guard::kPermission, Permission::kLuaRead},
      {"/api/lua/scripts-dir", "POST", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/scripts/rename", "", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/scripts/folder/delete", "POST", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/scripts/set-default", "", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/clear-default", "", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/validate", "", Guard::kPermission, Permission::kLuaWrite},
      {"/api/lua/scripts/run", "", Guard::kPermission, Permission::kLuaRun},
      {"/api/lua/scripts/stop", "", Guard::kPermission, Permission::kLuaRun},
      {"/api/lua/run", "", Guard::kPermission, Permission::kLuaRun},
      {"/api/lua/stop", "", Guard::kPermission, Permission::kLuaRun},
      {"/api/lua/restart", "", Guard::kPermission, Permission::kLuaRun},
      {"/api/lua/runtimes/prune", "", Guard::kPermission, Permission::kLuaRun},
      {"/api/lua/scripts", "", Guard::kPermission, Permission::kLuaRead},
      {"/api/lua/runtime*", "", Guard::kPermission, Permission::kLuaRead},

      // -- production Lua packages (Phase 2) ----------------------------------
      // Three separate permissions, because these are three different jobs.
      // Building is a developer activity; deploying changes what the cabinet
      // in the lobby is running; rolling back is the thing you want someone to
      // be able to do at 3am without also being able to build a new artifact.
      {"/api/lua/packages", "GET", Guard::kPermission, Permission::kLuaRead},
      {"/api/lua/packages/import", "POST", Guard::kPermission, Permission::kConfigImport},
      {"/api/lua/compile", "", Guard::kPermission, Permission::kLuaCompile},
      {"/api/lua/packages/build", "", Guard::kPermission, Permission::kLuaCompile},
      {"/api/lua/packages/rollback", "", Guard::kPermission, Permission::kLuaRollback},
      // Creating a signing identity is not a build step; it decides which
      // artifacts this installation will ever trust. USER_WRITE, i.e. admin.
      {"/api/lua/packages/keys", "", Guard::kPermission, Permission::kUserWrite},
      {"/api/lua/packages/deploy", "", Guard::kPermission, Permission::kLuaDeploy},
      {"/api/lua/packages/stop", "POST", Guard::kPermission, Permission::kLuaDeploy},
      {"/api/lua/packages/start", "POST", Guard::kPermission, Permission::kLuaDeploy},
      {"/api/lua/packages/delete", "", Guard::kPermission, Permission::kLuaDeploy},
      {"/api/plugins/import", "POST", Guard::kPermission, Permission::kConfigImport},
      {"/api/plugins*", "GET", Guard::kPermission, Permission::kDeviceRead},
      {"/api/plugins*", "POST", Guard::kPermission, Permission::kDeviceControl},

      // -- script-served routes -------------------------------------------------
      // /api/app/<path> is whatever a running script registered with
      // Http.Register. The gateway cannot know what any of them do, so the
      // default is "a session is required" -- not a specific permission, since
      // inventing one for someone else's route would be guesswork. An
      // integration that needs to call these unauthenticated is what
      // auth.public_app_routes exists for.
      {"/api/app*", "", Guard::kAuthenticated, Permission::kCount},

      // -- over-the-air updates ---------------------------------------------------
      {"/api/update/status", "", Guard::kPermission, Permission::kUpdateRead},
      {"/api/update/check", "", Guard::kPermission, Permission::kUpdateRead},
      {"/api/update/dismiss", "", Guard::kPermission, Permission::kUpdateRead},
      {"/api/update/install", "", Guard::kPermission, Permission::kUpdateInstall},

      // -- card reader clients ------------------------------------------------------
      {"/api/card/clients", "GET", Guard::kPermission, Permission::kCardClientRead},
      {"/api/card/clients*", "", Guard::kPermission, Permission::kCardClientWrite},
      {"/api/card/cache", "GET", Guard::kPermission, Permission::kDeviceRead},
      {"/api/card/cache", "", Guard::kPermission, Permission::kDeviceControl},

      // -- Modbus / PLC ----------------------------------------------------------------
      {"/api/modbus/io", "", Guard::kPermission, Permission::kDeviceRead},
      {"/api/modbus/output", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/modbus/register", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/modbus/test", "", Guard::kPermission, Permission::kDeviceControl},

      // -- hardware tests ----------------------------------------------------------------
      // All of these drive real hardware. The relay and door routes can open a
      // physical door, which is why they are RELAY_CONTROL rather than a
      // generic test permission.
      {"/api/test/relay*", "", Guard::kPermission, Permission::kRelayControl},
      {"/api/test/aux-relay*", "", Guard::kPermission, Permission::kRelayControl},
      {"/api/test/beeper", "", Guard::kPermission, Permission::kRelayControl},
      {"/api/test/zk/door", "", Guard::kPermission, Permission::kRelayControl},
      {"/api/test/zk/device/control", "", Guard::kPermission, Permission::kRelayControl},
      {"/api/test/zk*", "", Guard::kPermission, Permission::kZkProtocolTest},
      {"/api/test/rabbitmq*", "", Guard::kPermission, Permission::kRabbitmqTest},
      {"/api/mq/test", "", Guard::kPermission, Permission::kRabbitmqTest},
      {"/api/mq/publish", "", Guard::kPermission, Permission::kRabbitmqTest},
      {"/api/test/aux-input/status", "", Guard::kPermission, Permission::kDeviceRead},
      {"/api/test/button/status", "", Guard::kPermission, Permission::kDeviceRead},
      {"/api/test/log", "GET", Guard::kPermission, Permission::kLogRead},
      {"/api/test/log", "", Guard::kPermission, Permission::kLogManage},
      {"/api/test/scenario*", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/test/serial", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/test/modbus", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/test/rest", "", Guard::kPermission, Permission::kSystemConfigRead},
      {"/api/serial/test", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/serial2/led-test", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/rfid/test", "", Guard::kPermission, Permission::kDeviceControl},
      {"/api/rest/test", "", Guard::kPermission, Permission::kSystemConfigRead},
  };
  return kPolicies;
}

namespace {

bool MethodMatches(const char* methods, const std::string& method) {
  if (methods == nullptr || *methods == '\0') return true;  // any
  const size_t methodLength = method.size();
  const char* cursor = methods;
  while (*cursor != '\0') {
    const char* comma = std::strchr(cursor, ',');
    const size_t span = comma ? static_cast<size_t>(comma - cursor) : std::strlen(cursor);
    if (span == methodLength && std::strncmp(cursor, method.c_str(), span) == 0) return true;
    if (!comma) break;
    cursor = comma + 1;
  }
  return false;
}

bool PathMatches(const char* pattern, const std::string& path) {
  const size_t length = std::strlen(pattern);
  if (length == 0) return false;
  if (pattern[0] == '*') {
    const size_t suffixLength = length - 1;
    if (path.size() < suffixLength) return false;
    return path.compare(path.size() - suffixLength, suffixLength, pattern + 1, suffixLength) == 0;
  }
  if (pattern[length - 1] == '*') {
    return path.compare(0, length - 1, pattern, length - 1) == 0;
  }
  return path == pattern;
}

}  // namespace

RoutePolicy PolicyFor(const std::string& path, const std::string& method) {
  for (const RoutePolicy& policy : RoutePolicies()) {
    if (PathMatches(policy.pattern, path) && MethodMatches(policy.methods, method)) return policy;
  }
  // Unclassified: require a session. A route added without a policy entry is
  // then merely inconvenient rather than silently wide open.
  return RoutePolicy{"", "", Guard::kAuthenticated, Permission::kCount};
}

}  // namespace hsf
