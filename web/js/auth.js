/* Authentication and role gating.
 *
 * Loaded from <head>, ahead of layout.js, so <html data-role> is set before
 * the navbar renders and before any protected markup paints.
 *
 * THIS FILE USED TO BE A LIE. It checked credentials in the browser against a
 * hardcoded table, kept "the session" in localStorage, and said of itself
 * "THIS IS NOT SECURITY" -- correctly, because every /api/* route was wide
 * open to anyone who could reach the port. It now talks to a real backend:
 * POST /api/auth/login returns a bearer token, and the gateway enforces
 * authentication and per-permission authorization on every route in its
 * middleware (src/security/). What remains here is presentation.
 *
 * WHAT THIS STILL IS NOT. Hiding a nav link does not protect an endpoint; the
 * backend does that now, and this only stops a viewer being shown buttons that
 * would 403. And the token lives in localStorage, which means script injected
 * into a page can read it -- the standard bearer-token trade. It buys immunity
 * from CSRF (nothing is sent automatically by the browser, so
 * request/AdvanceUpdate.md section 1.12.A does not apply), and it is why the
 * gateway sets a strict Content-Security-Policy and why tokens expire in 30
 * minutes and are revoked server-side on logout and password change.
 */
(function () {
  "use strict";

  var STORAGE_KEY = "hsf.session";
  var LOGIN_PAGE = "/login.html";
  var root = document.documentElement;

  /* Whether the gateway requires authentication at all (auth.enabled). Read
     synchronously below, because the page gate has to decide before paint. */
  var authEnabled = true;

  function readSession() {
    try {
      var raw = localStorage.getItem(STORAGE_KEY);
      if (!raw) return null;
      var s = JSON.parse(raw);
      if (!s || !s.token || !s.username || !s.role) return null;
      /* Expired tokens are dropped here rather than waiting for the server to
         say 401, so a stale tab redirects to login instead of flashing an
         error on every poll. Clock skew just means the server decides. */
      if (s.expiresAt && Date.now() / 1000 > s.expiresAt) {
        clearSession();
        return null;
      }
      return s;
    } catch (e) {
      return null;
    }
  }

  function writeSession(session) {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(session));
    } catch (e) {
      /* Storage unavailable: the session lives for this page view only. */
    }
  }

  function clearSession() {
    try {
      localStorage.removeItem(STORAGE_KEY);
    } catch (e) {
      /* nothing to do */
    }
  }

  function currentPath() {
    var p = window.location.pathname;
    return p === "/" || p === "" ? "/index.html" : p;
  }

  function isLoginPage() {
    return currentPath() === LOGIN_PAGE;
  }

  /* Synchronous on purpose, and the only synchronous request in the frontend.
     The pre-paint gate below has to know whether a login is required before it
     decides to redirect, and an async answer would mean either a flash of the
     wrong page or a redirect loop on a gateway with auth switched off. It is a
     same-origin request to a route that reads one boolean. */
  function readAuthMode() {
    try {
      var xhr = new XMLHttpRequest();
      xhr.open("GET", "/api/auth/mode", false);
      xhr.send(null);
      if (xhr.status === 200) {
        var parsed = JSON.parse(xhr.responseText);
        return parsed.enabled !== false;
      }
    } catch (e) {
      /* Fall through. */
    }
    /* Unreachable or unparseable: assume authentication IS required. Guessing
       the other way would drop the login page from an unreachable gateway. */
    return true;
  }

  function login(username, password) {
    return fetch("/api/auth/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: username, password: password })
    })
      .then(function (response) {
        return response
          .json()
          .catch(function () {
            return {};
          })
          .then(function (data) {
            return { status: response.status, data: data };
          });
      })
      .then(function (result) {
        if (result.status === 200 && result.data.success) {
          var session = {
            token: result.data.token,
            username: result.data.username,
            role: result.data.role,
            permissions: result.data.permissions || [],
            display: result.data.username,
            expiresAt: result.data.expires_at
          };
          writeSession(session);
          root.setAttribute("data-role", roleClass(session.role));
          return { ok: true, session: session };
        }
        if (result.status === 423) {
          var minutes = Math.ceil((result.data.retry_after_seconds || 900) / 60);
          return {
            ok: false,
            error: "Account locked after too many failed attempts. Try again in " + minutes + " minute" +
                   (minutes === 1 ? "" : "s") + "."
          };
        }
        if (result.status === 429) {
          return { ok: false, error: "Too many attempts. Wait a moment and try again." };
        }
        /* Everything else collapses to one message, matching what the gateway
           deliberately does not tell us. */
        return { ok: false, error: "Invalid username or password" };
      });
  }

  /* The nav and CSS gate were written against admin/client. Map the real role
     set onto that vocabulary so existing [data-requires-role="admin"] markup
     keeps meaning what it meant. */
  function roleClass(role) {
    return role === "ADMIN" ? "admin" : "client";
  }

  var HsfAuth = {
    session: readSession,

    authEnabled: function () {
      return authEnabled;
    },

    token: function () {
      var s = readSession();
      return s ? s.token : null;
    },

    role: function () {
      var s = readSession();
      return s ? s.role : null;
    },

    isAuthenticated: function () {
      return readSession() !== null;
    },

    hasRole: function (role) {
      if (!role) return true;
      if (!authEnabled) return true;
      return roleClass(this.role()) === role;
    },

    /* Permission names match the backend's exactly (see
       src/security/Permissions.cpp), so a page can ask the same question the
       middleware will ask. */
    hasPermission: function (permission) {
      if (!authEnabled) return true;
      var s = readSession();
      if (!s || !s.permissions) return false;
      return s.permissions.indexOf(permission) !== -1;
    },

    login: login,

    logout: function () {
      var s = readSession();
      var done = function () {
        clearSession();
        window.location.href = LOGIN_PAGE;
      };
      if (!s) return done();
      /* Tell the gateway so the token is revoked server-side, not merely
         forgotten here -- otherwise it stays valid until it expires. */
      fetch("/api/auth/logout", {
        method: "POST",
        headers: { Authorization: "Bearer " + s.token }
      })
        .then(done)
        .catch(done);
    },

    /* Called by layout.js once the navbar exists. */
    decorateNav: function (mount) {
      var s = readSession();

      var badge = mount.querySelector("#hsfRoleBadge");
      if (badge && s) {
        badge.textContent = s.display || s.role;
        badge.hidden = false;
      }

      var logout = mount.querySelector("#hsfLogout");
      if (logout && s) {
        logout.hidden = false;
        logout.addEventListener("click", function () {
          HsfAuth.logout();
        });
      }
    }
  };

  window.HsfAuth = HsfAuth;

  /* ---- attach the token to every API call -------------------------------
   *
   * One wrapper here rather than editing every fetch() in every page script.
   * That is not only less work: it means a page added later cannot forget to
   * authenticate, and there is a single place where a 401 is handled.
   */
  var nativeFetch = window.fetch.bind(window);
  window.fetch = function (input, init) {
    init = init || {};
    var url = typeof input === "string" ? input : (input && input.url) || "";
    var sameOrigin = url.indexOf("http://") !== 0 && url.indexOf("https://") !== 0;
    var session = readSession();

    if (sameOrigin && session && url.indexOf("/api/") === 0) {
      var headers = new Headers(init.headers || (typeof input === "object" ? input.headers : undefined));
      if (!headers.has("Authorization")) headers.set("Authorization", "Bearer " + session.token);
      init = Object.assign({}, init, { headers: headers });
    }

    return nativeFetch(input, init).then(function (response) {
      if (response.status === 401 && sameOrigin && url.indexOf("/api/") === 0 && !isLoginPage()) {
        /* The token was rejected -- expired, revoked, or the account was
           disabled while the tab was open. Send them to login rather than
           letting the page render a wall of failed requests. */
        clearSession();
        var target = window.location.pathname + window.location.search;
        window.location.replace(LOGIN_PAGE + "?next=" + encodeURIComponent(target));
      }
      return response;
    });
  };

  /* ---- Gate the page, before anything paints ---------------------- */

  authEnabled = readAuthMode();
  var session = readSession();

  if (!authEnabled) {
    /* Authentication is switched off gateway-side. Show everything rather
       than gating on a session that will never exist -- and make that visible
       in the DOM so it is greppable when someone wonders why. */
    root.setAttribute("data-role", "admin");
    root.setAttribute("data-auth", "disabled");
    if (isLoginPage()) window.location.replace("/index.html");
  } else {
    root.setAttribute("data-role", session ? roleClass(session.role) : "none");
    root.setAttribute("data-auth", "enabled");

    if (!isLoginPage()) {
      if (!session) {
        var next = window.location.pathname + window.location.search;
        window.location.replace(LOGIN_PAGE + "?next=" + encodeURIComponent(next));
      } else {
        /* Per-page role requirement, declared as
             <meta name="hsf-require-role" content="admin">
           so a viewer typing /lua_editor.html directly is bounced too, not
           just denied the nav link. The backend refuses the underlying calls
           either way; this only avoids showing a page that cannot work. */
        var meta = document.querySelector('meta[name="hsf-require-role"]');
        var required = meta ? meta.getAttribute("content") : null;
        if (required && roleClass(session.role) !== required) {
          window.location.replace("/index.html?denied=" + encodeURIComponent(currentPath()));
        }
      }
    } else if (session) {
      var params = new URLSearchParams(window.location.search);
      var wanted = params.get("next");
      window.location.replace(
        wanted && wanted.charAt(0) === "/" && wanted.charAt(1) !== "/" ? wanted : "/index.html"
      );
    }
  }
})();
