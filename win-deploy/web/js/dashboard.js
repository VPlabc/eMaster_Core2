(function () {
  // Bounded so a gateway left open for days can't grow the tab's memory
  // without limit (spec section 33).
  const MAX_POINTS = 60;
  const cpuHistory = [];
  const ramHistory = [];
  const labels = [];

  const theme = () => (window.HsfTheme ? window.HsfTheme.colors() : {});

  const chart = new Chart(document.getElementById("historyChart"), {
    type: "line",
    data: {
      labels,
      datasets: [
        { label: "CPU %", data: cpuHistory, tension: 0.3, pointRadius: 0, borderWidth: 2 },
        { label: "RAM %", data: ramHistory, tension: 0.3, pointRadius: 0, borderWidth: 2 },
      ],
    },
    options: {
      // The canvas fills .hsf-chart-wrap, which owns the height -- without
      // this Chart.js forces its own 2:1 aspect ratio and ignores it.
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      scales: { y: { min: 0, max: 100 } },
      plugins: { legend: { position: "bottom" } },
    },
  });

  // Chart.js resolves colours once at construction and can't read CSS
  // variables, so theme colours are pushed in here and re-pushed whenever
  // the theme flips.
  function applyChartTheme() {
    const c = theme();
    chart.data.datasets[0].borderColor = c.accent;
    chart.data.datasets[0].backgroundColor = c.accent;
    chart.data.datasets[1].borderColor = c.warn;
    chart.data.datasets[1].backgroundColor = c.warn;

    const grid = c.border;
    const tick = c.textMuted;
    chart.options.scales.y.grid = { color: grid };
    chart.options.scales.y.ticks = { color: tick };
    chart.options.scales.x.grid = { color: grid };
    chart.options.scales.x.ticks = { color: tick, maxTicksLimit: 8 };
    chart.options.plugins.legend.labels = { color: c.text, boxWidth: 12, usePointStyle: true };
    chart.update("none");
  }
  applyChartTheme();
  window.addEventListener("hsf:themechange", applyChartTheme);

  // state: "online" | "warning" | "error" | "disconnected" | "processing"
  function setLed(id, state) {
    const el = document.getElementById(id);
    if (!el) return;
    el.classList.remove("is-online", "is-warning", "is-error", "is-disconnected", "is-processing");
    el.classList.add("is-" + state);
  }

  function clockTime(unixSeconds) {
    return new Date(unixSeconds * 1000).toLocaleTimeString();
  }

  function agoText(unixSeconds) {
    const secs = Math.max(0, Math.floor(Date.now() / 1000) - unixSeconds);
    if (secs < 60) return secs + "s";
    if (secs < 3600) return Math.floor(secs / 60) + "m";
    if (secs < 86400) return Math.floor(secs / 3600) + "h";
    return Math.floor(secs / 86400) + "d";
  }

  // While connected, the interesting fact is how long it has been up; while
  // down, it's when it was last reachable. "never" is stated plainly rather
  // than shown as a fake timestamp.
  function setConnectionMeta(id, info) {
    const el = document.getElementById(id);
    if (!el) return;
    if (!info) {
      el.textContent = "";
      return;
    }
    if (info.connected) {
      el.textContent = "up " + agoText(info.changed_at) + " · since " + clockTime(info.changed_at);
    } else if (info.last_connected > 0) {
      el.textContent = "last " + clockTime(info.last_connected) + " · " + agoText(info.last_connected) + " ago";
    } else {
      el.textContent = "never connected";
    }
  }

  function setDot(id, connected) {
    const el = document.getElementById(id);
    if (!el) return;
    el.classList.remove("ok", "bad");
    el.classList.add(connected ? "ok" : "bad");
  }

  function formatUptime(seconds) {
    const d = Math.floor(seconds / 86400);
    const h = Math.floor((seconds % 86400) / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    return `${d}d ${h}h ${m}m`;
  }

  function renderVariables(vars) {
    const tbody = document.querySelector("#variablesTable tbody");
    tbody.innerHTML = "";
    Object.keys(vars).sort().forEach((name) => {
      const value = vars[name];
      const tr = document.createElement("tr");
      // textContent, not innerHTML: these names/values come from Lua scripts
      // and would otherwise be an injection point into the dashboard.
      const tdName = document.createElement("td");
      tdName.textContent = name;
      const tdValue = document.createElement("td");
      tdValue.textContent = String(value);
      const tdType = document.createElement("td");
      tdType.textContent = typeof value;
      tr.appendChild(tdName);
      tr.appendChild(tdValue);
      tr.appendChild(tdType);
      tbody.appendChild(tr);
    });
  }

  // Rebuilding these rows every second would fight the user's pointer and
  // discard focus, so rows are created once per registration set and only
  // their state is patched afterwards. `signature` detects an actual change
  // in what's registered (script restarted, points renamed) versus a plain
  // value update.
  const ioState = { inputs: "", outputs: "" };

  function signatureOf(points) {
    return points.map((p) => p.name + ":" + p.address).join("|");
  }

  const isAdmin = () => document.documentElement.getAttribute("data-role") === "admin";

  function stateClass(point) {
    if (!point.valid) return "is-unknown";
    return point.value ? "is-on" : "is-off";
  }

  function stateText(point) {
    if (!point.valid) return "--";
    return point.value ? "ON" : "OFF";
  }

  function fcLabel(point) {
    if (point.function_code === 2) return "FC02";
    if (point.function_code === 1) return "FC01";
    return point.source === "coil" ? "FC01?" : "FC02?";
  }

  function fcTitle(point) {
    if (!point.function_code) {
      return "Not read yet. Registered source: " + (point.source || "auto") + ".";
    }
    const space = point.function_code === 2 ? "discrete inputs (FC02)" : "coils (FC01)";
    const other = point.function_code === 2 ? "coil" : "discrete";
    return "Read from " + space + ". If this value never changes, the live state is probably in the " +
           "other address space — pass \"" + other + "\" as the third argument to " +
           "Modbus.RegisterInput(name, address, source).";
  }

  function buildRow(point, isOutput) {
    const row = document.createElement("div");
    row.className = "hsf-io-row " + stateClass(point);
    row.dataset.name = point.name;

    const name = document.createElement("span");
    name.className = "hsf-io-name";
    name.textContent = point.name;
    row.appendChild(name);

    const addr = document.createElement("span");
    addr.className = "hsf-io-addr";
    addr.textContent = "@" + point.address;
    row.appendChild(addr);

    // Which address space answered. Inputs can legitimately come from either
    // coils (FC01) or discrete inputs (FC02), and picking the wrong one shows
    // up as a value that simply never changes -- so it is worth surfacing
    // rather than leaving the operator to guess.
    if (!isOutput) {
      const fc = document.createElement("span");
      fc.className = "hsf-io-fc";
      fc.textContent = fcLabel(point);
      fc.title = fcTitle(point);
      row.appendChild(fc);
    }

    const state = document.createElement("span");
    state.className = "hsf-io-state";
    const dot = document.createElement("span");
    dot.className = "hsf-io-dot";
    const label = document.createElement("span");
    label.className = "hsf-io-label";
    label.textContent = stateText(point);
    state.appendChild(dot);
    state.appendChild(label);
    row.appendChild(state);

    // Outputs are actuator commands to a live machine, so the control is
    // Admin-only both here and via the data-requires-role CSS gate.
    if (isOutput && isAdmin()) {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btn btn-sm btn-outline-secondary ms-2";
      btn.textContent = "Toggle";
      btn.setAttribute("data-requires-role", "admin");
      btn.addEventListener("click", () => toggleOutput(point.name, row, btn));
      row.appendChild(btn);
    }

    return row;
  }

  function toggleOutput(name, row, btn) {
    // Read the current state off the row rather than a captured value: the
    // point is repolled every cycle and a stale closure would send the
    // wrong target.
    const current = row.classList.contains("is-on");
    btn.disabled = true;
    fetch("/api/modbus/output", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name, value: !current }),
    })
      .then((r) => r.json().then((body) => ({ ok: r.ok, body })))
      .then(({ ok, body }) => {
        if (!ok) {
          console.warn("output toggle failed:", body && body.error);
          return;
        }
        document.getElementById("testModeBadge").hidden = false;
      })
      .catch((err) => console.warn("output toggle failed:", err))
      .finally(() => {
        btn.disabled = false;
      });
  }

  function renderIo(containerId, emptyId, points, isOutput) {
    const container = document.getElementById(containerId);
    const empty = document.getElementById(emptyId);
    if (!container) return;

    const key = isOutput ? "outputs" : "inputs";
    const sig = signatureOf(points);

    if (sig !== ioState[key]) {
      ioState[key] = sig;
      container.innerHTML = "";
      points.forEach((p) => container.appendChild(buildRow(p, isOutput)));
    } else {
      points.forEach((p) => {
        const row = container.querySelector('[data-name="' + CSS.escape(p.name) + '"]');
        if (!row) return;
        row.classList.remove("is-on", "is-off", "is-unknown");
        row.classList.add(stateClass(p));
        const label = row.querySelector(".hsf-io-label");
        if (label) label.textContent = stateText(p);
        // An "auto" point latches onto a space on its first successful read,
        // so this label changes after the row was built.
        const fc = row.querySelector(".hsf-io-fc");
        if (fc) {
          fc.textContent = fcLabel(p);
          fc.title = fcTitle(p);
        }
      });
    }

    if (empty) empty.hidden = points.length > 0;
  }

  // ---- PLC Registers -----------------------------------------------------

  // Same rebuild-vs-patch split as the I/O rows: an editable field must not
  // be torn out from under the operator mid-type, so rows are rebuilt only
  // when the declared set actually changes.
  let registerSig = "";

  function registerSignature(regs) {
    return regs.map((r) => r.name + ":" + r.address + ":" + r.type + ":" + r.endian).join("|");
  }

  function formatRegisterValue(reg) {
    if (!reg.valid) return "--";
    if (reg.value === null || reg.value === undefined) return "NaN / Inf";

    // Format by the DECLARED type, not by whether JS considers the number an
    // integer: a float read with the wrong endianness lands on values like
    // 2.7e23, which Number.isInteger() calls an integer and would print in
    // full. Those garbage magnitudes are exactly what the card exists to make
    // obvious, so they need the compact float treatment most of all.
    const isFloat = reg.type === "float32" || reg.type === "float64";
    if (isFloat && typeof reg.value === "number") {
      const digits = reg.type === "float32" ? 7 : 15;
      const magnitude = Math.abs(reg.value);
      if (magnitude !== 0 && (magnitude >= 1e9 || magnitude < 1e-4)) {
        return reg.value.toExponential(6);
      }
      // Float32 carries ~7 significant digits; printing the full double
      // expansion (36.599998474121094) is just noise.
      return String(parseFloat(reg.value.toPrecision(digits)));
    }
    return String(reg.value);
  }

  function formatRaw(reg) {
    if (!reg.raw || !reg.raw.length) return "--";
    return reg.raw.map((w) => w.toString(16).toUpperCase().padStart(4, "0")).join(" ");
  }

  function buildRegisterRow(reg) {
    const tr = document.createElement("tr");
    tr.dataset.name = reg.name;

    const tdName = document.createElement("td");
    tdName.textContent = reg.name;
    if (reg.unit) {
      const unit = document.createElement("span");
      unit.className = "text-muted small ms-1";
      unit.textContent = reg.unit;
      tdName.appendChild(unit);
    }

    const tdAddr = document.createElement("td");
    tdAddr.className = "hsf-io-addr";
    tdAddr.textContent = "@" + reg.address +
      (reg.count > 1 ? "-" + (reg.address + reg.count - 1) : "");

    const tdType = document.createElement("td");
    const typeBadge = document.createElement("span");
    typeBadge.className = "hsf-io-fc";
    typeBadge.textContent = reg.type;
    typeBadge.title = "FC0" + reg.function_code + " " +
      (reg.function_code === 4 ? "input register (read-only)" : "holding register") +
      ", " + reg.count + " register(s)";
    tdType.appendChild(typeBadge);

    const tdEndian = document.createElement("td");
    const endianBadge = document.createElement("span");
    endianBadge.className = "hsf-io-fc";
    endianBadge.textContent = reg.endian;
    endianBadge.title = "Byte/word order. If the value looks wrong, compare the Raw column " +
      "against the other orders (ABCD / BADC / CDAB / DCBA).";
    tdEndian.appendChild(endianBadge);

    const tdRaw = document.createElement("td");
    tdRaw.className = "hsf-reg-raw";
    tdRaw.textContent = formatRaw(reg);

    const tdValue = document.createElement("td");
    tdValue.className = "hsf-reg-value";
    tdValue.textContent = formatRegisterValue(reg);

    const tdAction = document.createElement("td");
    if (reg.writable && isAdmin()) {
      const group = document.createElement("div");
      group.className = "input-group input-group-sm hsf-reg-write";

      const input = document.createElement("input");
      input.className = "form-control form-control-sm";
      input.placeholder = "value";
      input.setAttribute("aria-label", "New value for " + reg.name);

      const btn = document.createElement("button");
      btn.className = "btn btn-outline-secondary";
      btn.type = "button";
      btn.textContent = "Set";
      btn.setAttribute("data-requires-role", "admin");
      btn.addEventListener("click", () => writeRegister(reg.name, input, btn));
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter") btn.click();
      });

      group.append(input, btn);
      tdAction.appendChild(group);
    } else if (reg.function_code === 4) {
      const ro = document.createElement("span");
      ro.className = "text-muted small";
      ro.textContent = "read-only";
      tdAction.appendChild(ro);
    }

    tr.append(tdName, tdAddr, tdType, tdEndian, tdRaw, tdValue, tdAction);
    return tr;
  }

  function writeRegister(name, input, btn) {
    const raw = input.value.trim();
    if (!raw) return;
    btn.disabled = true;
    fetch("/api/modbus/register", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name, value: raw }),
    })
      .then((r) => r.json().then((body) => ({ ok: r.ok, body })))
      .then(({ ok, body }) => {
        if (!ok) {
          // The encoder's message ("value 70000 does not fit uint16") is the
          // useful part -- surfacing it beats a generic failure toast.
          input.classList.add("is-invalid");
          input.title = (body && body.error) || "write failed";
          return;
        }
        input.classList.remove("is-invalid");
        input.title = "";
        input.value = "";
        document.getElementById("testModeBadge").hidden = false;
      })
      .catch((err) => {
        input.classList.add("is-invalid");
        input.title = String(err);
      })
      .finally(() => {
        btn.disabled = false;
      });
  }

  function renderRegisters(regs) {
    const tbody = document.querySelector("#plcRegisters tbody");
    const empty = document.getElementById("plcRegistersEmpty");
    if (!tbody) return;

    const sig = registerSignature(regs);
    if (sig !== registerSig) {
      registerSig = sig;
      tbody.innerHTML = "";
      regs.forEach((r) => tbody.appendChild(buildRegisterRow(r)));
    } else {
      regs.forEach((r) => {
        const row = tbody.querySelector('[data-name="' + CSS.escape(r.name) + '"]');
        if (!row) return;
        row.querySelector(".hsf-reg-raw").textContent = formatRaw(r);
        const valueCell = row.querySelector(".hsf-reg-value");
        valueCell.textContent = formatRegisterValue(r);
        // A failing read greys the row and explains itself on hover, rather
        // than leaving a stale value looking current.
        row.classList.toggle("hsf-reg-stale", !!r.error);
        valueCell.title = r.error || "";
      });
    }

    if (empty) empty.hidden = regs.length > 0;
    document.getElementById("plcRegisters").hidden = regs.length === 0;
  }

  function formatUptimeShort(seconds) {
    const s = Math.max(0, Math.floor(seconds));
    if (s < 60) return s + "s";
    const m = Math.floor(s / 60);
    if (m < 60) return m + "m " + (s % 60) + "s";
    const h = Math.floor(m / 60);
    return h + "h " + (m % 60) + "m";
  }

  function runtimeStateClass(state) {
    if (state === "RUNNING") return "hsf-badge-ok";
    if (state === "ERROR") return "hsf-badge-bad";
    if (state === "STARTING" || state === "STOPPING") return "hsf-badge-warn";
    return "hsf-badge-idle";
  }

  function renderLuaRuntime(runtime) {
    const wrap = document.getElementById("luaRuntimeWrap");
    const empty = document.getElementById("luaRuntimeEmpty");
    const errWrap = document.getElementById("luaRuntimeErrorWrap");
    const errEl = document.getElementById("luaRuntimeError");
    const countEl = document.getElementById("luaRuntimeCount");
    if (!wrap) return;

    const scripts = (runtime && runtime.scripts) || [];
    wrap.hidden = scripts.length === 0;
    empty.hidden = scripts.length > 0;

    if (countEl) {
      const running = (runtime && runtime.running_count) || 0;
      countEl.hidden = scripts.length === 0;
      countEl.textContent = running + " running of " + scripts.length;
    }

    const tbody = wrap.querySelector("tbody");
    tbody.innerHTML = "";
    scripts.forEach((sc) => {
      const tr = document.createElement("tr");
      const cells = [
        sc.name,
        sc.state,
        sc.cpu_percent.toFixed(1) + "%",
        // MB rather than KB, matching section 24's table -- and the point of
        // the column is "is this script's memory growing", which reads better
        // in the same unit the dashboard's process RAM uses.
        (sc.memory_kb / 1024).toFixed(2) + " MB",
        // -1 means the debug hook hasn't sampled yet; show that rather
        // than pretending the script is pinned to core -1.
        sc.cpu_core >= 0 ? String(sc.cpu_core) : "--",
        String(sc.runtime_id || "--"),
        sc.start_time ? new Date(sc.start_time * 1000).toLocaleTimeString() : "--",
        formatUptimeShort(sc.uptime_seconds),
      ];
      cells.forEach((text, i) => {
        const td = document.createElement("td");
        td.textContent = text;
        if (i === 0 && sc.path) td.title = sc.path;
        if (i === 1) {
          const badge = document.createElement("span");
          badge.className = "hsf-badge " + runtimeStateClass(text);
          badge.textContent = text;
          td.textContent = "";
          td.appendChild(badge);
        }
        tr.appendChild(td);
      });
      tbody.appendChild(tr);
    });

    // One block per failed runtime. Kept visible after a script stops: the
    // error is usually the reason it stopped, so clearing it with the state
    // would hide the explanation.
    const failed = scripts.filter((sc) => sc.last_error);
    errWrap.hidden = failed.length === 0;
    errEl.innerHTML = "";
    failed.forEach((sc) => {
      const block = document.createElement("div");
      block.className = "hsf-runtime-error mb-2";
      const head = document.createElement("div");
      head.className = "fw-semibold";
      head.textContent =
        sc.name + (sc.error_line ? " (line " + sc.error_line + ")" : "") + " - runtime #" + sc.runtime_id;
      const body = document.createElement("div");
      body.textContent = sc.last_error;
      block.append(head, body);
      errEl.appendChild(block);
    });
  }

  HsfWs.onMessage((payload) => {
    if (payload.type !== "status") return;
    const s = payload.status;

    document.getElementById("statCpu").textContent = s.cpu_percent.toFixed(1) + "%";
    document.getElementById("statRam").textContent = Math.round(s.ram_used_mb) + " / " + Math.round(s.ram_total_mb) + " MB";
    document.getElementById("statDisk").textContent = s.disk_used_gb.toFixed(1) + " / " + s.disk_total_gb.toFixed(1) + " GB";
    document.getElementById("statUptime").textContent = formatUptime(s.uptime_seconds);
    document.getElementById("statThreads").textContent = s.thread_count;
    document.getElementById("statNetwork").textContent = s.network_up ? "Online" : "Offline";
    setDot("statNetworkDot", s.network_up);

    setLed("ledRest", s.rest_ok ? "online" : "disconnected");
    document.getElementById("txtRest").textContent = s.rest_ok ? "OK" : "Unavailable";
    setLed("ledPlc", s.plc_connected ? "online" : "error");
    document.getElementById("txtPlc").textContent = s.plc_connected ? "Connected" : "Disconnected";
    setLed("ledRfid", s.rfid_connected ? "online" : "disconnected");
    document.getElementById("txtRfid").textContent = s.rfid_connected ? "Connected" : "Disconnected";
    setLed("ledSerial", s.serial_open ? "online" : "disconnected");
    document.getElementById("txtSerial").textContent = s.serial_open ? "Open" : "Closed";
    setLed("ledLua", s.lua_running ? "processing" : "disconnected");
    const luaCount = (payload.lua_runtime && payload.lua_runtime.running_count) || 0;
    document.getElementById("txtLua").textContent =
      luaCount > 1 ? luaCount + " scripts running" : s.lua_running ? "Running" : "Stopped";

    const conns = s.connections || {};
    setConnectionMeta("metaRest", conns.rest);
    setConnectionMeta("metaPlc", conns.plc);
    setConnectionMeta("metaRfid", conns.rfid);
    setConnectionMeta("metaSerial", conns.serial);
    setConnectionMeta("metaLua", conns.lua);

    const ramPercent = s.ram_total_mb > 0 ? (s.ram_used_mb / s.ram_total_mb) * 100 : 0;
    labels.push(new Date().toLocaleTimeString());
    cpuHistory.push(s.cpu_percent);
    ramHistory.push(ramPercent);
    if (labels.length > MAX_POINTS) {
      labels.shift();
      cpuHistory.shift();
      ramHistory.shift();
    }
    chart.update("none");

    const io = payload.modbus_io || { inputs: [], outputs: [], registers: [] };
    renderIo("plcInputs", "plcInputsEmpty", io.inputs || [], false);
    renderIo("plcOutputs", "plcOutputsEmpty", io.outputs || [], true);
    renderRegisters(io.registers || []);

    renderLuaRuntime(payload.lua_runtime);

    renderVariables(payload.variables || {});
  });
})();
