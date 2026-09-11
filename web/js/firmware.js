(function () {
  "use strict";
  var statusBox = document.getElementById("firmwareStatus");
  var install = document.getElementById("firmwareInstall");
  var offeredVersion = "";

  function json(url, options) {
    return fetch(url, options || {}).then(function (r) {
      return r.json().catch(function () { return {}; }).then(function (data) {
        if (!r.ok) throw new Error(data.error || data.message || "request failed");
        return data;
      });
    });
  }
  function post(url, body) {
    return json(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  }
  function renderUpdate(data) {
    offeredVersion = data.version || data.available_version || (data.available && data.available.version) || "";
    install.disabled = !offeredVersion || data.state !== "available";
    var text = "State: " + (data.state || "unknown");
    if (data.current_version) text += " · running " + data.current_version;
    if (offeredVersion) text += " · offered " + offeredVersion;
    if (data.error) text += " · " + data.error;
    statusBox.textContent = text;
    statusBox.className = "hsf-inline-status py-2 small " + (data.state === "error" ? "hsf-inline-status-warning" : data.state === "available" ? "hsf-inline-status-info" : "hsf-inline-status-neutral");
  }
  function refresh() {
    json("/api/update/status").then(renderUpdate).catch(function (e) { statusBox.textContent = e.message; statusBox.className = "hsf-inline-status hsf-inline-status-warning py-2 small"; });
    json("/api/lua/packages").then(function (data) {
      var body = document.getElementById("firmwarePackages"); body.textContent = "";
      (data.packages || []).forEach(function (pkg) {
        var row = document.createElement("tr");
        var state = pkg.running ? "running · startup enabled" :
          (pkg.run_on_startup ? "start pending" : (pkg.active ? "deployed · stopped" : "stored · stopped"));
        [pkg.app_id || "", pkg.version || "", pkg.file || "", state].forEach(function (v) { var c = document.createElement("td"); c.textContent = v; row.appendChild(c); });
        var actions = document.createElement("td");
        var button = document.createElement("button");
        button.type = "button";
        button.className = "btn btn-sm " + (pkg.running ? "btn-outline-danger" : "btn-outline-success");
        button.textContent = pkg.running ? "Stop" : "Run";
        button.addEventListener("click", function () {
          button.disabled = true;
          var action = pkg.running ? "stop" : "start";
          post("/api/lua/packages/" + action, { file: pkg.file }).then(function () {
            document.getElementById("packageResult").textContent = (action === "start" ? "Running " : "Stopped ") + pkg.file + "; startup state saved.";
            refresh();
          }).catch(function (e) {
            document.getElementById("packageResult").textContent = e.message;
            button.disabled = false;
          });
        });
        actions.appendChild(button);
        row.appendChild(actions);
        body.appendChild(row);
      });
      if (!body.children.length) { var row = document.createElement("tr"); var c = document.createElement("td"); c.colSpan = 5; c.className = "text-muted"; c.textContent = "No packages imported yet."; row.appendChild(c); body.appendChild(row); }
    });
  }
  document.getElementById("firmwareCheck").addEventListener("click", function () { statusBox.textContent = "Checking release server..."; post("/api/update/check", {}).then(function (d) { renderUpdate(d.status || d); }).catch(function (e) { statusBox.textContent = e.message; }); });
  install.addEventListener("click", function () { if (!offeredVersion) return; if (!window.HsfConfirm) return; window.HsfConfirm("Install " + offeredVersion + " and restart the gateway?", "Install firmware").then(function (yes) { if (!yes) return; return post("/api/update/install", { version: offeredVersion }); }).then(function (d) { if (d) renderUpdate(d.status || d); }).catch(function (e) { statusBox.textContent = e.message; }); });
  document.getElementById("firmwareUploadPackage").addEventListener("click", function () {
    var file = document.getElementById("firmwarePackageFile").files[0]; if (!file) return;
    var form = new FormData(); form.append("filename", file.name); form.append("package", file);
    fetch("/api/lua/packages/import", { method: "POST", body: form }).then(function (r) { return r.json(); }).then(function (d) { document.getElementById("packageResult").textContent = d.error || "Uploaded " + file.name + " to Packages."; refresh(); });
  });
  document.getElementById("firmwareImportPath").addEventListener("click", function () { var path = document.getElementById("firmwarePackagePath").value.trim(); if (!path) return; post("/api/lua/packages/import-path", { path: path }).then(function (d) { document.getElementById("packageResult").textContent = "Copied " + d.file + " to Packages."; refresh(); }).catch(function (e) { document.getElementById("packageResult").textContent = e.message; }); });
  document.getElementById("firmwareRefresh").addEventListener("click", refresh);
  refresh();
})();
