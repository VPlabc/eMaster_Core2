#pragma once

// The Crow middleware that enforces request/AdvanceUpdate.md Phase 1 on every
// request to the configuration app, in the order the plan's diagram gives:
//
//   Request -> Rate limit -> Authentication -> Authorization -> handler
//                                                                  |
//                                                     Audit + security headers
//
// THIS HEADER LIVES UNDER src/, NOT include/hsf/. Every other module's public
// header is in include/hsf/ and is deliberately crow-free -- WebServer.h uses
// a pimpl precisely so that crow.h stays out of the public surface (see the
// note on WebServer::Impl). This type IS a Crow middleware; it cannot avoid
// crow.h, and nothing outside WebServer.cpp constructs one. Putting it beside
// its only user keeps that property intact rather than punching a hole in it.

#include <crow.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>

#include "hsf/ConfigManager.h"
#include "hsf/Logger.h"
#include "hsf/security/Permissions.h"
#include "hsf/security/RateLimiter.h"
#include "hsf/security/SecurityStore.h"
#include "hsf/security/Validation.h"

namespace hsf {

// Audit event names (request/AdvanceUpdate.md section 1.10). String constants
// rather than an enum because they are queried by name over REST.
namespace audit {
inline constexpr const char* kLoginSuccess = "LOGIN_SUCCESS";
inline constexpr const char* kLoginFailed = "LOGIN_FAILED";
inline constexpr const char* kAccountLocked = "ACCOUNT_LOCKED";
inline constexpr const char* kLogout = "LOGOUT";
inline constexpr const char* kTokenExpired = "TOKEN_EXPIRED";
inline constexpr const char* kAccessDenied = "ACCESS_DENIED";
inline constexpr const char* kRateLimited = "RATE_LIMITED";
inline constexpr const char* kConfigChanged = "CONFIG_CHANGED";
inline constexpr const char* kDeviceControl = "DEVICE_CONTROL";
inline constexpr const char* kLuaWrite = "LUA_WRITE";
inline constexpr const char* kLuaRun = "LUA_RUN";
inline constexpr const char* kUpdateInstall = "UPDATE_INSTALL";
inline constexpr const char* kUserCreated = "USER_CREATED";
inline constexpr const char* kUserChanged = "USER_PERMISSION_CHANGED";
inline constexpr const char* kPasswordChanged = "PASSWORD_CHANGED";
}  // namespace audit

struct SecurityMiddleware {
  // Per-request state, reachable from a handler with
  // app.get_context<SecurityMiddleware>(req).
  struct context {
    bool authenticated = false;
    Session session;
    std::string source_ip;
    // Set when before_handle already answered; after_handle uses it to avoid
    // double-writing headers onto a finished response.
    bool short_circuited = false;
  };

  // Two buckets, because they defend different things. `login` throttles
  // credential guessing from one source; `api` stops a client from burning
  // gateway CPU on any endpoint. Sharing one bucket would let ordinary
  // dashboard polling exhaust the allowance that is supposed to be protecting
  // the login endpoint.
  RateLimiter loginLimiter{0.2, 5.0};
  RateLimiter apiLimiter{40.0, 120.0};

  // --- helpers ---------------------------------------------------------------

  static std::string ClientIp(const crow::request& req) {
    // X-Forwarded-For is NOT consulted. It is trivially forged, and trusting it
    // would let one attacker present a fresh "source" per request and walk
    // straight through both the rate limiter and the lockout. If this gateway
    // is ever deployed behind a real reverse proxy, that trust has to be an
    // explicit, configured decision -- not a default.
    return req.remote_ip_address;
  }

  static std::string BearerToken(const crow::request& req) {
    std::string header = req.get_header_value("Authorization");
    if (header.empty()) return std::string();
    // Case-insensitive scheme, exactly one space, per RFC 6750.
    static const std::string kPrefix = "bearer ";
    if (header.size() <= kPrefix.size()) return std::string();
    std::string scheme = header.substr(0, kPrefix.size());
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    if (scheme != kPrefix) return std::string();

    std::string token = header.substr(kPrefix.size());
    // Trim: a shell one-liner that reads the token from a file brings a
    // trailing newline with it, the same trap the card API's API-Key hit.
    const size_t first = token.find_first_not_of(" \t\r\n");
    const size_t last = token.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return token.substr(first, last - first + 1);
  }

  // Path without query string, which is what the policy table matches on.
  static std::string PathOf(const crow::request& req) {
    std::string path = req.url;
    const size_t question = path.find('?');
    if (question != std::string::npos) path = path.substr(0, question);
    return path;
  }

  static void Deny(crow::response& res, int code, const std::string& error, context& ctx) {
    // A fixed error vocabulary, never an internal message
    // (request/AdvanceUpdate.md section 1.9). The detail goes to the audit log
    // and the runtime log, where it is protected.
    res.code = code;
    res.set_header("Content-Type", "application/json");
    res.body = nlohmann::json{{"success", false}, {"error", error}}.dump();
    ctx.short_circuited = true;
    res.end();
  }

  void Audit(const AuthConfig& config, const std::string& event, const std::string& username,
             const std::string& ip, const std::string& resource, const std::string& result,
             const std::string& reason) const {
    if (!config.audit_log) return;
    SecurityStore::Instance().RecordAudit(event, Validate::SanitiseForLog(username, 64),
                                          Validate::SanitiseForLog(ip, 64),
                                          Validate::SanitiseForLog(resource, 200), result,
                                          Validate::SanitiseForLog(reason, 200));
  }

  // --- the pipeline -----------------------------------------------------------

  void before_handle(crow::request& req, crow::response& res, context& ctx) {
    const AuthConfig config = ConfigManager::Instance().GetAuth();
    ctx.source_ip = ClientIp(req);
    const std::string path = PathOf(req);
    const std::string method = crow::method_name(req.method);

    // CORS preflight never carries credentials and must be answerable before
    // any auth decision, or the browser never gets to send the real request.
    if (req.method == crow::HTTPMethod::Options) {
      ctx.short_circuited = true;
      res.code = 204;
      res.end();
      return;
    }

    // 1. Body ceiling, before anything reads or parses it (section 1.12 C).
    const size_t bodyLimit = (path.rfind("/api/lua", 0) == 0)
                                 ? static_cast<size_t>(std::max(1024, config.max_lua_body_bytes))
                                 : static_cast<size_t>(std::max(1024, config.max_body_bytes));
    if (req.body.size() > bodyLimit) {
      Audit(config, audit::kAccessDenied, "", ctx.source_ip, path, "DENIED", "body too large");
      Deny(res, 413, "REQUEST_TOO_LARGE", ctx);
      return;
    }

    // 2. Rate limiting (section 1.4).
    if (config.rate_limit_enabled) {
      loginLimiter.Configure(config.login_rate_per_sec, config.login_burst);
      apiLimiter.Configure(config.api_rate_per_sec, config.api_burst);

      const bool isLogin = (path == "/api/auth/login");
      RateLimiter& limiter = isLogin ? loginLimiter : apiLimiter;
      if (!limiter.Allow(ctx.source_ip)) {
        const double retry = limiter.RetryAfter(ctx.source_ip);
        res.set_header("Retry-After", std::to_string(static_cast<int>(retry + 0.999)));
        Audit(config, audit::kRateLimited, "", ctx.source_ip, path, "DENIED",
              isLogin ? "login rate limit" : "api rate limit");
        Deny(res, 429, "TOO_MANY_REQUESTS", ctx);
        return;
      }
    }

    // 3. With auth switched off the pipeline stops here: rate limiting and the
    // body ceiling still apply (they are not authentication, and turning off
    // logins is no reason to accept a 500 MB body), but nothing is
    // authenticated and every route runs as before.
    if (!config.enabled) return;

    RoutePolicy policy = PolicyFor(path, method);

    // A script-served route is only public when the installation says so.
    if (config.public_app_routes && path.rfind("/api/app", 0) == 0) {
      policy.guard = Guard::kPublic;
    }

    if (policy.guard == Guard::kPublic) return;
    if (policy.guard == Guard::kApiKey) return;  // the handler checks its own scheme

    // 4. Authentication.
    const std::string token = BearerToken(req);
    if (token.empty()) {
      Audit(config, audit::kAccessDenied, "", ctx.source_ip, path, "DENIED", "no bearer token");
      Deny(res, 401, "UNAUTHENTICATED", ctx);
      return;
    }
    std::optional<Session> session = SecurityStore::Instance().Authenticate(token);
    if (!session) {
      // Expired, revoked, unknown, or a disabled account -- all one answer.
      // Distinguishing them tells an attacker which tokens once existed.
      Audit(config, audit::kTokenExpired, "", ctx.source_ip, path, "DENIED", "invalid or expired token");
      Deny(res, 401, "UNAUTHENTICATED", ctx);
      return;
    }
    ctx.authenticated = true;
    ctx.session = *session;

    // 5. Authorization.
    if (policy.guard == Guard::kPermission && !RoleHasPermission(session->role, policy.permission)) {
      Audit(config, audit::kAccessDenied, session->username, ctx.source_ip, path, "DENIED",
            "missing " + PermissionName(policy.permission));
      Deny(res, 403, "FORBIDDEN", ctx);
      return;
    }
    // Input validation and business logic are the handler's own; the reusable
    // helpers for the first are in Validation.h.
  }

  void after_handle(crow::request& req, crow::response& res, context& ctx) {
    const AuthConfig config = ConfigManager::Instance().GetAuth();

    if (config.security_headers) {
      // Section 1.8. The CSP matches what the pages actually load: Bootstrap
      // and Chart.js come from jsdelivr, everything else is same-origin, and
      // 'unsafe-inline' is required because several pages carry inline
      // <script> and style attributes. Tightening that means editing the
      // frontend first, so it is honestly permissive rather than quietly
      // broken.
      res.set_header("Content-Security-Policy",
                     "default-src 'self'; "
                     "script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; "
                     "style-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; "
                     "img-src 'self' data:; "
                     "connect-src 'self' ws: wss:; "
                     "font-src 'self' data: https://cdn.jsdelivr.net; "
                     "frame-ancestors 'none'; "
                     "base-uri 'self'; "
                     "form-action 'self'");
      res.set_header("X-Content-Type-Options", "nosniff");
      res.set_header("X-Frame-Options", "DENY");
      res.set_header("Referrer-Policy", "no-referrer");
      // The API answers with credentials in a header; nothing here should ever
      // be reused from a cache shared with another user.
      if (PathOf(req).rfind("/api/", 0) == 0) {
        res.set_header("Cache-Control", "no-store");
      }
    }

    // CORS: an allowlist, never "*" (section 1.12 B). With no configured
    // origins no header is emitted at all, which leaves the browser's
    // same-origin default in place -- the right answer for a UI the gateway
    // serves itself.
    const std::string origin = req.get_header_value("Origin");
    if (!origin.empty() && !config.allowed_origins.empty()) {
      std::stringstream stream(config.allowed_origins);
      std::string candidate;
      while (std::getline(stream, candidate, ',')) {
        const size_t first = candidate.find_first_not_of(" \t");
        const size_t last = candidate.find_last_not_of(" \t");
        if (first == std::string::npos) continue;
        if (candidate.substr(first, last - first + 1) == origin) {
          res.set_header("Access-Control-Allow-Origin", origin);
          res.set_header("Vary", "Origin");
          res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, API-Key");
          res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
          break;
        }
      }
    }
  }
};

}  // namespace hsf
