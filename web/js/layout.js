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
  var NAV = [
    { href: "/index.html", label: "Dashboard" },
    { href: "/test_tools.html", label: "Test Tools" },
    { href: "/plugins.html", label: "Plugins" },
    { href: "/firmware.html", label: "Firmware", role: "admin" },
    { href: "/config_file.html", label: "Config Files" },
    { href: "/lua_editor.html", label: "Lua Editor", role: "admin" },
    { href: "/config.html", label: "Configuration", role: "admin" },
    { href: "/security.html", label: "Security", role: "admin" },
    { href: "/logs.html", label: "Logs" }
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
      var active = item.href === path ? " active" : "";
      var gate = item.role ? ' data-requires-role="' + item.role + '"' : "";
      html += '<a class="nav-link' + active + '" href="' + item.href + '"' + gate + '>' +
              item.label + "</a>";
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

    var notice = document.createElement("div");
    notice.className = "alert alert-warning alert-dismissible fade show m-3 mb-0";
    notice.setAttribute("role", "alert");

    var text = document.createElement("span");
    // textContent, not innerHTML -- `denied` is attacker-controllable via a
    // crafted link, and this lands in the page verbatim.
    text.textContent = "Your account does not have access to " + denied + ".";
    notice.appendChild(text);

    var close = document.createElement("button");
    close.type = "button";
    close.className = "btn-close";
    close.setAttribute("data-bs-dismiss", "alert");
    close.setAttribute("aria-label", "Close");
    notice.appendChild(close);

    mount.parentNode.insertBefore(notice, mount.nextSibling);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", render);
  } else {
    render();
  }
})();

// Shared application notifications. Keep destructive confirmations and
// errors inside the UI instead of using browser alert()/confirm() dialogs.
(function () {
  "use strict";

  function ensureHost(id, className) {
    var node = document.getElementById(id);
    if (node) return node;
    node = document.createElement("div");
    node.id = id;
    node.className = className;
    document.body.appendChild(node);
    return node;
  }

  window.HsfNotify = function (message, ok) {
    var host = ensureHost("hsfNotificationHost", "position-fixed top-0 end-0 p-3");
    var item = document.createElement("div");
    item.className = "alert shadow-sm " + (ok === false ? "alert-danger" : "alert-success");
    item.setAttribute("role", "status");
    item.textContent = String(message);
    host.appendChild(item);
    window.setTimeout(function () { item.remove(); }, 4500);
  };

  window.HsfConfirm = function (message, title) {
    return new Promise(function (resolve) {
      var backdrop = document.createElement("div");
      backdrop.className = "modal fade show";
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
      var finish = function (value) {
        backdrop.remove();
        resolve(value);
      };
      backdrop.querySelector(".btn-close").addEventListener("click", function () { finish(false); });
      backdrop.querySelector('[data-choice="no"]').addEventListener("click", function () { finish(false); });
      backdrop.querySelector('[data-choice="yes"]').addEventListener("click", function () { finish(true); });
      document.body.appendChild(backdrop);
    });
  };
})();
