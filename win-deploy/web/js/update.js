/* Software update popup.
 *
 * Loaded on every page that has the shared chrome, not just Configuration --
 * whoever is standing at the machine when a release lands is as likely to be
 * looking at Logs or Test Tools, and a notice they have to go hunting for is a
 * notice nobody reads.
 *
 * The gateway pushes {"type":"update", ...} frames over the existing /ws
 * channel (see WebServer's BroadcastLoop); this file also fetches
 * /api/update/status once on load, so a page opened after the announcement
 * still sees the offer instead of waiting for the next state change.
 *
 * Actions go back over REST rather than up the socket. request/CICD.md drew it
 * the other way, but /ws has always been push-only, and a POST is what gives
 * the button a synchronous answer when the request is refused.
 *
 * Deliberately a corner toast, never a full-screen modal: this gateway may be
 * mid-transaction with someone standing in front of the cabinet, and an
 * unmissable dialog over the whole UI is exactly the wrong thing to put
 * between an operator and a door they are trying to open.
 */
(function () {
  "use strict";

  var STORAGE_KEY = "hsf.update.dismissed";
  var mount = null;
  var lastStatus = null;

  /* Whether the operator started something and is owed an answer.
   *
   * This gates whether an "error" state gets a popup at all: a background check
   * that could not reach the release server is a log line and a note on the
   * Configuration page, not an interruption. An error that came from a click
   * is different -- someone is waiting on it.
   *
   * It must survive re-renders. Status frames arrive once a second, so a flag
   * cleared while painting the error would take the error back off the screen
   * on the very next frame. Only the operator closing the toast, or the update
   * actually progressing, clears it. */
  var awaitingResult = false;

  /* Postponing is remembered per version in the browser as well as on the
     gateway. The server-side flag is the real one -- it survives a different
     browser -- but it also resets when the gateway restarts, and an operator
     who clicked Later does not want the popup back on every page navigation in
     the meantime. */
  function locallyDismissed(version) {
    try {
      return localStorage.getItem(STORAGE_KEY) === version;
    } catch (e) {
      return false;
    }
  }

  function rememberDismissed(version) {
    try {
      localStorage.setItem(STORAGE_KEY, version);
    } catch (e) {
      /* Private mode: the server-side flag still holds for this run. */
    }
  }

  function ensureMount() {
    if (mount) return mount;
    mount = document.createElement("div");
    mount.className = "hsf-update-toast";
    mount.setAttribute("role", "status");
    mount.setAttribute("aria-live", "polite");
    mount.hidden = true;
    document.body.appendChild(mount);
    return mount;
  }

  function escapeHtml(text) {
    return String(text == null ? "" : text)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function formatBytes(bytes) {
    if (!bytes || bytes < 0) return "";
    var units = ["B", "KB", "MB", "GB"];
    var value = bytes;
    var i = 0;
    while (value >= 1024 && i < units.length - 1) {
      value /= 1024;
      i++;
    }
    return (i === 0 ? value : value.toFixed(1)) + " " + units[i];
  }

  function hide() {
    if (mount) mount.hidden = true;
  }

  function postJson(url, body) {
    return fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {})
    }).then(function (response) {
      return response.json().then(function (data) {
        return { ok: response.ok, data: data };
      });
    });
  }

  /* ---- rendering ---------------------------------------------------- */

  function renderOffer(status) {
    var notes = (status.release_notes || []).slice(0, 5);
    var notesHtml = notes.length
      ? '<ul class="hsf-update-notes">' +
        notes
          .map(function (note) {
            return "<li>" + escapeHtml(note) + "</li>";
          })
          .join("") +
        "</ul>"
      : "";

    var size = formatBytes(status.size_bytes);
    var mandatory = !!status.mandatory;

    return (
      '<div class="hsf-update-head">' +
      '  <span class="hsf-update-title">' +
      (mandatory ? "Update required" : "New update available") +
      "</span>" +
      (mandatory
        ? ""
        : '  <button type="button" class="hsf-update-close" data-action="later" aria-label="Dismiss">&times;</button>') +
      "</div>" +
      '<div class="hsf-update-version">eMaster Gateway ' +
      escapeHtml(status.version) +
      "</div>" +
      '<div class="hsf-update-current">Current version: ' +
      escapeHtml(status.current_version) +
      (size ? " &middot; download " + escapeHtml(size) : "") +
      "</div>" +
      notesHtml +
      (mandatory
        ? '<p class="hsf-update-note">This release is required and cannot be postponed.</p>'
        : "") +
      '<div class="hsf-update-actions">' +
      (mandatory
        ? ""
        : '  <button type="button" class="btn btn-sm btn-outline-secondary" data-action="later">Later</button>') +
      '  <button type="button" class="btn btn-sm btn-primary" data-action="update">Update now</button>' +
      "</div>"
    );
  }

  function renderProgress(status) {
    var percent = typeof status.progress_percent === "number" ? status.progress_percent : -1;
    var label;
    switch (status.state) {
      case "downloading":
        label = percent >= 0 ? "Downloading... " + percent + "%" : "Downloading...";
        break;
      case "verifying":
        label = "Verifying checksum and signature...";
        break;
      case "installing":
        label = "Installing...";
        break;
      default:
        label = "Restarting the gateway...";
    }

    /* An indeterminate bar when the server sent no Content-Length: a bar stuck
       at 0% reads as "hung", which this very much is not. */
    var barClass = "hsf-update-bar" + (percent < 0 || status.state !== "downloading" ? " indeterminate" : "");
    var barStyle = percent >= 0 && status.state === "downloading" ? ' style="width:' + percent + '%"' : "";

    return (
      '<div class="hsf-update-head">' +
      '  <span class="hsf-update-title">Updating to ' +
      escapeHtml(status.version || "") +
      "</span>" +
      "</div>" +
      '<div class="hsf-update-current">' +
      escapeHtml(label) +
      "</div>" +
      '<div class="hsf-update-track"><div class="' +
      barClass +
      '"' +
      barStyle +
      "></div></div>" +
      '<p class="hsf-update-note hsf-update-warn">Do not power off the device.</p>'
    );
  }

  function renderError(status) {
    return (
      '<div class="hsf-update-head">' +
      '  <span class="hsf-update-title">Update failed</span>' +
      '  <button type="button" class="hsf-update-close" data-action="close" aria-label="Dismiss">&times;</button>' +
      "</div>" +
      '<div class="hsf-update-current">' +
      escapeHtml(status.error || "Unknown error") +
      "</div>" +
      '<div class="hsf-update-actions">' +
      '  <button type="button" class="btn btn-sm btn-outline-secondary" data-action="retry">Try again</button>' +
      "</div>"
    );
  }

  function render(status) {
    lastStatus = status;
    var node = ensureMount();

    if (!status || !status.enabled) {
      hide();
      return;
    }

    var state = status.state;

    if (state === "downloading" || state === "verifying" || state === "installing" || state === "ready") {
      node.className = "hsf-update-toast busy";
      node.innerHTML = renderProgress(status);
      node.hidden = false;
      return;
    }

    /* An error is only worth a popup if it came from something the operator
       just asked for. A failed background check is a log line, not an
       interruption -- the Configuration page shows it. */
    if (state === "error" && awaitingResult) {
      node.className = "hsf-update-toast error";
      node.innerHTML = renderError(status);
      node.hidden = false;
      return;
    }

    /* Anything else means the request resolved one way or the other. */
    awaitingResult = false;

    if (state === "available" && status.available) {
      if (!status.mandatory && (status.dismissed || locallyDismissed(status.version))) {
        hide();
        return;
      }
      node.className = "hsf-update-toast" + (status.mandatory ? " mandatory" : "");
      node.innerHTML = renderOffer(status);
      node.hidden = false;
      return;
    }

    hide();
  }

  /* ---- actions ------------------------------------------------------ */

  function onClick(event) {
    var button = event.target.closest("[data-action]");
    if (!button || !mount || !mount.contains(button)) return;
    var action = button.getAttribute("data-action");
    var version = lastStatus ? lastStatus.version : "";

    if (action === "close") {
      awaitingResult = false;
      hide();
      return;
    }

    if (action === "later") {
      awaitingResult = false;
      rememberDismissed(version);
      hide();
      postJson("/api/update/dismiss", { version: version }).catch(function () {
        /* Hidden locally regardless; the gateway will offer it again next boot. */
      });
      return;
    }

    if (action === "update" || action === "retry") {
      awaitingResult = true;
      button.disabled = true;
      postJson("/api/update/install", { version: version })
        .then(function (result) {
          if (!result.ok) {
            // A refusal the gateway answered synchronously ("no manifest for
            // that version", "already in progress"). Shown in the same place
            // the progress would have been, since that is where the operator
            // is looking.
            render(Object.assign({}, lastStatus, { state: "error", error: result.data.error }));
            return;
          }
          render(result.data.status);
        })
        .catch(function () {
          render(Object.assign({}, lastStatus, { state: "error", error: "Could not reach the gateway." }));
        });
    }
  }

  /* ---- wiring -------------------------------------------------------- */

  function start() {
    ensureMount();
    document.addEventListener("click", onClick);

    fetch("/api/update/status")
      .then(function (r) {
        return r.json();
      })
      .then(render)
      .catch(function () {
        /* No update endpoint (older gateway, or the page is served by
           something else): stay silent rather than showing an error nobody
           asked for. */
      });

    if (window.HsfWs) {
      window.HsfWs.onMessage(function (payload) {
        if (payload && payload.type === "update") render(payload);
      });
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start);
  } else {
    start();
  }

  window.HsfUpdate = { refresh: function () { return fetch("/api/update/status").then(function (r) { return r.json(); }).then(render); } };
})();
