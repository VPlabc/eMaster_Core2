/* Production packaging panel on the Lua Editor (request/AdvanceUpdate.md
 * sections 2.3, 2.5, 2.10, 2.11).
 *
 * Compile / Build & Test / Deploy / Roll back. The backend enforces
 * LUA_COMPILE, LUA_DEPLOY and LUA_ROLLBACK separately, so a button here being
 * clickable is not permission to use it -- the panel hides what the session
 * cannot do purely so nobody clicks their way into a 403.
 */
(function () {
  "use strict";

  var card = document.getElementById("packagingCard");
  if (!card) return;

  var STAGES = ["compile", "sign", "verify", "load", "write"];
  var STAGE_LABELS = {
    compile: "Compile to bytecode",
    sign: "Encrypt + sign",
    verify: "Verify signature + decrypt",
    load: "Load with this interpreter",
    write: "Write artifact"
  };

  function el(id) {
    return document.getElementById(id);
  }

  function setResult(message, ok) {
    var box = el("pkgResult");
    box.textContent = message;
    box.className = "small mt-2 " + (ok ? "text-success" : "text-danger");
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

  function postJson(url, body) {
    return apiJson(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {})
    });
  }

  /* Renders the pipeline as a checklist rather than one pass/fail line. The
     stage that failed is the single most useful piece of information -- a
     syntax error and a runtime mismatch need completely different responses,
     and "build failed" tells you neither. */
  function renderStages(report) {
    var box = el("pkgStages");
    box.textContent = "";
    var passed = report.stages_passed || [];
    var list = document.createElement("div");

    STAGES.forEach(function (stage) {
      var done = passed.indexOf(stage) !== -1;
      var failed = report.failed_stage === stage;
      if (!done && !failed && !report.ok && passed.length === 0 && stage !== STAGES[0]) return;

      var row = document.createElement("div");
      row.className = done ? "text-success" : failed ? "text-danger" : "text-muted";
      var mark = done ? "✓ " : failed ? "✗ " : "· ";
      row.textContent = mark + STAGE_LABELS[stage];
      list.appendChild(row);
    });
    box.appendChild(list);

    if (report.ok) {
      var summary = document.createElement("div");
      summary.className = "text-muted mt-2";
      summary.textContent =
        report.modules_compiled + " modules · " + report.source_bytes + " B source → " +
        report.bytecode_bytes + " B bytecode → " + report.package_bytes + " B package";
      box.appendChild(summary);
    }
  }

  function describeFailure(report) {
    var message = report.error || "build failed";
    if (report.error_module) message = report.error_module + ": " + message;
    if (report.error_line) message += " (line " + report.error_line + ")";
    return message;
  }

  function build(write) {
    var body = {
      app_id: el("pkgAppId").value.trim(),
      version: el("pkgVersion").value.trim(),
      entry: el("pkgEntry").value.trim() || "main.lua",
      source_dir: el("pkgSourceDir").value.trim(),
      notes: el("pkgNotes").value.trim()
    };
    if (!body.app_id || !body.version) {
      return setResult("Application id and version are required.", false);
    }

    setResult(write ? "Building and testing..." : "Compiling...", true);
    el("pkgStages").textContent = "";

    postJson(write ? "/api/lua/packages/build" : "/api/lua/compile", body).then(function (result) {
      var report = result.data || {};
      renderStages(report);
      if (!result.ok || !report.ok) {
        return setResult(describeFailure(report), false);
      }
      setResult(
        write ? "Built " + report.file + " — ready to deploy." : "Compiles cleanly. Nothing was written.",
        true
      );
      if (write) refresh();
    });
  }

  el("btnPkgCompile").addEventListener("click", function () {
    build(false);
  });
  el("btnPkgBuild").addEventListener("click", function () {
    build(true);
  });

  el("btnPkgStop").addEventListener("click", function () {
    window.HsfConfirm("Stop the active Lua package? It will remain selected but will not run until restarted.", "Stop Lua package")
      .then(function (confirmed) {
        if (!confirmed) return null;
        setResult("Stopping active package...", true);
        return postJson("/api/lua/packages/stop", {});
      }).then(function (result) {
      if (!result) return;
      if (!result.ok) return setResult(result.data.error || "Stop failed.", false);
      setResult(result.data.stopped ? "Active package stopped." : "Active package was already stopped.", true);
      refresh();
    });
  });

  el("btnPkgRollback").addEventListener("click", function () {
    window.HsfConfirm("Roll the Lua application back to the previous package and restart it?", "Rollback Lua package")
      .then(function (confirmed) {
        if (!confirmed) return null;
        setResult("Rolling back...", true);
        return postJson("/api/lua/packages/rollback", {});
      }).then(function (result) {
      if (!result) return;
      if (!result.ok) return setResult(result.data.error || "Rollback failed.", false);
      /* A rollback that repointed but could not start is NOT a success worth a
         green message: the gateway is now running nothing. */
      if (result.data.start_error) {
        setResult("Rolled back to " + result.data.active + ", but it did not start: " + result.data.start_error, false);
      } else {
        setResult("Rolled back to " + result.data.active + " and restarted it.", true);
      }
      refresh();
    });
  });

  function cell(row, text, className) {
    var td = document.createElement("td");
    td.textContent = text == null ? "" : String(text);
    if (className) td.className = className;
    row.appendChild(td);
    return td;
  }

  function deploy(file) {
    window.HsfConfirm("Deploy " + file + "? The running Lua application is replaced immediately.", "Deploy Lua package")
      .then(function (confirmed) {
        if (!confirmed) return null;
        setResult("Deploying " + file + "...", true);
        return postJson("/api/lua/packages/deploy", { file: file });
      }).then(function (result) {
      if (!result) return;
      if (!result.ok) return setResult(result.data.error || "Deploy failed.", false);
      if (result.data.start_error) {
        setResult("Deployed " + file + " but it did not start: " + result.data.start_error, false);
      } else {
        setResult("Deployed " + file + " and restarted the application.", true);
      }
      refresh();
    });
  }

  function setPackageRunning(file, running) {
    var verb = running ? "Start" : "Stop";
    window.HsfConfirm(verb + " " + file + "? This choice is saved for the next gateway startup.",
                      verb + " Lua package")
      .then(function (confirmed) {
        if (!confirmed) return null;
        setResult(verb + "ing " + file + "...", true);
        return postJson("/api/lua/packages/" + (running ? "start" : "stop"), { file: file });
      }).then(function (result) {
        if (!result) return;
        if (!result.ok) return setResult(result.data.error || (verb + " failed."), false);
        setResult(file + (running ? " is running and will start after reboot."
                                  : " is stopped and will stay stopped after reboot."), true);
        refresh();
      });
  }

  document.getElementById("btnPkgImport").addEventListener("click", function () {
    var file = document.getElementById("pkgImportFile").files[0];
    if (!file) { setResult("Choose a .pkg file first.", false); return; }
    var form = new FormData();
    form.append("filename", file.name);
    form.append("package", file);
    setResult("Importing " + file.name + "...", true);
    fetch("/api/lua/packages/import", { method: "POST", body: form })
      .then(function (r) { return r.json(); })
      .then(function (result) { setResult(result.error || "Imported " + file.name + ".", !result.error); refresh(); })
      .catch(function (error) { setResult(String(error), false); });
  });

  document.getElementById("btnPkgImportPath").addEventListener("click", function () {
    var path = document.getElementById("pkgImportPath").value.trim();
    if (!path) { setResult("Enter the package path on the gateway host first.", false); return; }
    setResult("Importing the gateway-host package...", true);
    fetch("/api/lua/packages/import-path", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: path })
    }).then(function (r) { return r.json().then(function (data) { return { ok: r.ok, data: data }; }); })
      .then(function (result) {
        if (!result.ok) { setResult(result.data.error || "Import failed.", false); return; }
        setResult("Imported " + result.data.file + " from the gateway host.", true);
        refresh();
      }).catch(function (error) { setResult(String(error), false); });
  });

  function render(status) {
    var body = el("pkgTableBody");
    body.textContent = "";

    el("pkgRuntimeTag").textContent =
      "Interpreter " + (status.runtime_tag || "unknown") +
      (status.signing_key ? " · signing key " + status.signing_key : "");

    var warning = el("packagingKeyWarning");
    if (!status.can_build) {
      warning.textContent =
        "This gateway has no Lua signing key, so it can verify and run packages but cannot build them. " +
        "Build them on the machine that holds the key, or create one (see docs/lua-packaging.md).";
      warning.classList.remove("d-none");
      el("btnPkgBuild").disabled = true;
    } else {
      warning.classList.add("d-none");
      el("btnPkgBuild").disabled = false;
    }

    el("btnPkgRollback").disabled = !status.previous;
    el("btnPkgStop").disabled = !status.active;

    var packages = status.packages || [];
    if (!packages.length) {
      var empty = document.createElement("tr");
      cell(empty, "No packages built yet", "text-muted small").colSpan = 6;
      body.appendChild(empty);
      return;
    }

    packages.forEach(function (pkg) {
      var row = document.createElement("tr");
      var versionCell = cell(row, pkg.version || "?");
      if (pkg.active) {
        var live = document.createElement("span");
        live.className = "badge bg-success ms-2";
        live.textContent = "deployed";
        versionCell.appendChild(live);
      } else if (pkg.previous) {
        var prev = document.createElement("span");
        prev.className = "badge bg-secondary ms-2";
        prev.textContent = "rollback target";
        versionCell.appendChild(prev);
      }
      if (pkg.running) {
        var running = document.createElement("span");
        running.className = "badge bg-primary ms-2";
        running.textContent = "running";
        versionCell.appendChild(running);
      } else if (pkg.run_on_startup) {
        var startup = document.createElement("span");
        startup.className = "badge bg-warning text-dark ms-2";
        startup.textContent = "start pending";
        versionCell.appendChild(startup);
      }
      cell(row, pkg.app_id, "small");
      cell(row, pkg.built_at, "small text-nowrap");
      cell(row, pkg.built_by, "small");
      cell(row, Math.round((pkg.size_bytes || 0) / 1024) + " KB", "small");

      var actions = document.createElement("td");
      actions.className = "text-end";
      if (!pkg.active) {
        var deployButton = document.createElement("button");
        deployButton.type = "button";
        deployButton.className = "btn btn-sm btn-outline-primary me-1";
        deployButton.textContent = "Deploy";
        deployButton.addEventListener("click", function () {
          deploy(pkg.file);
        });
        actions.appendChild(deployButton);
      }
      var runButton = document.createElement("button");
      runButton.type = "button";
      runButton.className = "btn btn-sm " + (pkg.running ? "btn-outline-danger" : "btn-outline-success") + " me-1";
      runButton.textContent = pkg.running ? "Stop" : "Run";
      runButton.addEventListener("click", function () { setPackageRunning(pkg.file, !pkg.running); });
      actions.appendChild(runButton);
      var removeButton = document.createElement("button");
      removeButton.type = "button";
      removeButton.className = "btn btn-sm btn-outline-danger";
      removeButton.textContent = "Delete";
      /* The deployed package and the rollback target are not deletable -- the
         backend refuses both, and disabling them here says why before the
         click rather than after. */
      removeButton.disabled = pkg.active || pkg.previous || pkg.run_on_startup || pkg.running;
      if (removeButton.disabled) removeButton.title = "Deployed packages and the rollback target are kept.";
      removeButton.addEventListener("click", function () {
        window.HsfConfirm("Delete " + pkg.file + "?", "Delete Lua package").then(function (confirmed) {
          if (!confirmed) return null;
          return postJson("/api/lua/packages/delete", { file: pkg.file });
        }).then(function (result) {
          if (!result) return;
          if (!result.ok) return setResult(result.data.error || "Delete failed.", false);
          refresh();
        });
      });
      actions.appendChild(removeButton);
      row.appendChild(actions);
      body.appendChild(row);
    });
  }

  function refresh() {
    apiJson("/api/lua/packages").then(function (result) {
      if (!result.ok) {
        var body = el("pkgTableBody");
        body.textContent = "";
        var row = document.createElement("tr");
        cell(row, result.data.error || "Cannot read packages", "text-danger small").colSpan = 6;
        body.appendChild(row);
        return;
      }
      render(result.data);
    });
  }

  refresh();
})();
