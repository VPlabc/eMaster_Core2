(function () {
  const listEl = document.getElementById("scriptList");
  const newNameInput = document.getElementById("newScriptName");
  const currentLabel = document.getElementById("currentScriptLabel");
  const selectedCountEl = document.getElementById("selectedCount");
  let currentScriptName = null;
  let defaultScriptName = null;

  /* Ticked scripts, for "Run Selected" (upgrade.md sections 20 and 26). Kept
     outside the DOM so it survives refreshList() rebuilding the tree -- a
     selection that vanished every time a runtime state changed would be
     unusable, since the list refreshes on state changes. */
  const selected = new Set();
  // Latest state per script name from /api/lua/scripts, so a re-render keeps
  // the badges without waiting for another request.
  let scriptStates = {};

  function updateSelectedCount() {
    selectedCountEl.textContent = selected.size + " selected";
  }

  // The name goes in the query string, not the path: script names can now
  // contain '/' for subfolders, which a path segment can't carry.
  function apiPath(name, action) {
    return "/api/lua/scripts/" + (action || "file") + "?name=" + encodeURIComponent(name);
  }

  function postJson(url, body) {
    return fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {}),
    }).then((r) => r.json());
  }

  function setActiveHighlight(name) {
    listEl.querySelectorAll(".hsf-tree-file").forEach((el) => {
      el.classList.toggle("active", el.dataset.name === name);
    });
  }

  function setCurrentName(name) {
    currentScriptName = name;
    currentLabel.textContent = name || "(default startup script)";
    currentLabel.title = name || "";
    setActiveHighlight(name);
  }

  function refreshList() {
    return fetch("/api/lua/scripts")
      .then((r) => r.json())
      .then((data) => {
        defaultScriptName = data.default_name || null;
        scriptStates = {};
        (data.scripts || []).forEach((script) => {
          scriptStates[script.name] = { state: script.state || "STOPPED", error: script.last_error || "" };
        });
        renderTree(data.scripts || []);
      })
      .catch(() => window.HsfLuaEditor.log("Failed to load script list.", true));
  }

  /* Turns the flat list of relative paths the API returns
     ("card/issue.lua", "led.lua", ...) into a nested structure so folders
     can be rendered and collapsed (section 13). */
  function buildTree(scripts) {
    const root = { folders: new Map(), files: [] };
    scripts.forEach((script) => {
      const parts = script.name.split("/");
      let node = root;
      for (let i = 0; i < parts.length - 1; i++) {
        if (!node.folders.has(parts[i])) {
          node.folders.set(parts[i], { folders: new Map(), files: [] });
        }
        node = node.folders.get(parts[i]);
      }
      node.files.push({ label: parts[parts.length - 1], path: script.name, size: script.size });
    });
    return root;
  }

  function makeAction(label, title, handler) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "btn";
    btn.textContent = label;
    btn.title = title;
    btn.addEventListener("click", (event) => {
      // Without this the click also lands on the row and opens the file --
      // deleting would silently load it into the editor first.
      event.stopPropagation();
      handler();
    });
    return btn;
  }

  /* STOPPED gets no badge on purpose: with a dozen scripts listed, a column of
     grey "STOPPED" chips is noise that hides the two rows that matter. */
  function stateBadge(state) {
    if (!state || state === "STOPPED") return null;
    const badge = document.createElement("span");
    const cls =
      state === "RUNNING" ? "hsf-badge-ok" :
      state === "ERROR" ? "hsf-badge-bad" :
      "hsf-badge-warn";  // STARTING / STOPPING
    badge.className = "hsf-badge " + cls;
    badge.style.fontSize = "0.62rem";
    badge.textContent = state;
    return badge;
  }

  function renderFile(file) {
    const row = document.createElement("div");
    row.className = "hsf-tree-file";
    row.dataset.name = file.path;
    const info = scriptStates[file.path] || { state: "STOPPED", error: "" };
    row.title = file.path + " (" + file.size + " bytes) - " + info.state + (info.error ? "\n" + info.error : "");

    // Checkbox for the multi-select run/stop. Its own click must not also open
    // the file in the editor, which is what a click anywhere else on the row
    // does.
    const check = document.createElement("input");
    check.type = "checkbox";
    check.className = "form-check-input mt-0";
    check.style.flex = "0 0 auto";
    check.checked = selected.has(file.path);
    check.title = "Select for Run/Stop Selected";
    check.addEventListener("click", (event) => event.stopPropagation());
    check.addEventListener("change", () => {
      if (check.checked) {
        selected.add(file.path);
      } else {
        selected.delete(file.path);
      }
      updateSelectedCount();
    });
    row.appendChild(check);

    row.addEventListener("click", () => openScript(file.path));

    const name = document.createElement("span");
    name.className = "hsf-tree-name";
    name.textContent = file.label;
    row.appendChild(name);

    const badge = stateBadge(info.state);
    if (badge) row.appendChild(badge);

    if (file.path === defaultScriptName) {
      const star = document.createElement("span");
      star.className = "hsf-tree-star";
      star.textContent = "★";
      star.title = "Runs automatically on gateway startup";
      row.appendChild(star);
    }

    const actions = document.createElement("span");
    actions.className = "hsf-tree-actions";
    actions.appendChild(makeAction("▶", "Run this file from disk", () => runScripts([file.path])));
    // Per-row stop, since scripts are now independent -- stopping one from the
    // toolbar's Stop button would take everything else down with it.
    if (info.state === "RUNNING" || info.state === "STARTING") {
      actions.appendChild(makeAction("■", "Stop this script", () => stopScripts([file.path])));
    }
    actions.appendChild(
      makeAction(file.path === defaultScriptName ? "☆" : "★",
                 file.path === defaultScriptName ? "Stop running on startup" : "Run on startup",
                 () => (file.path === defaultScriptName ? clearDefaultScript(file.path) : setDefaultScript(file.path)))
    );
    actions.appendChild(makeAction("✎", "Rename or move", () => renameScript(file.path)));
    actions.appendChild(makeAction("✕", "Delete", () => deleteScript(file.path)));
    row.appendChild(actions);

    return row;
  }

  function renderNode(node, container) {
    // Folders first, then files, each alphabetically -- a stable order keeps
    // rows from jumping under the pointer when the list refreshes.
    [...node.folders.keys()].sort().forEach((folderName) => {
      const details = document.createElement("details");
      details.className = "hsf-tree-folder";
      details.open = true;

      const summary = document.createElement("summary");
      summary.textContent = folderName;
      const folderPath = node.path ? node.path + "/" + folderName : folderName;
      summary.appendChild(makeAction("✕", "Delete this folder and all Lua scripts in it", () => deleteFolder(folderPath)));
      details.appendChild(summary);

      const children = document.createElement("div");
      children.className = "hsf-tree-children";
      const childNode = node.folders.get(folderName);
      childNode.path = folderPath;
      renderNode(childNode, children);
      details.appendChild(children);

      container.appendChild(details);
    });

    node.files
      .slice()
      .sort((a, b) => a.label.localeCompare(b.label))
      .forEach((file) => container.appendChild(renderFile(file)));
  }

  function renderTree(scripts) {
    listEl.innerHTML = "";
    if (!scripts.length) {
      const empty = document.createElement("div");
      empty.className = "hsf-io-empty";
      empty.textContent = "No scripts yet.";
      listEl.appendChild(empty);
      return;
    }
    renderNode(buildTree(scripts), listEl);
    setActiveHighlight(currentScriptName);
  }

  function deleteFolder(name) {
    window.HsfConfirm("Delete folder " + name + " and all Lua scripts inside it? This cannot be undone.", "Delete Lua folder")
      .then(function (confirmed) {
        if (!confirmed) return;
        return postJson("/api/lua/scripts/folder/delete", { name: name });
      })
      .then((res) => {
        if (!res) return;
        if (!res.ok) {
          window.HsfLuaEditor.log("Failed to delete folder: " + (res.error || "unknown error"), true);
          return;
        }
        if (currentScriptName && currentScriptName.indexOf(name + "/") === 0) setCurrentName(null);
        window.HsfLuaEditor.log("Deleted folder " + name + ".");
        refreshList();
      })
      .catch(() => window.HsfLuaEditor.log("Folder delete request failed.", true));
  }

  function openScript(name) {
    fetch(apiPath(name))
      .then((r) => r.json())
      .then((data) => {
        setCurrentName(name);
        window.HsfLuaEditor.setCode(data.code || "");
        window.HsfLuaEditor.log("Opened " + name + ".");
      })
      .catch(() => window.HsfLuaEditor.log("Failed to open " + name + ".", true));
  }

  /* Runs one or many. Each name gets its own result, because one script failing
     to compile must not be reported as "the run failed" when three others
     started fine (upgrade.md section 22's isolation, surfaced in the UI). */
  function runScripts(names) {
    if (!names.length) {
      window.HsfLuaEditor.log("Tick at least one script first.", true);
      return;
    }

    postJson("/api/lua/scripts/run", { names: names })
      .then((res) => {
        (res.results || []).forEach((result) => {
          if (result.already_running) {
            // Section 27: this is the "Already Running" message, not a failure.
            window.HsfLuaEditor.log(result.name + ": already running.");
          } else if (result.ok) {
            window.HsfLuaEditor.log("Started " + result.name + ".");
          } else {
            window.HsfLuaEditor.log("Failed to start " + result.name + ": " + (result.error || "unknown error"), true);
          }
        });
        refreshList();
      })
      .catch(() => window.HsfLuaEditor.log("Run request failed.", true));
  }

  function stopScripts(names) {
    if (!names.length) {
      window.HsfLuaEditor.log("Tick at least one script first.", true);
      return;
    }

    postJson("/api/lua/scripts/stop", { names: names })
      .then((res) => {
        (res.results || []).forEach((result) => {
          window.HsfLuaEditor.log(
            result.ok ? "Stopped " + result.name + "." : result.name + " was not running.",
            false
          );
        });
        refreshList();
      })
      .catch(() => window.HsfLuaEditor.log("Stop request failed.", true));
  }

  function setDefaultScript(name) {
    fetch(apiPath(name, "set-default"), { method: "POST" })
      .then((r) => r.json())
      .then(() => {
        window.HsfLuaEditor.log(name + " will now run automatically on gateway startup.");
        refreshList();
      })
      .catch(() => {
        window.HsfLuaEditor.log("Failed to set " + name + " as the startup script.", true);
        refreshList();
      });
  }

  function clearDefaultScript(name) {
    fetch("/api/lua/clear-default", { method: "POST" })
      .then((r) => r.json())
      .then(() => {
        window.HsfLuaEditor.log(name + " will no longer run automatically on startup.");
        refreshList();
      })
      .catch(() => {
        window.HsfLuaEditor.log("Failed to clear the startup script.", true);
        refreshList();
      });
  }

  function renameScript(name) {
    let next = window.prompt("Rename or move (use / for folders):", name);
    if (next === null) return;
    next = next.trim();
    if (!next || next === name) return;
    if (!next.endsWith(".lua")) next += ".lua";

    postJson("/api/lua/scripts/rename", { from: name, to: next })
      .then((res) => {
        if (!res.ok) {
          window.HsfLuaEditor.log("Rename failed: " + (res.error || "unknown error"), true);
          return;
        }
        window.HsfLuaEditor.log("Renamed " + name + " to " + next + ".");
        // Follow the file, otherwise the next Save would write the old path
        // back and recreate the file under its previous name.
        if (currentScriptName === name) setCurrentName(next);
        refreshList();
      })
      .catch(() => window.HsfLuaEditor.log("Rename request failed.", true));
  }

  function deleteScript(name) {
    window.HsfConfirm("Delete " + name + "? This cannot be undone.", "Delete Lua script")
      .then(function (confirmed) {
        if (!confirmed) return null;
        return fetch(apiPath(name), { method: "DELETE" });
      })
      .then(function (response) {
        if (!response) return null;
        return response.json();
      })
      .then((r) => {
        if (!r) return;
        if (currentScriptName === name) setCurrentName(null);
        window.HsfLuaEditor.log("Deleted " + name + ".");
        refreshList();
      })
      .catch(() => window.HsfLuaEditor.log("Failed to delete " + name + ".", true));
  }

  function createScript() {
    let name = newNameInput.value.trim();
    if (!name) return;
    if (!name.endsWith(".lua")) name += ".lua";

    fetch(apiPath(name), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ code: "-- " + name + "\n" }),
    })
      .then((r) => r.json())
      .then((res) => {
        if (!res.ok) {
          window.HsfLuaEditor.log("Failed to create " + name + ".", true);
          return;
        }
        newNameInput.value = "";
        refreshList();
        openScript(name);
      })
      .catch(() => window.HsfLuaEditor.log("Failed to create " + name + ".", true));
  }

  document.getElementById("btnNewScript").addEventListener("click", createScript);
  newNameInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      createScript();
    }
  });

  document.getElementById("btnRename").addEventListener("click", () => {
    if (!currentScriptName) {
      window.HsfLuaEditor.log("Open a script first, then Rename.", true);
      return;
    }
    renameScript(currentScriptName);
  });

  document.getElementById("btnRunSelected").addEventListener("click", () => runScripts([...selected]));
  document.getElementById("btnStopSelected").addEventListener("click", () => stopScripts([...selected]));
  document.getElementById("btnSelectNone").addEventListener("click", () => {
    selected.clear();
    updateSelectedCount();
    listEl.querySelectorAll("input[type=checkbox]").forEach((cb) => (cb.checked = false));
  });

  document.getElementById("btnImportLuaScript").addEventListener("click", () => {
    const files = Array.from(document.getElementById("importLuaScript").files || [])
      .concat(Array.from(document.getElementById("importLuaFolder").files || []))
      .filter((file) => /\.lua$/i.test(file.name));
    const result = document.getElementById("importLuaResult");
    if (!files.length) {
      result.textContent = "Choose one or more Lua files, or a folder containing Lua files.";
      return;
    }

    result.textContent = `Importing 0/${files.length}...`;
    files.reduce((chain, file, index) => chain.then(() => {
      // webkitRelativePath preserves a selected folder's relative structure.
      const name = file.webkitRelativePath || file.name;
      const form = new FormData();
      form.append("name", name);
      form.append("script", file);
      return fetch("/api/lua/scripts/import", { method: "POST", body: form })
        .then(async (response) => {
          const data = await response.json().catch(() => ({}));
          if (!response.ok) throw new Error(data.error || `Import failed for ${name}`);
          result.textContent = `Importing ${index + 1}/${files.length}...`;
        });
    }), Promise.resolve())
      .then(() => {
        result.textContent = `Imported ${files.length} Lua file${files.length === 1 ? "" : "s"}.`;
        document.getElementById("importLuaScript").value = "";
        document.getElementById("importLuaFolder").value = "";
        refreshList();
      })
      .catch((error) => { result.textContent = error.message || "Import request failed."; });
  });

  window.HsfLuaScripts = {
    getCurrentName: () => currentScriptName,
    setCurrentName,
    refresh: refreshList,
    runScripts,
    stopScripts,
  };

  updateSelectedCount();
  refreshList();
})();
