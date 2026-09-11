(function () {
  "use strict";
  const result = document.getElementById("configResult");
  const editor = document.getElementById("configEditor");
  const admin = window.HsfAuth && window.HsfAuth.hasRole("admin");
  const show = (message, ok) => { result.textContent = message; result.className = "small align-self-center " + (ok ? "text-success" : "text-danger"); };
  fetch("/api/status").then((r) => r.json()).then((data) => { document.getElementById("configStatus").textContent = data.status === "ok" ? "Valid" : (data.status || "Unknown"); document.getElementById("configVersion").textContent = data.version || "—"; }).catch(() => { document.getElementById("configStatus").textContent = "Unavailable"; });
  if (!admin) return;
  document.getElementById("configAdminCard").hidden = false;
  fetch("/api/config").then((r) => r.json()).then((data) => { editor.value = JSON.stringify(data, null, 2); });
  document.getElementById("validateConfig").addEventListener("click", () => { try { JSON.parse(editor.value); show("Configuration JSON is valid.", true); } catch (e) { show("Invalid JSON: " + e.message, false); } });
  document.getElementById("saveConfig").addEventListener("click", () => { try { const body = JSON.parse(editor.value); fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }).then((r) => r.json()).then((data) => show(data.error || "Configuration saved and applied.", !data.error)); } catch (e) { show("Invalid JSON: " + e.message, false); } });
  document.getElementById("exportConfig").addEventListener("click", () => { fetch("/api/config/export").then((r) => r.json()).then((config) => { const link = document.createElement("a"); link.href = URL.createObjectURL(new Blob([JSON.stringify(config, null, 2)], { type: "application/json" })); link.download = "hsf-config.json"; link.click(); show("Configuration exported.", true); }).catch(() => show("Export failed.", false)); });
  document.getElementById("importConfig").addEventListener("change", (event) => { const file = event.target.files[0]; if (!file) return; const reader = new FileReader(); reader.onload = () => { try { editor.value = JSON.stringify(JSON.parse(reader.result), null, 2); show("Imported into editor. Validate, then Save / Apply.", true); } catch (_) { show("Invalid JSON file.", false); } }; reader.readAsText(file); });
})();
