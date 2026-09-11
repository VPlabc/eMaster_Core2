(function () {
  const root = document.querySelector("[data-dynamic-config]");
  if (!root) return;
  const projectList = document.getElementById("projectList");
  const fields = document.getElementById("configFields");
  const title = document.getElementById("configTitle");
  const status = document.getElementById("configStatus");
  let project = null, schema = null, saved = {}, savedSnapshot = "{}";

  function request(url, options) {
    return fetch(url, options).then(async r => { const data = await r.json(); if (!r.ok) { const error = new Error(data.error || (data.errors && JSON.stringify(data.errors)) || "Request failed"); error.data = data; throw error; } return data; });
  }
  function fieldList(node) { return (node.fields || []).concat((node.groups || []).flatMap(fieldList)); }
  function inputFor(field, value) {
    const wrap = document.createElement("div"); wrap.className = "col-md-6";
    const label = document.createElement("label"); label.className = "form-label"; label.textContent = field.label || field.key; label.htmlFor = "cfg-" + field.key; wrap.appendChild(label);
    let input;
    if (field.type === "boolean") { input = document.createElement("input"); input.type = "checkbox"; input.className = "form-check-input ms-2"; input.checked = value === true; label.appendChild(input); }
    else if (field.type === "select") { input = document.createElement("select"); input.className = "form-select"; (field.options || []).forEach(o => { const option = new Option(o.label || o.value, o.value); option.selected = value === o.value; input.add(option); }); }
    else if (field.type === "textarea") { input = document.createElement("textarea"); input.className = "form-control"; input.rows = 4; input.value = value == null ? "" : value; }
    else { input = document.createElement("input"); input.className = "form-control"; input.type = field.type === "number" ? "number" : (field.type === "password" || field.type === "secret" ? "password" : "text"); input.value = value == null ? "" : value; if (field.min != null) input.min = field.min; if (field.max != null) input.max = field.max; if (field.step != null) input.step = field.step; }
    input.id = "cfg-" + field.key; input.dataset.key = field.key; input.dataset.type = field.type || "string"; if (field.required) input.required = true; input.addEventListener("input", () => { input.classList.remove("is-invalid"); const feedback = wrap.querySelector(".invalid-feedback"); if (feedback) feedback.remove(); updateDirty(); }); wrap.appendChild(input);
    if (field.unit) { const small = document.createElement("small"); small.className = "text-muted"; small.textContent = field.unit; wrap.appendChild(small); }
    return wrap;
  }
  function render() { fields.innerHTML = ""; if (!schema) return; title.textContent = schema.title || project + " Configuration"; (schema.groups || [{ fields: schema.fields || [] }]).forEach(group => { if (group.label) { const h = document.createElement("h6"); h.className = "col-12 border-bottom pt-2"; h.textContent = group.label; fields.appendChild(h); } fieldList(group).forEach(f => fields.appendChild(inputFor(f, saved[f.key]))); }); updateDirty(); }
  function valuesFromForm() { const result = {}; fields.querySelectorAll("[data-key]").forEach(input => { let v = input.type === "checkbox" ? input.checked : input.value; if (input.dataset.type === "number" && v !== "") v = Number(v); result[input.dataset.key] = v; }); return result; }
  function updateDirty() { if (!schema) return; const dirty = JSON.stringify(valuesFromForm()) !== savedSnapshot; status.textContent = dirty ? "Unsaved changes" : "Saved"; }
  function setFieldErrors(raw) { fields.querySelectorAll("[data-key]").forEach(input => { input.classList.remove("is-invalid"); const feedback = input.parentElement.querySelector(".invalid-feedback"); if (feedback) feedback.remove(); }); const errors = Array.isArray(raw) ? raw : (raw && raw.field ? [raw] : []); errors.forEach(error => { const input = fields.querySelector(`[data-key="${CSS.escape(error.field || "")}"]`); if (!input) return; input.classList.add("is-invalid"); const feedback = document.createElement("div"); feedback.className = "invalid-feedback"; feedback.textContent = error.message || error.code || "Invalid value"; input.parentElement.appendChild(feedback); }); }
  function select(name) { project = name; status.textContent = "Loading..."; Promise.all([request(`/api/lua/projects/${encodeURIComponent(name)}/config/schema`), request(`/api/lua/projects/${encodeURIComponent(name)}/config`)]).then(([s, v]) => { schema = s; saved = v; savedSnapshot = JSON.stringify(saved); render(); }).catch(e => { status.textContent = e.message; }); }
  function loadProjects() { request("/api/lua/projects").then(projects => { projectList.innerHTML = ""; projects.forEach(name => { const button = document.createElement("button"); button.type = "button"; button.className = "list-group-item list-group-item-action"; button.textContent = name; button.onclick = () => { projectList.querySelectorAll("button").forEach(b => b.classList.remove("active")); button.classList.add("active"); select(name); }; projectList.appendChild(button); }); if (projects.length) projectList.firstChild.click(); else projectList.innerHTML = '<div class="p-3 text-muted">No Lua project configuration schemas found.</div>'; }).catch(e => { projectList.innerHTML = `<div class="p-3 text-danger">${e.message}</div>`; }); }
  document.getElementById("resetConfig").onclick = () => { render(); };
  document.getElementById("configForm").onsubmit = e => { e.preventDefault(); if (!project) return; const next = valuesFromForm(); status.textContent = "Applying..."; request(`/api/lua/projects/${encodeURIComponent(project)}/config`, { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(next) }).then(() => { saved = next; savedSnapshot = JSON.stringify(saved); render(); }).catch(err => { setFieldErrors(err.data && err.data.errors); status.textContent = err.message; }); };
  window.addEventListener("beforeunload", e => { if (schema && JSON.stringify(valuesFromForm()) !== savedSnapshot) { e.preventDefault(); e.returnValue = "Unsaved configuration changes"; } });
  loadProjects();
})();
