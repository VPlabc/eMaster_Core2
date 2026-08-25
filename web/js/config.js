(function () {
  const form = document.getElementById("configForm");
  const alertBox = document.getElementById("saveAlert");

  function setByPath(obj, path, value) {
    const parts = path.split(".");
    let node = obj;
    for (let i = 0; i < parts.length - 1; i++) {
      node[parts[i]] = node[parts[i]] || {};
      node = node[parts[i]];
    }
    node[parts[parts.length - 1]] = value;
  }

  function getByPath(obj, path) {
    return path.split(".").reduce((node, key) => (node ? node[key] : undefined), obj);
  }

  function ensureOption(selectEl, value) {
    const stringValue = String(value);
    const exists = Array.from(selectEl.options).some((o) => o.value === stringValue);
    if (!exists && stringValue !== "") {
      const option = document.createElement("option");
      option.value = stringValue;
      option.textContent = stringValue;
      selectEl.appendChild(option);
    }
  }

  function populateForm(config) {
    form.querySelectorAll("[name]").forEach((input) => {
      const value = getByPath(config, input.name);
      if (value === undefined) return;
      if (input.type === "checkbox") {
        input.checked = !!value;
      } else if (input.tagName === "SELECT") {
        // The serial port list starts empty (populated by "Scan"), so make
        // sure whatever is already saved in config.json shows up as an
        // option too, instead of silently defaulting to the first entry.
        ensureOption(input, value);
        input.value = value;
      } else {
        input.value = value;
      }
    });
  }

  function showAlert(message, ok) {
    alertBox.textContent = message;
    alertBox.className = "alert " + (ok ? "alert-success" : "alert-danger");
    if (window.HsfNotify) window.HsfNotify(message, ok);
  }

  function currentFormValues() {
    const values = {};
    form.querySelectorAll("[name]").forEach((input) => {
      let value = input.type === "checkbox" ? input.checked : input.value;
      if (input.type === "number") value = Number(value);
      setByPath(values, input.name, value);
    });
    return values;
  }

  function setResult(elementId, message, ok) {
    const el = document.getElementById(elementId);
    el.textContent = message;
    el.className = "small " + (ok ? "text-success" : "text-danger");
  }

  function postJson(url, body) {
    return fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) })
      .then((r) => r.json());
  }

  fetch("/api/config")
    .then((r) => r.json())
    .then((config) => {
      populateForm(config);
      // Relabels the RFID fields for whichever method was saved; the labels
      // are wrong for two of the three modes until this runs.
      applyRfidMode();
    })
    .catch(() => showAlert("Failed to load configuration", false));

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    postJson("/api/config", currentFormValues())
      .then(() => showAlert("Configuration saved.", true))
      .catch(() => showAlert("Failed to save configuration.", false));
  });

  document.getElementById("exportConfig").addEventListener("click", () => {
    fetch("/api/config/export").then((r) => r.json()).then((config) => {
      const blob = new Blob([JSON.stringify(config, null, 2)], { type: "application/json" });
      const link = document.createElement("a");
      link.href = URL.createObjectURL(blob);
      link.download = "hsf-config.json";
      link.click();
      URL.revokeObjectURL(link.href);
    });
  });

  document.getElementById("importConfig").addEventListener("change", (event) => {
    const file = event.target.files[0];
    if (!file) return;
    const result = document.getElementById("configFileResult");
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const imported = JSON.parse(reader.result);
        postJson("/api/config/import", imported).then((response) => {
          result.textContent = response.error ? response.error : "Configuration imported.";
          if (!response.error) fetch("/api/config").then((r) => r.json()).then(populateForm);
        });
      } catch (_) { result.textContent = "Invalid JSON file."; }
    };
    reader.readAsText(file);
  });

  // Shared by both serial ports -- the LED display on Serial 2 needs exactly
  // the same scan/try-open affordances as the reader on Serial 1, and two
  // copies of this would drift.
  function wireSerialScan(buttonId, selectId, resultId) {
    document.getElementById(buttonId).addEventListener("click", (event) => {
      event.preventDefault();
      const resultEl = document.getElementById(resultId);
      const select = document.getElementById(selectId);
      resultEl.textContent = "Scanning...";
      resultEl.className = "small text-muted";

      fetch("/api/serial/ports")
        .then((r) => r.json())
        .then((data) => {
          const ports = data.ports || [];
          const previousValue = select.value;

          select.innerHTML = "";
          ports.forEach((port) => {
            const option = document.createElement("option");
            option.value = port;
            option.textContent = port;
            select.appendChild(option);
          });

          // Keep whatever was selected before scanning even if this scan
          // didn't detect it (e.g. a device that's temporarily unplugged),
          // rather than silently discarding it.
          if (previousValue && !ports.includes(previousValue)) {
            const option = document.createElement("option");
            option.value = previousValue;
            option.textContent = previousValue + " (not detected)";
            select.appendChild(option);
          }
          if (previousValue) select.value = previousValue;

          resultEl.textContent = ports.length ? `Found ${ports.length} port(s).` : "No serial ports found.";
          resultEl.className = "small " + (ports.length ? "text-success" : "text-muted");
        })
        .catch(() => {
          resultEl.textContent = "Scan failed.";
          resultEl.className = "small text-danger";
        });
    });
  }

  // `section` picks which half of the form is sent, so Serial 2 is tested
  // with its own settings rather than Serial 1's.
  function wireSerialTryOpen(buttonId, resultId, section) {
    document.getElementById(buttonId).addEventListener("click", (event) => {
      event.preventDefault();
      setResult(resultId, "Opening...", true);
      const values = currentFormValues();
      postJson("/api/serial/test", values[section] || {})
        .then((res) => {
          // `detail` is set when the gateway already holds the port open — a
          // pass, but for a different reason than "we just opened it", and the
          // operator needs to be able to tell those apart.
          const message = res.ok ? res.detail || "Opened successfully." : `Failed: ${res.error}`;
          setResult(resultId, message, res.ok);
        })
        .catch(() => setResult(resultId, "Test request failed.", false));
    });
  }

  wireSerialScan("btnScanSerial", "serialPortSelect", "serialScanResult");
  wireSerialTryOpen("btnTryOpenSerial", "serialOpenResult", "serial");

  wireSerialScan("btnScanSerial2", "serial2PortSelect", "serial2ScanResult");
  wireSerialTryOpen("btnTryOpenSerial2", "serial2OpenResult", "serial2");

  document.getElementById("btnSendLedTest").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("ledTestResult", "Sending...", true);
    const values = currentFormValues();
    const payload = Object.assign({}, values.serial2 || {}, {
      text: document.getElementById("ledTestText").value,
    });
    postJson("/api/serial2/led-test", payload)
      .then((res) => {
        if (res.ok) {
          setResult("ledTestResult", `Sent "${res.sent}" (${res.bytes} bytes). Check the display.`, true);
        } else {
          setResult("ledTestResult", `Failed: ${res.error}`, false);
        }
      })
      .catch(() => setResult("ledTestResult", "Test request failed.", false));
  });

  document.getElementById("btnTestRest").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("restTestResult", "Testing...", true);
    const values = currentFormValues();
    postJson("/api/rest/test", values.rest || {})
      .then((res) => {
        setResult("restTestResult", res.ok ? `OK (HTTP ${res.status_code})` : `Failed: ${res.error || res.status_code}`, res.ok);
      })
      .catch(() => setResult("restTestResult", "Test request failed.", false));
  });

  document.getElementById("btnTestModbus").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("modbusTestResult", "Testing...", true);
    const values = currentFormValues();
    postJson("/api/modbus/test", values.modbus || {})
      .then((res) => {
        setResult("modbusTestResult", res.ok ? "Connected." : `Failed: ${res.error}`, res.ok);
      })
      .catch(() => setResult("modbusTestResult", "Test request failed.", false));
  });

  document.getElementById("btnTestMq").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("mqTestResult", "Connecting...", true);
    const values = currentFormValues();
    postJson("/api/mq/test", values.mq || {})
      .then((res) => {
        setResult("mqTestResult", res.ok ? "Handshake completed." : `Failed: ${res.error}`, res.ok);
      })
      .catch(() => setResult("mqTestResult", "Test request failed.", false));
  });

  // Deliberately sends nothing from the form: this goes through the gateway's
  // live client, so it proves the SAVED settings work end to end. Saving first
  // is the point, not an oversight.
  document.getElementById("btnMqPublish").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("mqTestResult", "Publishing...", true);
    postJson("/api/mq/publish", { body: "eMaster Gateway test message" })
      .then((res) => {
        setResult("mqTestResult", res.ok ? "Message published." : `Failed: ${res.error}`, res.ok);
      })
      .catch(() => setResult("mqTestResult", "Publish request failed.", false));
  });

  // The same three fields mean different things per method, so relabel them
  // rather than leaving "Reconnect Interval" on a mode that never connects.
  const RFID_MODES = {
    tcp_json: {
      help: "Connects to a TCP server that sends one JSON object per read; the card value is taken from data.Raw. Status = this connection being up.",
      ip: "Server IP",
      port: "Server Port",
      interval: "Reconnect Interval (ms)",
    },
    zk: {
      help: "Drives a ZKTeco controller over PullSDK, with its own 5s heartbeat and auto-reconnect. Status = that controller connection.",
      ip: "Controller IP",
      port: "Controller Port (usually 4370)",
      interval: "Status Poll Interval (ms)",
    },
    card_api: {
      help: "Readers POST to /api/card/input, so there is no outbound connection. Status comes from pinging the reader IP (falling back to a TCP connect on the port, since many networks drop ICMP).",
      ip: "Reader IP (ping target)",
      port: "Ping Fallback Port",
      interval: "Ping Interval (ms)",
    },
  };

  function applyRfidMode() {
    const select = document.getElementById("rfidMode");
    if (!select) return;
    const mode = RFID_MODES[select.value] || RFID_MODES.tcp_json;
    document.getElementById("rfidModeHelp").textContent = mode.help;
    document.getElementById("rfidIpLabel").textContent = mode.ip;
    document.getElementById("rfidPortLabel").textContent = mode.port;
    document.getElementById("rfidIntervalLabel").textContent = mode.interval;
  }

  document.getElementById("rfidMode").addEventListener("change", applyRfidMode);

  document.getElementById("btnTestRfid").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("rfidTestResult", "Testing...", true);
    const values = currentFormValues();
    postJson("/api/rfid/test", values.rfid || {})
      .then((res) => {
        if (!res.connected) {
          setResult("rfidTestResult", res.detail || "Failed to connect.", false);
        } else if (res.raw) {
          // The decoded value is the useful confirmation: bytes arriving
          // proves nothing if data.Raw isn't where we expect it.
          setResult("rfidTestResult", `Connected. Card value (data.Raw): ${res.raw}`, true);
        } else if (res.data) {
          setResult("rfidTestResult", `Connected, but no data.Raw in the payload: ${res.data}`, false);
        } else {
          setResult("rfidTestResult", res.detail || "Connected. No data received (idle reader).", true);
        }
      })
      .catch(() => setResult("rfidTestResult", "Test request failed.", false));
  });

  document.getElementById("btnGenerateCardKey").addEventListener("click", (event) => {
    event.preventDefault();
    const nameInput = document.getElementById("quickClientName");
    const name = nameInput.value.trim() || "Default Client";
    const keyBox = document.getElementById("cardKeyBox");
    const keyValue = document.getElementById("cardKeyValue");
    keyBox.classList.add("d-none");
    setResult("cardKeyResult", "Generating...", true);

    postJson("/api/card/clients", { name })
      .then((client) => {
        if (client.error) {
          setResult("cardKeyResult", client.error, false);
          return;
        }
        // The key goes in its own read-only field (not embedded in this
        // sentence) so copying it can't accidentally grab surrounding text
        // and send a mismatched key as the API-Key header.
        setResult("cardKeyResult", `Saved. API-Key for "${client.name}":`, true);
        keyValue.value = client.api_key;
        keyBox.classList.remove("d-none");
        nameInput.value = "";
      })
      .catch(() => setResult("cardKeyResult", "Failed to generate API-Key.", false));
  });

  document.getElementById("btnCopyCardKey").addEventListener("click", (event) => {
    event.preventDefault();
    const keyValue = document.getElementById("cardKeyValue");
    keyValue.select();
    navigator.clipboard.writeText(keyValue.value).catch(() => document.execCommand("copy"));
  });

  /* ---- Software Update ---------------------------------------------
   *
   * Only the settings and a one-line summary live here. The offer itself is
   * update.js's corner popup, which is on every page -- duplicating the
   * Update button here would give two things the power to start an install
   * and no way to tell which one the operator used.
   */
  const updateStatusBox = document.getElementById("updateStatusBox");

  function describeUpdate(status) {
    if (!status.enabled) return "Over-the-air updates are switched off.";

    // Worth saying loudly: signature checking is on, but this build has no
    // crypto backend, so every package will be rejected. Silently failing at
    // download time is a far worse way to learn that.
    if (status.require_signature && /not compiled in/.test(status.signature_backend || "")) {
      return (
        "This build cannot verify signatures (" + status.signature_backend +
        "), and signatures are required -- no update will install. Rebuild with OpenSSL available."
      );
    }
    if (!status.managed) {
      return "Running from " + (status.managed_reason || "an unmanaged layout") +
             " -- updates can be checked for, but not installed.";
    }
    if (status.rolled_back_from) {
      return "Version " + status.rolled_back_from + " failed to start and was rolled back to " +
             status.current_version + ".";
    }
    if (status.available) {
      return "Version " + status.version + " is available" + (status.mandatory ? " (required)" : "") +
             ". Current: " + status.current_version + ".";
    }
    if (status.state === "error") return "Last check failed: " + status.error;
    return "Up to date (" + status.current_version + ", " + status.platform + ")" +
           (status.last_check ? ", last checked " + status.last_check : "") + ".";
  }

  function showUpdateStatus(status) {
    if (!updateStatusBox) return;
    updateStatusBox.textContent = describeUpdate(status);
    const bad = status.state === "error" ||
                (status.enabled && status.require_signature && /not compiled in/.test(status.signature_backend || ""));
    updateStatusBox.className = "alert py-2 px-3 small " +
      (bad ? "alert-warning" : status.available ? "alert-info" : "alert-secondary");
  }

  function refreshUpdateStatus() {
    return fetch("/api/update/status").then((r) => r.json()).then(showUpdateStatus).catch(() => {});
  }

  refreshUpdateStatus();
  if (window.HsfWs) {
    window.HsfWs.onMessage((payload) => {
      if (payload && payload.type === "update") showUpdateStatus(payload);
    });
  }

  document.getElementById("btnCheckUpdate").addEventListener("click", (event) => {
    event.preventDefault();
    setResult("updateCheckResult", "Checking...", true);
    fetch("/api/update/check", { method: "POST" })
      .then((r) => r.json())
      .then((res) => {
        if (res.error) {
          setResult("updateCheckResult", res.error, false);
          return;
        }
        // The check runs on the update thread; the answer arrives over the
        // WebSocket. Poll once shortly after as a fallback for a page whose
        // socket is down.
        setResult("updateCheckResult", "Check requested.", true);
        setTimeout(refreshUpdateStatus, 3000);
      })
      .catch(() => setResult("updateCheckResult", "Could not reach the gateway.", false));
  });
})();
