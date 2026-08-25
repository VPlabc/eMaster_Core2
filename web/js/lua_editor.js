(function () {
  const editor = document.getElementById("editor");
  const highlightCode = document.querySelector("#highlightLayer code");
  const highlightLayer = document.getElementById("highlightLayer");
  const errorConsole = document.getElementById("errorConsole");

  const KEYWORDS = [
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while",
  ];
  const API_NAMES = [
    "Rest", "Serial2", "Serial", "Tcp", "Modbus", "Card", "Rfid", "Log", "Config",
    "SetVariable", "GetVariable", "Sleep",
  ];

  function escapeHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  // ONE alternation, matched in priority order, so a token is classified
  // once and never rescanned.
  //
  // This replaced a chain of per-token-type .replace() passes over the
  // partially-marked-up string, which corrupted itself: the comment pass
  // inserted <span class="lua-com">, then the string pass matched the
  // quoted "lua-com" *inside that tag* and wrapped it in another span. The
  // result nested tags into each other and leaked `"lua-com">` into the
  // rendered text at the start of every comment line.
  //
  // Comments come first so `-- "not a string"` stays a comment; strings
  // before numbers/keywords so `"end"` isn't highlighted as a keyword.
  const TOKEN_PATTERN = new RegExp(
    [
      "(--\\[\\[[\\s\\S]*?\\]\\])",                         // long comment
      "(--[^\\n]*)",                                        // line comment
      "(\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*')",   // string
      "(\\b\\d+(?:\\.\\d+)?\\b)",                           // number
      "\\b(" + KEYWORDS.join("|") + ")\\b",                 // keyword
      "\\b(" + API_NAMES.join("|") + ")\\b",                // gateway API
    ].join("|"),
    "g"
  );

  function highlight(code) {
    let out = "";
    let last = 0;
    let match;

    TOKEN_PATTERN.lastIndex = 0;
    while ((match = TOKEN_PATTERN.exec(code)) !== null) {
      // Zero-length matches would spin forever; nothing in the pattern can
      // produce one, but the guard costs nothing next to a hung editor.
      if (match[0] === "") {
        TOKEN_PATTERN.lastIndex++;
        continue;
      }

      const [full, longComment, lineComment, str, num, keyword] = match;
      const cls =
        longComment || lineComment ? "lua-com" :
        str ? "lua-str" :
        num ? "lua-num" :
        keyword ? "lua-kw" : "lua-fn";

      // Escaped per-piece, after tokenising -- escaping the whole document
      // up front is what let the later passes see markup as content.
      out += escapeHtml(code.slice(last, match.index));
      out += '<span class="' + cls + '">' + escapeHtml(full) + "</span>";
      last = match.index + full.length;
    }

    return out + escapeHtml(code.slice(last));
  }

  function refreshHighlight() {
    highlightCode.innerHTML = highlight(editor.value) + "\n";
  }

  editor.addEventListener("input", refreshHighlight);
  editor.addEventListener("scroll", () => {
    highlightLayer.scrollTop = editor.scrollTop;
    highlightLayer.scrollLeft = editor.scrollLeft;
  });

  function log(message, isError) {
    const time = new Date().toLocaleTimeString();
    const line = document.createElement("div");
    line.textContent = `[${time}] ${message}`;
    if (!isError) line.style.color = "#9cdcfe";
    errorConsole.appendChild(line);
    errorConsole.scrollTop = errorConsole.scrollHeight;
  }

  function loadScript() {
    fetch("/api/lua/script")
      .then((r) => r.json())
      .then((data) => {
        editor.value = data.code || "";
        refreshHighlight();
        if (window.HsfLuaScripts) window.HsfLuaScripts.setCurrentName(null);
        document.getElementById("currentScriptLabel").textContent = "(default startup script)";
        log("Script loaded.");
      })
      .catch(() => log("Failed to load script.", true));
  }

  function postJson(url, body) {
    return fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) })
      .then((r) => r.json());
  }

  /* Run acts on the file when one is open, and on the buffer otherwise.
     Before scripts were independent, both went to /api/lua/run and replaced
     whatever was running -- so pressing Run on an open file started an
     anonymous copy of its text rather than the script the tree was tracking,
     and the two could not be told apart in the runtime list. */
  document.getElementById("btnRun").addEventListener("click", () => {
    const currentName = window.HsfLuaScripts && window.HsfLuaScripts.getCurrentName();
    if (currentName) {
      window.HsfLuaScripts.runScripts([currentName]);
      return;
    }
    postJson("/api/lua/run", { code: editor.value }).then((res) => {
      log(res.ok ? "Editor buffer started." : "Run failed: " + res.error, !res.ok);
    });
  });

  document.getElementById("btnStop").addEventListener("click", () => {
    const currentName = window.HsfLuaScripts && window.HsfLuaScripts.getCurrentName();
    if (currentName) {
      window.HsfLuaScripts.stopScripts([currentName]);
      return;
    }
    // No file open: this is the buffer's own runtime.
    postJson("/api/lua/stop?name=" + encodeURIComponent("(editor buffer)")).then((res) => {
      log(res.ok ? "Editor buffer stopped." : "Editor buffer was not running.");
    });
  });

  document.getElementById("btnRestart").addEventListener("click", () => {
    const currentName = window.HsfLuaScripts && window.HsfLuaScripts.getCurrentName();
    const url = "/api/lua/restart?name=" + encodeURIComponent(currentName || "(editor buffer)");
    postJson(url).then((res) => {
      log(res.ok ? "Restarted " + (currentName || "the editor buffer") + "." : "Restart failed: " + res.error,
          !res.ok);
      if (window.HsfLuaScripts) window.HsfLuaScripts.refresh();
    });
  });

  document.getElementById("btnPruneRuntimes").addEventListener("click", () => {
    postJson("/api/lua/runtimes/prune").then((res) => {
      log("Forgot " + (res.removed || 0) + " stopped runtime(s).");
      if (window.HsfLuaScripts) window.HsfLuaScripts.refresh();
    });
  });

  document.getElementById("btnValidate").addEventListener("click", () => {
    postJson("/api/lua/validate", { code: editor.value }).then((res) => {
      log(res.ok ? "No syntax errors." : "Syntax error: " + res.error, !res.ok);
    });
  });

  document.getElementById("btnSave").addEventListener("click", () => {
    const currentName = window.HsfLuaScripts && window.HsfLuaScripts.getCurrentName();
    const url = currentName
      ? "/api/lua/scripts/file?name=" + encodeURIComponent(currentName)
      : "/api/lua/script";
    postJson(url, { code: editor.value }).then((res) => {
      log(res.ok ? "Saved" + (currentName ? " " + currentName + "." : ".") : "Save failed.", !res.ok);
      if (res.ok && window.HsfLuaScripts) window.HsfLuaScripts.refresh();
    });
  });

  document.getElementById("btnLoad").addEventListener("click", loadScript);

  /* Runtime strip. Rebuilt from the aggregate carried in the status frame
     rather than from section 25's per-runtime frames: the strip shows every
     runtime at once, and assembling that from a stream of single-runtime
     messages would mean tracking which ones stopped arriving. */
  const runtimeStrip = document.getElementById("runtimeStrip");
  const runtimeChips = document.getElementById("runtimeChips");
  // Reported errors, so a fault is written to the error panel once instead of
  // once per second for as long as the runtime stays in the list.
  const reportedErrors = {};

  function renderRuntimes(runtime) {
    const scripts = (runtime && runtime.scripts) || [];
    runtimeStrip.hidden = scripts.length === 0;
    runtimeChips.innerHTML = "";

    scripts.forEach((script) => {
      const chip = document.createElement("span");
      const cls =
        script.state === "RUNNING" ? "hsf-badge-ok" :
        script.state === "ERROR" ? "hsf-badge-bad" :
        script.state === "STOPPED" ? "hsf-badge-idle" :
        "hsf-badge-warn";
      chip.className = "hsf-badge " + cls;
      chip.textContent = script.name + " " + script.state;
      chip.title =
        "runtime #" + script.runtime_id +
        (script.last_error ? "\n" + script.last_error : "") +
        (script.path ? "\n" + script.path : "");
      runtimeChips.appendChild(chip);

      // Section 23 wants script errors in the editor's error panel too.
      if (script.state === "ERROR" && script.last_error) {
        const key = script.runtime_id + ":" + script.last_error;
        if (!reportedErrors[key]) {
          reportedErrors[key] = true;
          log(
            script.name + " stopped with an error" +
              (script.error_line ? " at line " + script.error_line : "") + ": " + script.last_error,
            true
          );
        }
      }
    });
  }

  let lastRunningCount = null;

  HsfWs.onMessage((payload) => {
    if (payload.type !== "status") return;
    const running = payload.status.lua_running;
    const dot = document.getElementById("dotLua");
    dot.classList.remove("ok", "bad");
    dot.classList.add(running ? "ok" : "bad");

    const count = (payload.lua_runtime && payload.lua_runtime.running_count) || 0;
    document.getElementById("txtLua").textContent =
      count > 1 ? count + " running" : running ? "Running" : "Stopped";

    renderRuntimes(payload.lua_runtime);

    // Keeps the tree's state badges honest without polling: the list is only
    // refetched when the number of running scripts actually changes.
    if (lastRunningCount !== null && lastRunningCount !== count && window.HsfLuaScripts) {
      window.HsfLuaScripts.refresh();
    }
    lastRunningCount = count;
  });

  window.HsfLuaEditor = {
    setCode(code) {
      editor.value = code;
      refreshHighlight();
    },
    log,
  };

  loadScript();
})();
