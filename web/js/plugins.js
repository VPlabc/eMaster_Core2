(function () {
  "use strict";
  var body = document.querySelector("#pluginsTable tbody");
  var message = document.getElementById("pluginMessage");
  var source = document.getElementById("pluginSource");

  function show(value) { message.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2); }
  function request(url, options) {
    return fetch(url, options || {}).then(function (r) {
      return r.json().then(function (data) { if (!r.ok) throw new Error(data.error || "request failed"); return data; });
    });
  }
  function post(url, payload) {
    return request(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload || {}) });
  }
  function action(id, name) {
    return post("/api/plugins/" + encodeURIComponent(id) + "/" + name);
  }
  function load() {
    request("/api/plugins").then(function (data) {
      body.innerHTML = "";
      (data.plugins || []).forEach(function (plugin) {
        var row = document.createElement("tr");
        [plugin.name || plugin.id, plugin.version || "",
         (plugin.state || "") + (plugin.enabled_on_startup ? " · startup enabled" : " · startup disabled"),
         plugin.has_driver ? "yes" : "no"]
          .forEach(function (value) { var cell = document.createElement("td"); cell.textContent = value; row.appendChild(cell); });
        var configCell = document.createElement("td");
        var configInput = document.createElement("input");
        configInput.className = "form-control form-control-sm mb-1";
        configInput.placeholder = '{"transport":{"type":"tcp","host":"192.168.1.10","port":502}}';
        configCell.appendChild(configInput);
        request("/api/plugins/" + encodeURIComponent(plugin.id) + "/config").then(function (result) {
          configInput.value = JSON.stringify(result.config || {});
        }).catch(function () {});
        var saveConfig = document.createElement("button");
        saveConfig.className = "btn btn-sm btn-outline-primary";
        saveConfig.textContent = "Save config";
        saveConfig.addEventListener("click", function () {
          var value;
          try { value = JSON.parse(configInput.value || "{}"); } catch (e) { show("Configuration must be valid JSON."); return; }
          request("/api/plugins/" + encodeURIComponent(plugin.id) + "/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(value) })
            .then(function (result) { show(result); }).catch(function (error) { show(error.message); });
        });
        configCell.appendChild(saveConfig);
        row.appendChild(configCell);
        var cell = document.createElement("td");
        [["Health", "test/health"], ["Scan", "test/discover"],
         [plugin.state === "RUNNING" ? "Disable" : "Enable", plugin.state === "RUNNING" ? "disable" : "enable"],
         ["Rollback", "rollback"], ["Delete", "delete"]].forEach(function (item) {
          var button = document.createElement("button");
          button.className = "btn btn-sm btn-outline-secondary me-1 mb-1";
          button.textContent = item[0];
          button.addEventListener("click", function () {
            var run = function () {
              var call = item[1] === "test/discover"
                ? request("/api/plugins/" + encodeURIComponent(plugin.id) + "/" + item[1])
                : action(plugin.id, item[1]);
              call.then(function (result) { show(result); load(); }).catch(function (error) {
                show(error.message);
                if (window.HsfNotify) window.HsfNotify(error.message, false);
              });
            };
            if (item[1] === "delete") {
              window.HsfConfirm("Delete " + plugin.id + "?", "Delete plugin").then(function (confirmed) {
                if (confirmed) run();
              });
            } else {
              run();
            }
          });
          cell.appendChild(button);
        });
        row.appendChild(cell);
        body.appendChild(row);
      });
    }).catch(function (error) { show(error.message); });
  }
  function install(url) {
    var path = source.value.trim();
    if (!path) { show("Package directory is required."); return; }
    post(url, { source_dir: path }).then(function (result) { show(result); load(); })
      .catch(function (error) { show(error.message); });
  }
  document.getElementById("importPlugin").addEventListener("click", function () {
    var manifest = document.getElementById("pluginManifest").files[0];
    var binary = document.getElementById("pluginBinary").files[0];
    if (!manifest || !binary) { show("Choose manifest.json and the plugin binary first."); return; }
    var form = new FormData();
    form.append("manifest", manifest);
    form.append("plugin", binary);
    request("/api/plugins/import", { method: "POST", body: form })
      .then(function (result) { show(result); load(); })
      .catch(function (error) { show(error.message); });
  });
  document.getElementById("refreshPlugins").addEventListener("click", load);
  document.getElementById("installPlugin").addEventListener("click", function () { install("/api/plugins/install"); });
  document.getElementById("updatePlugin").addEventListener("click", function () { install("/api/plugins/update"); });
  load();
})();
