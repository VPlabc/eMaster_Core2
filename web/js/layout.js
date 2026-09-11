/* Shared chrome: navbar, active-page marking, theme toggle, role gating.
 *
 * Rendered from one definition rather than copy-pasted into every page --
 * six hand-maintained <nav> blocks is how they drift, and role-based hiding
 * (Phase 2) needs exactly one place to apply. Pages just provide
 * <div id="hsfNav"></div>.
 */
(function () {
  "use strict";

  // `role: "admin"` marks a link only Admin sees. This is presentation only
  // -- the backend has to enforce the same rule, since hiding a link doesn't
  // stop anyone calling the endpoint directly.
  // Keep the primary navigation about operator responsibilities. Protocols,
  // plugins and individual tools belong in their owning workspace, not in a
  // growing row of top-level tabs.
  var NAV = [
    { href: "/index.html", label: "Dashboard" },
    { href: "/lua_editor.html", label: "Edge Logic", role: "admin", children: [
      { href: "/lua_editor.html", label: "Lua Scripts", role: "admin" },
      { href: "/visual_flow.html", label: "Visual Flow", role: "admin" },
      { href: "/dynamic_config.html", label: "Variables", role: "admin" }
    ] },
    { href: "/plugins.html", label: "Devices", children: [
      { href: "/plugins.html", label: "Device List" },
      { href: "/card_clients.html", label: "Device Details" }
    ] },
    { href: "/config.html", label: "Configuration", role: "admin", children: [
      { href: "/config.html", label: "Configuration Workspace", role: "admin" },
      { href: "/security.html", label: "Security", role: "admin" },
      { href: "/config_file.html", label: "Import / Export", role: "admin" }
    ] },
    { href: "/test_tools.html", label: "Tool", children: [
      { href: "/test_tools.html", label: "Protocol Test" }
    ] },
    { href: "/logs.html", label: "System", children: [
      { href: "/logs.html", label: "Logs" },
      { href: "/firmware.html", label: "Firmware / OTA", role: "admin" }
    ] }
  ];

  function currentPath() {
    var p = window.location.pathname;
    if (p === "/" || p === "") return "/index.html";
    return p;
  }

  // auth.js runs first and always sets data-role, so this is only a fallback
  // for a page that somehow loads without it. Assume admin there rather than
  // client: the CSS gate hides [data-requires-role="admin"] whenever <html>
  // isn't data-role="admin", and defaulting the other way would silently
  // strip Configuration and Lua Editor from the nav with no way back.
  function ensureRole() {
    var root = document.documentElement;
    if (!root.getAttribute("data-role")) {
      root.setAttribute("data-role", "admin");
    }
  }

  function render() {
    var mount = document.getElementById("hsfNav");
    if (!mount) return;

    ensureRole();

    var path = currentPath();
    var html = '' +
      '<nav class="navbar navbar-expand-lg hsf-navbar sticky-top">' +
      '  <div class="container-fluid">' +
      '    <a class="navbar-brand" href="/index.html">eMaster Gateway</a>' +
      '    <button class="navbar-toggler border-0 p-1" type="button" data-bs-toggle="collapse"' +
      '            data-bs-target="#hsfNavItems" aria-controls="hsfNavItems" aria-expanded="false"' +
      '            aria-label="Toggle navigation">' +
      '      <span class="navbar-toggler-icon"></span>' +
      '    </button>' +
      '    <div class="collapse navbar-collapse" id="hsfNavItems">' +
      '      <div class="navbar-nav me-auto">';

    for (var i = 0; i < NAV.length; i++) {
      var item = NAV[i];
      var active = item.href === path || (item.children && item.children.some(function (child) {
        return child.href === path;
      })) ? " active" : "";
      var gate = item.role ? ' data-requires-role="' + item.role + '"' : "";
      if (!item.children) {
        html += '<a class="nav-link' + active + '" href="' + item.href + '"' + gate + '>' +
                item.label + "</a>";
      } else {
        var menuId = "hsfMenu" + i;
        html += '<div class="nav-item dropdown"' + gate + '>' +
                '<a class="nav-link dropdown-toggle' + active + '" href="' + item.href +
                '" role="button" data-bs-toggle="dropdown" aria-expanded="false">' + item.label + '</a>' +
                '<ul class="dropdown-menu" id="' + menuId + '">';
        item.children.forEach(function (child) {
          var childGate = child.role ? ' data-requires-role="' + child.role + '"' : "";
          html += '<li><a class="dropdown-item" href="' + child.href + '"' + childGate + '>' +
                  child.label + '</a></li>';
        });
        html += '</ul></div>';
      }
    }

    html += '' +
      '      </div>' +
      '      <div class="d-flex align-items-center gap-2">' +
      '        <span class="hsf-badge hsf-badge-idle" id="hsfRoleBadge" hidden></span>' +
      '        <button class="hsf-theme-toggle" data-hsf-theme-toggle type="button"' +
      '                title="Toggle light / dark theme" aria-label="Toggle light / dark theme">' +
      '          <span class="icon-light" aria-hidden="true">&#9788;</span>' +
      '          <span class="icon-dark" aria-hidden="true">&#9790;</span>' +
      '        </button>' +
      '        <button class="btn btn-sm btn-outline-secondary" id="hsfLogout" type="button" hidden>Sign out</button>' +
      '      </div>' +
      '    </div>' +
      '  </div>' +
      '</nav>';

    mount.innerHTML = html;

    // theme.js binds toggles on DOMContentLoaded; this nav is injected after
    // that, so bind the one we just created.
    var toggle = mount.querySelector("[data-hsf-theme-toggle]");
    if (toggle && window.HsfTheme) {
      toggle.addEventListener("click", function () {
        window.HsfTheme.toggle();
      });
    }

    if (window.HsfAuth && typeof window.HsfAuth.decorateNav === "function") {
      window.HsfAuth.decorateNav(mount);
    }

    showDeniedNotice(mount);
  }

  // auth.js bounces a Client off an admin-only page with ?denied=<path>.
  // Without this the redirect is silent and just looks like a broken link.
  function showDeniedNotice(mount) {
    var denied = new URLSearchParams(window.location.search).get("denied");
    if (!denied) return;
    // Defer until the shared modal API below has been registered. `denied` is
    // still passed as text, never markup, because it originates in the URL.
    window.setTimeout(function () {
      window.HsfAlert("Your account does not have access to " + denied + ".", "Access denied", "warning");
    }, 0);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", render);
  } else {
    render();
  }
})();

// Shared application messages. Actionable feedback is modal rather than a
// page-level alert or disappearing toast, so it cannot be missed behind the
// sticky application header.
(function () {
  "use strict";

  function makeModal(message, title, variant, actions) {
    var backdrop = document.createElement("div");
    backdrop.className = "modal fade show hsf-modal hsf-alert-modal hsf-alert-" + variant;
    backdrop.style.display = "block";
    backdrop.setAttribute("role", "dialog");
    backdrop.setAttribute("aria-modal", "true");
    backdrop.innerHTML =
      '<div class="modal-dialog modal-dialog-centered"><div class="modal-content">' +
      '<div class="modal-header"><h5 class="modal-title"></h5>' +
      '<button type="button" class="btn-close" aria-label="Close"></button></div>' +
      '<div class="modal-body"></div><div class="modal-footer"></div></div></div>';
    backdrop.querySelector(".modal-title").textContent = title;
    backdrop.querySelector(".modal-body").textContent = String(message);
    actions(backdrop);
    document.body.appendChild(backdrop);
    backdrop.querySelector(".btn-close").focus();
    return backdrop;
  }

  window.HsfAlert = function (message, title, variant) {
    var level = variant || "info";
    var dialog = makeModal(message, title || (level === "error" ? "Error" : "Notice"), level, function (backdrop) {
      var close = document.createElement("button");
      close.type = "button";
      close.className = "btn btn-primary";
      close.textContent = "Close";
      var finish = function () { document.removeEventListener("keydown", escape); backdrop.remove(); };
      close.addEventListener("click", finish);
      backdrop.querySelector(".modal-footer").appendChild(close);
      backdrop.addEventListener("click", function (event) { if (event.target === backdrop) finish(); });
      var escape = function (event) { if (event.key === "Escape") finish(); };
      document.addEventListener("keydown", escape);
    });
    return dialog;
  };

  // Backward-compatible name for existing pages. `false` maps to an error;
  // callers may progressively use HsfAlert when they need a specific title.
  window.HsfNotify = function (message, ok) {
    return window.HsfAlert(message, ok === false ? "Error" : "Success", ok === false ? "error" : "success");
  };

  window.HsfConfirm = function (message, title) {
    return new Promise(function (resolve) {
      var backdrop = document.createElement("div");
      backdrop.className = "modal fade show hsf-modal";
      backdrop.style.display = "block";
      backdrop.setAttribute("role", "dialog");
      backdrop.setAttribute("aria-modal", "true");
      backdrop.innerHTML =
        '<div class="modal-dialog modal-dialog-centered"><div class="modal-content">' +
        '<div class="modal-header"><h5 class="modal-title"></h5>' +
        '<button type="button" class="btn-close" aria-label="Close"></button></div>' +
        '<div class="modal-body"></div><div class="modal-footer">' +
        '<button type="button" class="btn btn-secondary" data-choice="no">Cancel</button>' +
        '<button type="button" class="btn btn-danger" data-choice="yes">Confirm</button>' +
        '</div></div></div>';
      backdrop.querySelector(".modal-title").textContent = title || "Confirm action";
      backdrop.querySelector(".modal-body").textContent = String(message);
      var escape;
      var finish = function (value) {
        document.removeEventListener("keydown", escape);
        backdrop.remove();
        resolve(value);
      };
      backdrop.querySelector(".btn-close").addEventListener("click", function () { finish(false); });
      backdrop.querySelector('[data-choice="no"]').addEventListener("click", function () { finish(false); });
      backdrop.querySelector('[data-choice="yes"]').addEventListener("click", function () { finish(true); });
      backdrop.addEventListener("click", function (event) { if (event.target === backdrop) finish(false); });
      escape = function (event) { if (event.key === "Escape") finish(false); };
      document.addEventListener("keydown", escape);
      document.body.appendChild(backdrop);
    });
  };
})();
