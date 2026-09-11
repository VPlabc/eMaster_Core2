// Test Tools page (updateUI.md sections 24-31).
//
// Every panel here talks to a /api/test/* endpoint that performs the request
// from the gateway process rather than from the browser. That is the whole
// point: the browser cannot open a Modbus socket or a COM port, and it cannot
// reach a PLC on an isolated network segment. Card Clients keeps its own
// script (card_clients.js) since that panel predates this page.
(function () {
  const networkCheck = document.getElementById("netCheck");
  if (networkCheck) networkCheck.addEventListener("click", async function () {
    const badge = document.getElementById("netBadge");
    const output = document.getElementById("netOutput");
    const started = performance.now();
    badge.textContent = "Checking"; badge.className = "hsf-badge hsf-badge-warn";
    try {
      const response = await fetch("/api/health", { cache: "no-store" });
      const body = await response.text();
      const websocket = window.HsfWs ? "shared WebSocket client loaded" : "WebSocket client unavailable";
      badge.textContent = response.ok ? "OK" : "HTTP " + response.status;
      badge.className = "hsf-badge " + (response.ok ? "hsf-badge-ok" : "hsf-badge-bad");
      output.textContent = "HTTP /api/health: " + response.status + " (" + Math.round(performance.now() - started) + " ms)\n" + websocket + "\n" + body;
    } catch (error) {
      badge.textContent = "Failed"; badge.className = "hsf-badge hsf-badge-bad"; output.textContent = String(error);
    }
  });
  // ---------------------------------------------------------------- helpers

  // "error" reads better at the call sites than the stylesheet's "bad".
  var BADGE_CLASS = { ok: "ok", error: "bad", warn: "warn", idle: "idle" };

  function setBadge(el, state, text) {
    el.className = "hsf-badge hsf-badge-" + (BADGE_CLASS[state] || "idle");
    el.textContent = text;
  }

  // textContent everywhere, never innerHTML: everything shown here is a
  // response body or device payload from an arbitrary endpoint the user
  // pointed us at. Rendering that as markup would be an injection route.
  function show(el, text) {
    el.textContent = text;
  }

  function prettyJson(text) {
    try {
      return JSON.stringify(JSON.parse(text), null, 2);
    } catch (e) {
      return text;   // not JSON -- show it raw rather than hiding it
    }
  }

  function postJson(url, payload) {
    return fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    }).then(function (r) {
      return r.json().then(function (data) {
        return { httpStatus: r.status, data: data };
      });
    });
  }

  // ------------------------------------------------------- REST API Client

  var rcStatus = document.getElementById("rcStatus");
  var rcElapsed = document.getElementById("rcElapsed");
  var rcBodyHint = document.getElementById("rcBodyHint");

  function bodyMode() {
    return document.querySelector('input[name="rcBodyMode"]:checked').value;
  }

  Array.prototype.forEach.call(document.querySelectorAll('input[name="rcBodyMode"]'), function (radio) {
    radio.addEventListener("change", function () {
      rcBodyHint.textContent = bodyMode() === "form"
        ? "One field per line: name=value. Sent as multipart/form-data."
        : "Sent verbatim. Set Content-Type in Headers.";
      document.getElementById("rcBody").placeholder = bodyMode() === "form"
        ? "CardData=04AABBCCDDEE\nPosition=1"
        : '{"key": "value"}';
    });
  });

  function parseHeaderLines(text) {
    var out = {};
    text.split("\n").forEach(function (line) {
      var trimmed = line.trim();
      if (!trimmed) return;
      var idx = trimmed.indexOf(":");
      if (idx <= 0) return;
      out[trimmed.slice(0, idx).trim()] = trimmed.slice(idx + 1).trim();
    });
    return out;
  }

  function parseFormLines(text) {
    var out = {};
    text.split("\n").forEach(function (line) {
      var trimmed = line.trim();
      if (!trimmed) return;
      var idx = trimmed.indexOf("=");
      if (idx <= 0) return;
      out[trimmed.slice(0, idx).trim()] = trimmed.slice(idx + 1);
    });
    return out;
  }

  document.getElementById("rcSend").addEventListener("click", function () {
    var url = document.getElementById("rcUrl").value.trim();
    if (!url) {
      setBadge(rcStatus, "error", "URL required");
      return;
    }

    var payload = {
      method: document.getElementById("rcMethod").value,
      url: url,
      headers: parseHeaderLines(document.getElementById("rcHeaders").value),
      timeout_ms: parseInt(document.getElementById("rcTimeout").value, 10) || 5000,
      verify_ssl: document.getElementById("rcVerifySsl").checked
    };

    var raw = document.getElementById("rcBody").value;
    if (bodyMode() === "form") {
      payload.form_fields = parseFormLines(raw);
    } else {
      payload.body = raw;
    }

    setBadge(rcStatus, "idle", "Sending...");
    rcElapsed.textContent = "";
    postJson("/api/test/rest", payload).then(function (res) {
      var d = res.data;
      if (d.error) {
        setBadge(rcStatus, "error", d.error);
      } else {
        setBadge(rcStatus, d.ok ? "ok" : "error", "HTTP " + d.status);
      }
      rcElapsed.textContent = d.elapsed_ms != null ? d.elapsed_ms.toFixed(0) + " ms" : "";
      show(document.getElementById("rcResponseBody"), prettyJson(d.body || ""));
      show(document.getElementById("rcResponseHeaders"), d.headers || "");
    }).catch(function (e) {
      setBadge(rcStatus, "error", "Request failed");
      show(document.getElementById("rcResponseBody"), String(e));
    });
  });

  // ------------------------------------------------------- REST API Server

  // Only GETs are marked tryable. The state-changing routes are listed so the
  // page documents the real surface, but firing them from a browse-the-API
  // panel would mean an idle click could write a coil or clear the logs.
  var SERVER_ROUTES = [
    { m: "GET", p: "/api/status", d: "Gateway + connection status", try: true },
    { m: "GET", p: "/api/config", d: "Full configuration", try: true },
    { m: "GET", p: "/api/logs?limit=20", d: "Recent log entries", try: true },
    { m: "GET", p: "/api/variables", d: "Lua runtime variables", try: true },
    { m: "GET", p: "/api/modbus/io", d: "Registered PLC inputs/outputs", try: true },
    { m: "GET", p: "/api/lua/runtime", d: "Lua script runtime state", try: true },
    { m: "GET", p: "/api/serial/ports", d: "Serial ports on this host", try: true },
    { m: "GET", p: "/api/card/cache", d: "Last card received", try: true },
    { m: "GET", p: "/api/card/clients", d: "Card reader clients", try: true },
    { m: "POST", p: "/api/config", d: "Update configuration" },
    { m: "POST", p: "/api/card/input", d: "Submit a card UID (API-Key required)" },
    { m: "POST", p: "/api/modbus/output", d: "Write a registered PLC output" },
    { m: "POST", p: "/api/lua/run", d: "Run Lua source" },
    { m: "POST", p: "/api/logs/clear", d: "Clear the in-memory log buffer" },
    { m: "GET", p: "/ws", d: "WebSocket status/log stream (1 s tick)" }
  ];

  (function buildRouteTable() {
    var tbody = document.querySelector("#serverRoutesTable tbody");
    SERVER_ROUTES.forEach(function (route) {
      var tr = document.createElement("tr");

      var tdM = document.createElement("td");
      var code = document.createElement("code");
      code.textContent = route.m;
      tdM.appendChild(code);

      var tdP = document.createElement("td");
      var pCode = document.createElement("code");
      pCode.textContent = route.p;
      tdP.appendChild(pCode);

      var tdD = document.createElement("td");
      tdD.textContent = route.d;

      var tdA = document.createElement("td");
      if (route.try) {
        var btn = document.createElement("button");
        btn.className = "btn btn-sm btn-outline-secondary";
        btn.textContent = "Try";
        btn.addEventListener("click", function () {
          var badge = document.getElementById("rsStatus");
          setBadge(badge, "idle", "Fetching...");
          fetch(route.p)
            .then(function (r) {
              return r.text().then(function (t) {
                setBadge(badge, r.ok ? "ok" : "error", route.p + " - HTTP " + r.status);
                show(document.getElementById("rsOutput"), prettyJson(t));
              });
            })
            .catch(function (e) {
              setBadge(badge, "error", "Failed");
              show(document.getElementById("rsOutput"), String(e));
            });
        });
        tdA.appendChild(btn);
      }

      tr.append(tdM, tdP, tdD, tdA);
      tbody.appendChild(tr);
    });
  })();

  // ----------------------------------------------------- Modbus TCP Client

  var mbOp = document.getElementById("mbOp");
  var mbStatus = document.getElementById("mbStatus");
  var mbBits = document.getElementById("mbBits");

  function syncModbusFields() {
    var op = mbOp.value;
    var isWrite = op === "write_coil" || op === "write_holding";
    var isConnect = op === "connect";

    document.getElementById("mbCountWrap").classList.toggle("d-none", isWrite || isConnect);
    document.getElementById("mbValueWrap").classList.toggle("d-none", !isWrite);
    document.getElementById("mbAddress").parentElement.classList.toggle("d-none", isConnect);

    document.getElementById("mbBoolValue").classList.toggle("d-none", op !== "write_coil");
    document.getElementById("mbNumValue").classList.toggle("d-none", op !== "write_holding");
  }
  mbOp.addEventListener("change", syncModbusFields);
  syncModbusFields();

  // Seed IP/port from the configured PLC so the common case is one click.
  fetch("/api/config").then(function (r) { return r.json(); }).then(function (cfg) {
    if (cfg.modbus) {
      if (cfg.modbus.ip) document.getElementById("mbIp").value = cfg.modbus.ip;
      if (cfg.modbus.port) document.getElementById("mbPort").value = cfg.modbus.port;
    }
  }).catch(function () { /* leave the fields blank */ });

  function renderBits(values, address) {
    mbBits.innerHTML = "";
    if (!values || !values.length) return;
    values.forEach(function (v, i) {
      var chip = document.createElement("span");
      chip.className = "hsf-io-chip " + (v ? "hsf-io-on" : "hsf-io-off");
      chip.textContent = (address + i) + ": " + (v ? "1" : "0");
      mbBits.appendChild(chip);
    });
  }

  function modbusPayload() {
    var op = mbOp.value;
    var payload = {
      ip: document.getElementById("mbIp").value.trim(),
      port: parseInt(document.getElementById("mbPort").value, 10) || 502,
      op: op,
      address: parseInt(document.getElementById("mbAddress").value, 10) || 0,
      count: parseInt(document.getElementById("mbCount").value, 10) || 1
    };
    if (op === "write_coil") payload.value = document.getElementById("mbBoolValue").value === "true";
    if (op === "write_holding") payload.value = parseInt(document.getElementById("mbNumValue").value, 10) || 0;
    return payload;
  }

  document.getElementById("mbSend").addEventListener("click", function () {
    var payload = modbusPayload();
    if (!payload.ip) {
      setBadge(mbStatus, "error", "IP required");
      return;
    }
    setBadge(mbStatus, "idle", "Working...");
    mbBits.innerHTML = "";

    postJson("/api/test/modbus", payload).then(function (res) {
      var d = res.data;
      if (d.error && !d.ok) {
        setBadge(mbStatus, "error", d.error);
      } else {
        setBadge(mbStatus, d.ok ? "ok" : "error", d.ok ? "OK" : "Failed");
      }
      if (d.function_code === 1 || d.function_code === 2) renderBits(d.values, payload.address);
      show(document.getElementById("mbOutput"), JSON.stringify(d, null, 2));
    }).catch(function (e) {
      setBadge(mbStatus, "error", "Request failed");
      show(document.getElementById("mbOutput"), String(e));
    });
  });

  // Mirrors config/scripts/io_test.lua. Vendors disagree wildly on where the
  // physical I/O lands, and an exception 0x02 from one base says nothing about
  // the others -- so sweeping them is the fastest way to find a live address.
  var SCAN_BASES = [0, 1, 100, 1000, 1024, 2048, 4096, 8192, 10000, 10001];

  document.getElementById("mbScan").addEventListener("click", function () {
    var ip = document.getElementById("mbIp").value.trim();
    var port = parseInt(document.getElementById("mbPort").value, 10) || 502;
    if (!ip) {
      setBadge(mbStatus, "error", "IP required");
      return;
    }

    setBadge(mbStatus, "idle", "Scanning " + (SCAN_BASES.length * 2) + " addresses...");
    mbBits.innerHTML = "";
    var out = document.getElementById("mbOutput");
    show(out, "");

    var jobs = [];
    SCAN_BASES.forEach(function (base) {
      ["read_coils", "read_discrete_inputs"].forEach(function (op) {
        jobs.push({ base: base, op: op });
      });
    });

    // Sequential on purpose: many small PLCs accept only one TCP connection at
    // a time, and firing 20 at once makes every one of them fail to connect.
    var lines = [];
    var index = 0;
    (function next() {
      if (index >= jobs.length) {
        var hits = lines.filter(function (l) { return l.indexOf("OK") === 0; });
        setBadge(mbStatus, hits.length ? "ok" : "error",
                 hits.length ? hits.length + " readable" : "No address responded");
        return;
      }
      var job = jobs[index++];
      postJson("/api/test/modbus",
               { ip: ip, port: port, op: job.op, address: job.base, count: 1 })
        .then(function (res) {
          var d = res.data;
          var label = (job.op === "read_coils" ? "FC01 coils      " : "FC02 disc inputs");
          lines.push((d.ok ? "OK  " : "--  ") + label + " @ " + job.base +
                     (d.ok ? "  = " + (d.values && d.values[0] ? 1 : 0) : "  " + (d.error || "no response")));
          show(out, lines.join("\n"));
          next();
        })
        .catch(function () {
          lines.push("--  error at " + job.base);
          show(out, lines.join("\n"));
          next();
        });
    })();
  });

  // ---------------------------------------------------------- Serial Master

  var smStatus = document.getElementById("smStatus");

  document.getElementById("smScan").addEventListener("click", function () {
    fetch("/api/serial/ports").then(function (r) { return r.json(); }).then(function (d) {
      var list = document.getElementById("smPortList");
      list.innerHTML = "";
      (d.ports || []).forEach(function (p) {
        var opt = document.createElement("option");
        opt.value = p;
        list.appendChild(opt);
      });
      setBadge(smStatus, (d.ports || []).length ? "ok" : "idle",
               (d.ports || []).length + " port(s) found");
    }).catch(function () {
      setBadge(smStatus, "error", "Scan failed");
    });
  });

  document.getElementById("smSend").addEventListener("click", function () {
    var port = document.getElementById("smPort").value.trim();
    if (!port) {
      setBadge(smStatus, "error", "Port required");
      return;
    }
    var payload = {
      port: port,
      baudrate: parseInt(document.getElementById("smBaud").value, 10) || 9600,
      data_bits: parseInt(document.getElementById("smDataBits").value, 10) || 8,
      stop_bits: parseInt(document.getElementById("smStopBits").value, 10) || 1,
      parity: document.getElementById("smParity").value,
      data: document.getElementById("smData").value,
      hex: document.getElementById("smHex").checked,
      append_cr: document.getElementById("smAppendCr").checked,
      read_ms: parseInt(document.getElementById("smReadMs").value, 10) || 500
    };

    setBadge(smStatus, "idle", "Sending...");
    postJson("/api/test/serial", payload).then(function (res) {
      var d = res.data;
      if (!d.ok) {
        setBadge(smStatus, "error", d.error || "Failed");
        show(document.getElementById("smOutput"), d.error || JSON.stringify(d, null, 2));
        return;
      }
      setBadge(smStatus, "ok", "Sent " + d.sent_bytes + " B, received " + d.received_bytes + " B");
      show(document.getElementById("smOutput"),
           "TX bytes : " + d.sent_bytes + "\n" +
           "RX bytes : " + d.received_bytes + "\n" +
           "RX text  : " + (d.received || "(nothing)") + "\n" +
           "RX hex   : " + (d.received_hex || "(nothing)"));
    }).catch(function (e) {
      setBadge(smStatus, "error", "Request failed");
      show(document.getElementById("smOutput"), String(e));
    });
  });

  // ------------------------------------------ Not-yet-built server/slave tabs

  // These two are simulators: the gateway would have to listen for incoming
  // Modbus TCP connections and answer them from a settable register map, and
  // to hold a COM port open answering a master's polls. Neither exists in the
  // backend today. Saying so plainly beats shipping a panel of dead controls
  // that looks functional until someone depends on it.
  function renderPending(el, title, needs) {
    var wrap = document.createElement("div");

    var badge = document.createElement("span");
    badge.className = "hsf-badge hsf-badge-idle mb-3 d-inline-block";
    badge.textContent = "Not implemented";
    wrap.appendChild(badge);

    var p = document.createElement("p");
    p.className = "text-muted";
    p.textContent = title;
    wrap.appendChild(p);

    var h = document.createElement("p");
    h.className = "mb-1 small fw-semibold";
    h.textContent = "Requires, in the gateway backend:";
    wrap.appendChild(h);

    var ul = document.createElement("ul");
    ul.className = "text-muted small";
    needs.forEach(function (n) {
      var li = document.createElement("li");
      li.textContent = n;
      ul.appendChild(li);
    });
    wrap.appendChild(ul);

    el.innerHTML = "";
    el.appendChild(wrap);
  }

  renderPending(
    document.getElementById("modbusServerBody"),
    "Would make the gateway act as a Modbus TCP slave, so a PLC or SCADA package can poll it " +
    "and so the Modbus TCP Client tab can be exercised without any hardware present.",
    [
      "A listening socket + accept loop (TcpSocket is connect-only today).",
      "A settable coil / discrete-input / holding-register map with a UI to edit it.",
      "Request dispatch for FC01, FC02, FC03, FC05, FC06, FC0F and FC10, including correct exception replies.",
      "A live request log so you can see what the polling master asked for."
    ]
  );

  renderPending(
    document.getElementById("serialSlaveBody"),
    "Would hold a COM port open and answer a serial master's polls with configured responses, " +
    "for testing the other end of a serial link without the real device.",
    [
      "A long-lived SerialPort owned by the test layer (today /api/test/serial opens and closes per request).",
      "A rule table mapping received frames to canned responses.",
      "A live TX/RX trace streamed to this page over the existing WebSocket."
    ]
  );

  // ================================================================ hardware
  //
  // UpdateTestToolPlan.md sections 43-48. Everything below calls
  // /api/test/{zk,relay,aux-relay,beeper,aux-input,button} on the gateway,
  // which drives the LIVE ZkController -- the same session the SmartLocker
  // script uses. Nothing here speaks the ZK protocol; that stays in the driver
  // (section 41).

  var el = document.getElementById.bind(document);

  function num(id, fallback) {
    var value = parseInt(el(id).value, 10);
    return isNaN(value) ? fallback : value;
  }

  // Section 59 asks for these to be admin-only. This hides them from a
  // non-admin session, which is exactly as much as the gateway can currently
  // enforce: web/js/auth.js is a localStorage session and says of itself that
  // it is not security. Anyone who can reach the port can still call the routes
  // directly -- the operation log is what makes that visible.
  function applyAdminGate() {
    var isAdmin = !window.HsfAuth || window.HsfAuth.hasRole("admin");
    var nodes = document.querySelectorAll("[data-hsf-admin]");

    for (var i = 0; i < nodes.length; i++) {
      nodes[i].hidden = !isAdmin;
    }

    if (!isAdmin) {
      var warn = document.createElement("div");
      warn.className = "alert alert-secondary py-2 small mb-3";
      warn.textContent =
        "Signed in as " + (window.HsfAuth.role() || "a non-admin role") +
        ": hardware controls are hidden. Status, RTLog and the operation log stay readable.";
      var host = el("tool-zk");
      if (host) host.insertBefore(warn, host.firstChild);
    }
  }

  // Shared "run one operation and report it" wiring: the routes all answer
  // {ok, error, duration_ms, log}, so one renderer serves every button.
  function runOp(url, payload, badge, out) {
    setBadge(badge, "idle", "…");
    return postJson(url, payload || {}).then(function (res) {
      var data = res.data || {};
      setBadge(badge, data.ok ? "ok" : "error",
                (data.ok ? "OK" : "FAILED") + " · " + (data.duration_ms || 0) + " ms");
      show(out, data.ok ? prettyJson(JSON.stringify(data))
                        : (data.error || "failed") + "\n\n" + prettyJson(JSON.stringify(data)));
      loadTestLog();
      return data;
    }).catch(function (error) {
      setBadge(badge, "error", "error");
      show(out, String(error));
    });
  }

  // ------------------------------------------------------------ ZK status

  function renderZkStatus(data) {
    var badge = el("zkConnBadge");

    if (!data.available) {
      setBadge(badge, "warn", "not in this build");
    } else if (data.connected) {
      setBadge(badge, "ok", "connected");
    } else {
      setBadge(badge, "error", data.reconnecting ? "reconnecting" : "disconnected");
    }

    setBadge(el("zkRtBadge"), data.rtlog_running ? "ok" : "idle",
              data.rtlog_running ? "running" : "stopped");

    show(el("zkStatusOut"), prettyJson(JSON.stringify(data)));
  }

  function loadZkStatus() {
    return fetch("/api/test/zk/status").then(function (r) { return r.json(); })
      .then(renderZkStatus)
      .catch(function (error) { show(el("zkStatusOut"), String(error)); });
  }

  el("zkRefresh").addEventListener("click", loadZkStatus);

  el("zkConnect").addEventListener("click", function () {
    var payload = { timeout_ms: num("zkTimeout", 3000) };
    // Left out entirely when blank, so the gateway falls back to its own
    // configured controller rather than being told to connect to "".
    if (el("zkIp").value.trim()) payload.ip = el("zkIp").value.trim();
    if (el("zkPort").value.trim()) payload.port = num("zkPort", 4370);
    if (el("zkPassword").value) payload.password = el("zkPassword").value;

    runOp("/api/test/zk/connect", payload, el("zkConnBadge"), el("zkStatusOut"))
      .then(loadZkStatus);
  });

  el("zkDisconnect").addEventListener("click", function () {
    runOp("/api/test/zk/disconnect", {}, el("zkConnBadge"), el("zkStatusOut"))
      .then(loadZkStatus);
  });

  // ------------------------------------------------------------ ZK RTLog

  var rtBody = el("zkRtTable").querySelector("tbody");
  var rtRows = 0;

  function addRtRow(row, prepend) {
    var tr = document.createElement("tr");

    [row.time, row.card_no, row.pin, row.door_no,
     row.event_type, row.in_out_status, row.verify_mode].forEach(function (value) {
      var td = document.createElement("td");
      // textContent, not innerHTML: these strings come off a device.
      td.textContent = (value === null || value === undefined) ? "" : String(value);
      tr.appendChild(td);
    });

    if (prepend && rtBody.firstChild) {
      rtBody.insertBefore(tr, rtBody.firstChild);
    } else {
      rtBody.appendChild(tr);
    }

    rtRows += 1;

    // The page keeps a window, not a history -- the gateway holds the tail and
    // /api/test/zk/rtlog serves it.
    while (rtBody.children.length > 300) {
      rtBody.removeChild(rtBody.lastChild);
    }

    el("zkRtEmpty").hidden = true;
    el("zkRtCount").textContent = rtBody.children.length + " record(s) shown";
  }

  function loadRtlog() {
    return fetch("/api/test/zk/rtlog?limit=200").then(function (r) { return r.json(); })
      .then(function (data) {
        rtBody.innerHTML = "";
        rtRows = 0;
        (data.rows || []).slice().reverse().forEach(function (row) { addRtRow(row, false); });
        el("zkRtEmpty").hidden = (data.rows || []).length > 0;
        el("zkRtCount").textContent = rtBody.children.length + " record(s) shown";
        setBadge(el("zkRtBadge"), data.running ? "ok" : "idle", data.running ? "running" : "stopped");
      })
      .catch(function () { /* the status panel already reports a dead gateway */ });
  }

  el("zkRtStart").addEventListener("click", function () {
    runOp("/api/test/zk/rtlog/start", {}, el("zkRtBadge"), el("zkStatusOut")).then(loadZkStatus);
  });

  el("zkRtStop").addEventListener("click", function () {
    runOp("/api/test/zk/rtlog/stop", {}, el("zkRtBadge"), el("zkStatusOut")).then(loadZkStatus);
  });

  el("zkRtClear").addEventListener("click", function () {
    fetch("/api/test/zk/rtlog/clear", { method: "POST" }).then(loadRtlog);
  });

  // ------------------------------------------------------- ZK device control

  el("zkOpenDoor").addEventListener("click", function () {
    runOp("/api/test/zk/door", { door: num("zkDoor", 1), seconds: num("zkDoorSeconds", 3) },
           el("zkDevBadge"), el("zkDevOut"));
  });

  el("zkControl").addEventListener("click", function () {
    runOp("/api/test/zk/device/control", {
      operation: num("zkOp", 1), param1: num("zkP1", 0), param2: num("zkP2", 0),
      param3: num("zkP3", 0), param4: num("zkP4", 0)
    }, el("zkDevBadge"), el("zkDevOut"));
  });

  el("zkGetParams").addEventListener("click", function () {
    var items = encodeURIComponent(el("zkParamItems").value.trim());
    setBadge(el("zkDevBadge"), "idle", "…");
    fetch("/api/test/zk/device/params?items=" + items)
      .then(function (r) { return r.json(); })
      .then(function (data) {
        setBadge(el("zkDevBadge"), data.ok ? "ok" : "error", data.ok ? "OK" : "no values");
        show(el("zkDevOut"), prettyJson(JSON.stringify(data)));
      })
      .catch(function (error) {
        setBadge(el("zkDevBadge"), "error", "error");
        show(el("zkDevOut"), String(error));
      });
  });

  // ------------------------------------------------------------ relays

  function relayPayload() {
    return { relay: num("rlNumber", 1), duration_ms: num("rlDuration", 200) };
  }

  function relayUrl(action) {
    return "/api/test/" + el("rlKind").value + "/" + action;
  }

  el("rlOn").addEventListener("click", function () {
    runOp(relayUrl("on"), relayPayload(), el("rlBadge"), el("rlOut"));
  });

  el("rlOff").addEventListener("click", function () {
    runOp(relayUrl("off"), relayPayload(), el("rlBadge"), el("rlOut"));
  });

  el("rlPulse").addEventListener("click", function () {
    runOp(relayUrl("pulse"), relayPayload(), el("rlBadge"), el("rlOut"));
  });

  el("rlList").addEventListener("click", function () {
    setBadge(el("rlBadge"), "idle", "…");
    fetch("/api/test/relay/list").then(function (r) { return r.json(); })
      .then(function (data) {
        setBadge(el("rlBadge"), data.ok ? "ok" : "error", data.ok ? "OK" : "failed");
        show(el("rlOut"), prettyJson(JSON.stringify(data)));
      })
      .catch(function (error) {
        setBadge(el("rlBadge"), "error", "error");
        show(el("rlOut"), String(error));
      });
  });

  // ------------------------------------------------------------ beeper

  el("bpBeep").addEventListener("click", function () {
    runOp("/api/test/beeper", {
      relay: num("bpNumber", 1),
      auxiliary: el("bpKind").value === "aux",
      count: num("bpCount", 2),
      on_ms: num("bpOn", 150),
      gap_ms: num("bpGap", 150)
    }, el("bpBadge"), el("bpOut"));
  });

  // ------------------------------------------------------------ inputs

  var inBody = el("inTable").querySelector("tbody");

  function stampOf(seconds) {
    if (!seconds) return "";
    var d = new Date(seconds * 1000);
    return d.toLocaleString();
  }

  function loadInputs() {
    return fetch("/api/test/aux-input/status").then(function (r) { return r.json(); })
      .then(function (data) {
        inBody.innerHTML = "";

        (data.inputs || []).forEach(function (row) {
          var tr = document.createElement("tr");
          [String(row.input), row.state, stampOf(row.changed_at)].forEach(function (value) {
            var td = document.createElement("td");
            td.textContent = value;
            tr.appendChild(td);
          });
          inBody.appendChild(tr);
        });

        var count = (data.inputs || []).length;
        el("inEmpty").hidden = count > 0;
        setBadge(el("inBadge"), count > 0 ? "ok" : "idle",
                  count + " reporting of " + (data.configured || 0) + " configured");
      })
      .catch(function () { /* status panel covers a dead gateway */ });
  }

  el("inRefresh").addEventListener("click", loadInputs);

  fetch("/api/test/button/status").then(function (r) { return r.json(); })
    .then(function (data) { el("btnNote").textContent = data.note || ""; })
    .catch(function () { el("btnNote").textContent = "Could not read button support."; });

  // ------------------------------------------------------------ test log

  var tlBody = el("tlTable").querySelector("tbody");

  function renderTestLog(rows) {
    tlBody.innerHTML = "";

    rows.slice().reverse().forEach(function (row) {
      var tr = document.createElement("tr");
      var cells = [
        row.timestamp, row.module, row.operation, row.result,
        (row.duration_ms || 0) + " ms",
        JSON.stringify(row.parameters || {}),
        row.error || ""
      ];

      cells.forEach(function (value, index) {
        var td = document.createElement("td");
        td.textContent = value;
        if (index === 3) td.className = row.result === "SUCCESS" ? "text-success" : "text-danger";
        tr.appendChild(td);
      });

      tlBody.appendChild(tr);
    });

    el("tlEmpty").hidden = rows.length > 0;
  }

  function loadTestLog() {
    return fetch("/api/test/log").then(function (r) { return r.json(); })
      .then(function (data) { renderTestLog(data.rows || []); })
      .catch(function () { /* nothing to show */ });
  }

  el("tlRefresh").addEventListener("click", loadTestLog);

  el("tlClear").addEventListener("click", function () {
    fetch("/api/test/log", { method: "DELETE" }).then(loadTestLog);
  });

  // ------------------------------------------------------------ realtime
  //
  // Section 58: hardware events reach the page when they happen, not on a
  // poll. The shared /ws channel already exists; these are three more frame
  // types on it.
  if (window.HsfWs) {
    window.HsfWs.onMessage(function (payload) {
      if (!payload || !payload.type) return;

      if (payload.type === "zk.rtlog") {
        addRtRow(payload.data || {}, true);
      } else if (payload.type === "zk.aux_input") {
        loadInputs();
      } else if (payload.type === "test.log") {
        loadTestLog();
      }
    });
  }

  // ------------------------------------------------------------ RabbitMQ
  //
  // Sections 49-55. Every call opens its own broker connection on the gateway
  // side, so nothing here competes with the live client for its own inbox.

  // Blank stays blank: the gateway falls back to its configured broker rather
  // than being told to connect to "".
  function mqConnection() {
    var payload = {};
    if (el("mqHost").value.trim()) payload.host = el("mqHost").value.trim();
    if (el("mqPort").value.trim()) payload.port = num("mqPort", 5672);
    if (el("mqUser").value.trim()) payload.user = el("mqUser").value.trim();
    if (el("mqPassword").value) payload.password = el("mqPassword").value;
    if (el("mqVhost").value.trim()) payload.vhost = el("mqVhost").value.trim();
    return payload;
  }

  function mqPayload(extra) {
    var payload = mqConnection();

    payload.exchange = el("mqExchange").value.trim();
    payload.exchange_type = el("mqExchangeType").value;
    payload.queue = el("mqQueue").value.trim();
    payload.routing_key = el("mqRoutingKey").value.trim();
    payload.durable = el("mqDurable").checked;
    payload.auto_delete = el("mqAutoDelete").checked;
    payload.exclusive = el("mqExclusive").checked;

    Object.keys(extra || {}).forEach(function (key) { payload[key] = extra[key]; });
    return payload;
  }

  function loadMqStatus() {
    return fetch("/api/test/rabbitmq/status").then(function (r) { return r.json(); })
      .then(function (data) {
        var live = data.live || {};
        setBadge(el("mqConnBadge"), live.connected ? "ok" : (live.configured ? "error" : "idle"),
                  live.connected ? "gateway connected"
                                 : (live.configured ? "gateway disconnected" : "mq disabled"));
        show(el("mqStatusOut"), prettyJson(JSON.stringify(data)));

        // Prefills from the gateway's own topology, so the buttons act on what
        // this deployment actually uses unless the operator says otherwise.
        var topo = data.topology || {};
        if (!el("mqExchange").value) el("mqExchange").value = topo.exchange || "";
        if (!el("mqQueue").value) el("mqQueue").value = topo.queue || "";
        if (!el("mqRoutingKey").value) el("mqRoutingKey").value = topo.routing_key || "";
        if (topo.exchange_type) el("mqExchangeType").value = topo.exchange_type;
      })
      .catch(function (error) { show(el("mqStatusOut"), String(error)); });
  }

  el("mqStatus").addEventListener("click", loadMqStatus);

  el("mqConnect").addEventListener("click", function () {
    runOp("/api/test/rabbitmq/connect", mqConnection(), el("mqConnBadge"), el("mqStatusOut"))
      .then(loadMqStatus);
  });

  el("mqCreateExchange").addEventListener("click", function () {
    runOp("/api/test/rabbitmq/exchange/create", mqPayload(), el("mqTopoBadge"), el("mqTopoOut"));
  });

  el("mqCreateQueue").addEventListener("click", function () {
    runOp("/api/test/rabbitmq/queue/create", mqPayload(), el("mqTopoBadge"), el("mqTopoOut"));
  });

  el("mqBind").addEventListener("click", function () {
    runOp("/api/test/rabbitmq/bind", mqPayload(), el("mqTopoBadge"), el("mqTopoOut"));
  });

  el("mqPublish").addEventListener("click", function () {
    runOp("/api/test/rabbitmq/publish", mqPayload({ body: el("mqBody").value }),
           el("mqPubBadge"), el("mqPubOut"));
  });

  // Section 55's two scenarios, verbatim. Filling them in is not the test --
  // the test is whether the gateway's own consumer picks the message up and
  // the locker state changes, which is watched on the Dashboard and the
  // SmartLocker page.
  var SCENARIOS = {
    employee: {
      routing_key: "employee.card.created",
      body: {
        event: "employee.card.created",
        data: {
          username: "TEST USER", card_code: "TEST0001", role: "employee",
          gender: "male", expire_at: null, active: true
        }
      }
    },
    contractor: {
      routing_key: "employee.card.created",
      body: {
        event: "employee.card.created",
        data: {
          username: "TEST CONTRACTOR", card_code: "TEST0002", role: "contractor",
          gender: "female", expire_at: "2026-12-31", active: true
        }
      }
    }
  };

  function fillScenario(name) {
    var scenario = SCENARIOS[name];
    el("mqRoutingKey").value = scenario.routing_key;
    el("mqBody").value = JSON.stringify(scenario.body, null, 2);
  }

  el("mqFillEmployee").addEventListener("click", function () { fillScenario("employee"); });
  el("mqFillContractor").addEventListener("click", function () { fillScenario("contractor"); });

  var mqMsgBody = el("mqMsgTable").querySelector("tbody");

  function runConsume(ack) {
    var payload = mqPayload({
      ack: ack,
      max_messages: num("mqMax", 10),
      timeout_ms: num("mqTimeout", 3000)
    });

    setBadge(el("mqConsBadge"), "idle", "…");

    return postJson("/api/test/rabbitmq/consume", payload).then(function (res) {
      var data = res.data || {};
      var messages = data.messages || [];

      mqMsgBody.innerHTML = "";
      messages.forEach(function (message) {
        var tr = document.createElement("tr");
        [message.exchange, message.routing_key, message.message_id,
         message.redelivered ? "yes" : "", message.body].forEach(function (value) {
          var td = document.createElement("td");
          // textContent: a message body is whatever a publisher sent.
          td.textContent = value === null || value === undefined ? "" : String(value);
          tr.appendChild(td);
        });
        mqMsgBody.appendChild(tr);
      });

      el("mqMsgEmpty").hidden = messages.length > 0;
      el("mqMsgEmpty").textContent = data.ok
        ? "Nothing received — the queue is empty."
        : (data.error || "Consume failed.");

      setBadge(el("mqConsBadge"), data.ok ? (messages.length ? "ok" : "idle") : "error",
                data.ok ? messages.length + " message(s)" : "failed");

      loadTestLog();
      return data;
    }).catch(function (error) {
      setBadge(el("mqConsBadge"), "error", "error");
      el("mqMsgEmpty").textContent = String(error);
    });
  }

  el("mqPeek").addEventListener("click", function () { runConsume(false); });
  el("mqConsume").addEventListener("click", function () { runConsume(true); });

  // -------------------------------------------------- SmartLocker scenario
  //
  // Sections 55 and 60. The publish is the easy half; what this panel is for is
  // the other half -- did the gateway decode it, store it and hand out a
  // locker. Each step is checked against the SmartLocker database rather than
  // inferred from a successful publish.

  var scBody = el("scStepTable").querySelector("tbody");

  function renderSteps(steps) {
    scBody.innerHTML = "";

    (steps || []).forEach(function (step) {
      var tr = document.createElement("tr");

      var mark = document.createElement("td");
      mark.textContent = step.ok ? "✓" : "✗";
      mark.className = step.ok ? "text-success fw-bold" : "text-danger fw-bold";
      tr.appendChild(mark);

      [step.step, step.detail || ""].forEach(function (value) {
        var td = document.createElement("td");
        td.textContent = value;
        tr.appendChild(td);
      });

      scBody.appendChild(tr);
    });
  }

  // Built from the RabbitMQ tab's scenario payloads so there is one definition
  // of what an "employee created" event looks like, not two that drift.
  function scenarioPayload() {
    var role = el("scRole").value;
    var card = el("scCard").value.trim();
    var scenario = JSON.parse(JSON.stringify(SCENARIOS[role]));

    scenario.body.data.card_code = card;

    var payload = mqPayload({
      body: JSON.stringify(scenario.body),
      // The exchange and routing key stay whatever the RabbitMQ tab says: they
      // have to match this gateway's binding or the message never arrives.
      card_code: card,
      wait_ms: num("scWait", 8000)
    });

    return payload;
  }

  function renderVerify(data) {
    var steps = data.steps || {};
    renderSteps([
      { step: "Card in the database", ok: !!steps.received,
        detail: data.employee ? (data.employee.username || "") : "not found" },
      { step: "Locker assigned", ok: !!steps.assigned,
        detail: data.locker ? ("locker " + data.locker.locker_number + " (" + data.locker.status + ")")
                            : "no locker holds this card" }
    ]);
    show(el("scOut"), prettyJson(JSON.stringify(data)));
    setBadge(el("scBadge"), steps.assigned ? "ok" : (steps.received ? "warn" : "idle"),
              steps.assigned ? "assigned" : (steps.received ? "known, no locker" : "not found"));
  }

  el("scVerify").addEventListener("click", function () {
    var card = encodeURIComponent(el("scCard").value.trim());
    setBadge(el("scBadge"), "idle", "…");
    fetch("/api/test/scenario/verify?card=" + card)
      .then(function (r) { return r.json(); })
      .then(renderVerify)
      .catch(function (error) {
        setBadge(el("scBadge"), "error", "error");
        show(el("scOut"), String(error));
      });
  });

  el("scRun").addEventListener("click", function () {
    setBadge(el("scBadge"), "idle", "running…");
    renderSteps([{ step: "Publishing and waiting for the chain…", ok: true, detail: "" }]);

    postJson("/api/test/scenario/run", scenarioPayload()).then(function (res) {
      var data = res.data || {};
      renderSteps(data.steps);
      setBadge(el("scBadge"), data.ok ? "ok" : "error",
                (data.ok ? "complete" : "incomplete") + " · " + (data.duration_ms || 0) + " ms");
      show(el("scOut"), prettyJson(JSON.stringify(data)));
      loadTestLog();
    }).catch(function (error) {
      setBadge(el("scBadge"), "error", "error");
      show(el("scOut"), String(error));
    });
  });

  el("scRelease").addEventListener("click", function () {
    var card = encodeURIComponent(el("scCard").value.trim());
    setBadge(el("scReleaseBadge"), "idle", "…");

    // Verified first, because the release route wants a locker id and the
    // whole point of this button is that the operator does not have to know it.
    fetch("/api/test/scenario/verify?card=" + card)
      .then(function (r) { return r.json(); })
      .then(function (data) {
        if (!data.locker) {
          setBadge(el("scReleaseBadge"), "idle", "no locker to release");
          return null;
        }
        // Through the SmartLocker script's own route, so the state machine that
        // owns the door is the thing that releases it.
        return fetch("/api/app/lockers/" + data.locker.id + "/release?source=test-tool",
                      { method: "POST" }).then(function (r) { return r.json(); });
      })
      .then(function (result) {
        if (!result) return;
        setBadge(el("scReleaseBadge"), result.ok ? "ok" : "error",
                  result.ok ? "released" : (result.error || "failed"));
      })
      .catch(function (error) {
        setBadge(el("scReleaseBadge"), "error", String(error));
      });
  });

  applyAdminGate();
  loadZkStatus();
  loadRtlog();
  loadInputs();
  loadTestLog();
  loadMqStatus();
})();
