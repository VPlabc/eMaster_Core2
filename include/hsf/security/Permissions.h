#pragma once

#include <string>
#include <vector>

namespace hsf {

// The authorization vocabulary (request/AdvanceUpdate.md sections 1.2 and
// 2.10), and the table that says which permission each API route needs.
//
// WHY A TABLE AND NOT A CHECK PER HANDLER. There are 86 routes on the
// configuration app. Ninety-odd `if (!Authorized(req, "...")) return 403;`
// lines at the top of ninety-odd lambdas is not a security layer, it is
// ninety-odd chances to forget one -- and a forgotten one is invisible,
// because the route keeps working. Putting the mapping in a single ordered
// table means the whole policy can be read on one screen and reviewed as a
// unit, and the middleware applies it to every request before any handler
// runs. A route nobody classified gets the default (authentication required),
// so forgetting to add an entry fails closed rather than open.

enum class Permission {
  kSystemConfigRead,
  kSystemConfigWrite,
  kConfigExport,
  kConfigImport,
  kDeviceRead,
  kDeviceControl,
  kRelayControl,
  kZkProtocolTest,
  kRabbitmqTest,
  kLuaRead,
  kLuaWrite,
  kLuaRun,
  // Declared now, unused until Phase 2 builds the compile/deploy pipeline.
  // They are here so the role tables and the audit log do not have to change
  // shape when that lands.
  kLuaCompile,
  kLuaDeploy,
  kLuaRollback,
  kLogRead,
  kLogManage,
  kCardClientRead,
  kCardClientWrite,
  kUpdateRead,
  kUpdateInstall,
  kUserRead,
  kUserWrite,
  kCount
};

std::string PermissionName(Permission permission);
// kCount when the name is not recognised.
Permission PermissionFromName(const std::string& name);

// Roles are named bundles of permissions. Deliberately a closed set rather
// than per-user permission rows: this gateway has a handful of accounts, and
// an arbitrary permission matrix is a lot of surface to secure in exchange for
// flexibility nobody has asked for. A user's effective permissions are exactly
// their role's.
enum class Role {
  kAdmin,     // everything, including user management and Lua
  kOperator,  // run the machine: doors, relays, device tests, read config
  kUser,      // look, don't touch
  kCount
};

std::string RoleName(Role role);
Role RoleFromName(const std::string& name);
bool RoleHasPermission(Role role, Permission permission);
std::vector<std::string> PermissionNamesFor(Role role);

// How a route is guarded.
enum class Guard {
  kPublic,        // no token: static assets and the login endpoint itself
  kAuthenticated, // any valid session, no specific permission
  kPermission,    // a valid session holding `permission`
  kApiKey         // authenticated by its own scheme (the card readers' API-Key)
};

struct RoutePolicy {
  // Matched against the request path. A trailing '*' makes it a prefix match,
  // a leading '*' a suffix match; otherwise the whole path must be equal.
  const char* pattern;
  // Empty means "any method". Otherwise a comma-separated list, e.g. "GET,HEAD".
  // This is what lets one path be readable by a viewer and writable only by an
  // admin -- /api/config is exactly that case.
  const char* methods;
  Guard guard;
  Permission permission;  // meaningful only when guard == kPermission
};

// Ordered, first match wins, so a specific rule can precede a general one.
// Defined in Permissions.cpp.
const std::vector<RoutePolicy>& RoutePolicies();

// Resolves one request. Falls through to {kAuthenticated} when nothing matches
// -- see the note at the top about failing closed.
RoutePolicy PolicyFor(const std::string& path, const std::string& method);

}  // namespace hsf
