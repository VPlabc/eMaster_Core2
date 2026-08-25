(function () {
  const result = document.getElementById("configResult");
  document.getElementById("exportConfig").addEventListener("click", () => {
    fetch("/api/config/export").then((r) => r.json()).then((config) => {
      const blob = new Blob([JSON.stringify(config, null, 2)], { type: "application/json" });
      const link = document.createElement("a"); link.href = URL.createObjectURL(blob); link.download = "hsf-config.json"; link.click();
      result.textContent = "Configuration exported.";
    }).catch(() => { result.textContent = "Export failed."; });
  });
  document.getElementById("importConfig").addEventListener("change", (event) => {
    const file = event.target.files[0]; if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      try {
        fetch("/api/config/import", { method: "POST", headers: { "Content-Type": "application/json" }, body: reader.result })
          .then((r) => r.json()).then((data) => { result.textContent = data.error || "Configuration imported."; });
      } catch (_) { result.textContent = "Invalid JSON file."; }
    };
    reader.readAsText(file);
  });
})();
