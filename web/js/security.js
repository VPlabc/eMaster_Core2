/* Security page: accounts, own password, and the audit trail.
 *
 * Every call here goes through the fetch wrapper auth.js installs, so the
 * bearer token is attached automatically and a 401 bounces to the login page.
 * Nothing on this page is a security control -- the gateway enforces
 * USER_READ / USER_WRITE on the endpoints regardless of what is rendered.
 */
(function () {
  "use strict";

  var alertBox = document.getElementById("securityAlert");

  function notify(message, ok) {
    alertBox.textContent = message;
    alertBox.className = "alert " + (ok ? "alert-success" : "alert-danger");
    if (window.HsfNotify) window.HsfNotify(message, ok);
    if (ok) {
      window.setTimeout(function () {
        alertBox.className = "alert d-none";
      }, 4000);
    }
  }

  function setResult(id, message, ok) {
    var el = document.getElementById(id);
    el.textContent = message;
    el.className = "small mt-2 " + (ok ? "text-success" : "text-danger");
  }

  /* textContent everywhere below, never innerHTML with server data. A username
     is chosen by an admin and an audit `reason` can contain a path someone
     else supplied -- both are strings from outside this page. */
  function cell(row, text, className) {
    var td = document.createElement("td");
    td.textContent = text == null ? "" : String(text);
    if (className) td.className = className;
    row.appendChild(td);
    return td;
  }

  function formatTime(unixSeconds) {
    if (!unixSeconds) return "never";
    return new Date(unixSeconds * 1000).toLocaleString();
  }

  function apiJson(url, options) {
    return fetch(url, options).then(function (response) {
      return response
        .json()
        .catch(function () {
          return {};
        })
        .then(function (data) {
          return { ok: response.ok, status: response.status, data: data };
        });
    });
  }

  function describeError(result) {
    var data = result.data || {};
    if (data.fields) {
      return Object.keys(data.fields)
        .map(function (k) {
          return k + " " + data.fields[k];
        })
        .join("; ");
    }
    return data.error || "Request failed (HTTP " + result.status + ")";
  }

  /* ---- accounts ------------------------------------------------------ */

  var me = null;

  function renderUsers(users) {
    var body = document.getElementById("userTableBody");
    body.textContent = "";
    if (!users.length) {
      var empty = document.createElement("tr");
      cell(empty, "No accounts", "text-muted small").colSpan = 5;
      body.appendChild(empty);
      return;
    }

    users.forEach(function (user) {
      var row = document.createElement("tr");
      var nameCell = cell(row, user.username);
      if (me && user.username === me.username) {
        var you = document.createElement("span");
        you.className = "badge bg-secondary ms-2";
        you.textContent = "you";
        nameCell.appendChild(you);
      }
      cell(row, user.role);
      cell(row, user.enabled ? "Enabled" : "Disabled", user.enabled ? "text-success" : "text-danger");
      cell(row, formatTime(user.last_login), "small");

      var actions = document.createElement("td");
      actions.className = "text-end";

      var toggle = document.createElement("button");
      toggle.type = "button";
      toggle.className = "btn btn-sm btn-outline-secondary me-1";
      toggle.textContent = user.enabled ? "Disable" : "Enable";
      toggle.addEventListener("click", function () {
        patchUser(user.id, { enabled: !user.enabled });
      });

      var reset = document.createElement("button");
      reset.type = "button";
      reset.className = "btn btn-sm btn-outline-secondary me-1";
      reset.textContent = "Reset password";
      /* Not on your own row. This path is "an admin resets someone else's
         password", so it deliberately does not ask for the old one -- and
         pointing it at yourself was a way to change your own password without
         proving you knew the current one, which is precisely what the Change
         My Password card below exists to require. Two doors into the same
         room, one of them unlocked. */
      var isSelf = !!(me && user.username === me.username);
      reset.disabled = isSelf;
      if (isSelf) reset.title = "Use “Change My Password” below — it verifies your current password.";
      reset.addEventListener("click", function () {
        var generated = randomPassword();
        // window.prompt is the only way to hand a generated password to a
        // human without putting it in the DOM where it lingers in the page
        // (and in a screenshot). Selectable, copyable, gone when dismissed.
        var chosen = window.prompt(
          "New password for " + user.username + ".\nA strong one has been generated; copy it now.",
          generated
        );
        if (chosen) patchUser(user.id, { password: chosen });
      });

      var remove = document.createElement("button");
      remove.type = "button";
      remove.className = "btn btn-sm btn-outline-danger";
      remove.textContent = "Delete";
      remove.disabled = isSelf;
      remove.addEventListener("click", function () {
        window.HsfConfirm("Delete " + user.username + "? Their sessions end immediately.", "Delete account").then(function (confirmed) {
          if (!confirmed) return null;
          return apiJson("/api/users/" + user.id, { method: "DELETE" });
        }).then(function (result) {
          if (!result) return;
          if (!result.ok) return notify(describeError(result), false);
          notify("Deleted " + user.username, true);
          loadUsers();
        });
      });

      actions.appendChild(toggle);
      actions.appendChild(reset);
      actions.appendChild(remove);
      row.appendChild(actions);
      body.appendChild(row);
    });
  }

  function patchUser(id, changes) {
    apiJson("/api/users/" + id, {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(changes)
    }).then(function (result) {
      if (!result.ok) return notify(describeError(result), false);
      notify("Account updated.", true);
      loadUsers();
      loadAudit();
    });
  }

  function loadUsers() {
    apiJson("/api/users").then(function (result) {
      if (!result.ok) return notify(describeError(result), false);
      renderUsers(result.data.users || []);
    });
  }

  /* Matches the alphabet the gateway uses for its seeded password: no
     look-alike characters, because these get read aloud and typed. */
  function randomPassword() {
    var alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    var bytes = new Uint32Array(20);
    window.crypto.getRandomValues(bytes);
    var out = "";
    for (var i = 0; i < bytes.length; i++) out += alphabet.charAt(bytes[i] % alphabet.length);
    return out;
  }

  document.getElementById("btnGeneratePassword").addEventListener("click", function () {
    var field = document.getElementById("newPassword");
    var confirmField = document.getElementById("confirmNewPassword");
    var generated = randomPassword();
    /* Both fields, and both revealed: a generated password is meant to be read
       off the screen and copied, so masking it would only invite retyping it
       by hand -- which is the mistake the confirm field is there to catch. */
    field.type = "text";
    field.value = generated;
    confirmField.type = "text";
    confirmField.value = generated;
    setResult("createUserResult", "Copy this password now -- it is not shown again after the account is created.", true);
  });

  document.getElementById("btnCreateUser").addEventListener("click", function () {
    var usernameField = document.getElementById("newUsername");
    var passwordField = document.getElementById("newPassword");
    var confirmField = document.getElementById("confirmNewPassword");
    var username = usernameField.value.trim();
    var password = passwordField.value;
    var confirm = confirmField.value;
    var role = document.getElementById("newRole").value;

    if (username.length < 3) {
      usernameField.focus();
      return setResult("createUserResult", "Username must be at least 3 characters.", false);
    }
    if (password.length < 12) {
      passwordField.focus();
      return setResult("createUserResult", "Password must be at least 12 characters.", false);
    }
    if (password !== confirm) {
      confirmField.focus();
      confirmField.select();
      return setResult("createUserResult", "The two passwords do not match.", false);
    }

    apiJson("/api/users", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: username, password: password, role: role })
    }).then(function (result) {
      if (!result.ok) return setResult("createUserResult", describeError(result), false);
      setResult("createUserResult", "Created " + username + " as " + role + ".", true);
      usernameField.value = "";
      passwordField.value = "";
      passwordField.type = "password";
      confirmField.value = "";
      confirmField.type = "password";
      loadUsers();
      loadAudit();
    });
  });

  /* ---- own password --------------------------------------------------- */

  document.getElementById("btnChangePassword").addEventListener("click", function () {
    var currentField = document.getElementById("currentPassword");
    var nextField = document.getElementById("nextPassword");
    var confirmField = document.getElementById("confirmPassword");
    var current = currentField.value;
    var next = nextField.value;
    var confirm = confirmField.value;

    /* Checked here as well as on the gateway. The backend is the boundary --
       it rejects a wrong or missing current password with 401/400 either way
       -- but bouncing it locally gives a sentence the user can act on instead
       of a field-name error, and does not burn a lockout attempt against
       their own account on an obvious typo. */
    if (!current) {
      currentField.focus();
      return setResult("passwordResult", "Enter your current password.", false);
    }
    if (next.length < 12) {
      nextField.focus();
      return setResult("passwordResult", "New password must be at least 12 characters.", false);
    }
    /* The confirm field is the whole reason this cannot be one input: the
       change signs you out of every session immediately, so a typo in a
       password nobody has read back would lock you out of the gateway with no
       way to correct it. */
    if (next !== confirm) {
      confirmField.focus();
      confirmField.select();
      return setResult("passwordResult", "The two new passwords do not match.", false);
    }
    if (next === current) {
      return setResult("passwordResult", "The new password must differ from the current one.", false);
    }

    apiJson("/api/auth/password", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ current_password: current, new_password: next })
    }).then(function (result) {
      if (!result.ok) {
        /* 401 here means exactly one thing -- the current password was wrong.
           Say that, rather than the generic "invalid credentials" the gateway
           returns to keep login answers uniform. */
        var message = result.status === 401 ? "Current password is incorrect." : describeError(result);
        currentField.value = "";
        currentField.focus();
        return setResult("passwordResult", message, false);
      }
      currentField.value = "";
      nextField.value = "";
      confirmField.value = "";
      setResult("passwordResult", "Password changed. Signing you out...", true);
      window.setTimeout(function () {
        window.HsfAuth.logout();
      }, 1500);
    });
  });

  /* ---- audit ------------------------------------------------------------ */

  function loadAudit() {
    var event = document.getElementById("auditEvent").value;
    var user = document.getElementById("auditUser").value.trim();
    var query = "/api/audit?limit=200";
    if (event) query += "&event=" + encodeURIComponent(event);
    if (user) query += "&user=" + encodeURIComponent(user);

    apiJson(query).then(function (result) {
      var body = document.getElementById("auditTableBody");
      body.textContent = "";
      if (!result.ok) {
        var errorRow = document.createElement("tr");
        cell(errorRow, describeError(result), "text-danger small").colSpan = 7;
        body.appendChild(errorRow);
        return;
      }
      var entries = result.data.entries || [];
      document.getElementById("auditCount").textContent =
        entries.length + " shown of " + (result.data.total || 0) + " total";

      if (!entries.length) {
        var empty = document.createElement("tr");
        cell(empty, "No matching events", "text-muted small").colSpan = 7;
        body.appendChild(empty);
        return;
      }
      entries.forEach(function (entry) {
        var row = document.createElement("tr");
        cell(row, formatTime(entry.timestamp), "small text-nowrap");
        cell(row, entry.event, "small");
        cell(row, entry.username, "small");
        cell(row, entry.source_ip, "small");
        cell(row, entry.resource, "small");
        cell(row, entry.result, "small " + (entry.result === "DENIED" ? "text-danger" : ""));
        cell(row, entry.reason, "small text-muted");
        body.appendChild(row);
      });
    });
  }

  document.getElementById("btnRefreshAudit").addEventListener("click", loadAudit);
  document.getElementById("auditEvent").addEventListener("change", loadAudit);

  /* ---- start ------------------------------------------------------------- */

  apiJson("/api/auth/me").then(function (result) {
    if (result.ok) me = result.data;
    loadUsers();
    loadAudit();
  });
})();
