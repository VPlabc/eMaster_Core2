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
  function setPath(object, path, value) { const parts = path.replace(/\[(\d+)\]/g, ".$1").split("."); let current = object; parts.forEach((part, i) => { if (i === parts.length - 1) current[part] = value; else current = current[part] = current[part] || (/^\d+$/.test(parts[i + 1]) ? [] : {}); }); }
  function inputFor(field, value, path) {
    const wrap = document.createElement("div"); wrap.className = "col-md-6";
    if (field.type === "array_group") {
      wrap.className = "col-12";
      wrap.dataset.arrayPath = path;
      const heading = document.createElement("div"); heading.className = "d-flex justify-content-between align-items-center mb-2";
      const label = document.createElement("strong"); label.textContent = field.label || field.key; heading.appendChild(label);
      const add = document.createElement("button"); add.type = "button"; add.className = "btn btn-sm btn-outline-primary"; add.textContent = "+ Add"; heading.appendChild(add); wrap.appendChild(heading);
      const list = document.createElement("div"); list.className = "row g-2"; wrap.appendChild(list);
      wrap._arrayValue = () => value;
      const itemSchema = field.item_schema || { fields: [] };
      function draw() { list.innerHTML = ""; const items = Array.isArray(value) ? value : []; items.forEach((item, index) => { const card = document.createElement("div"); card.className = "col-12 border rounded p-2"; const bar = document.createElement("div"); bar.className = "d-flex justify-content-between mb-2"; const itemTitle = document.createElement("span"); itemTitle.className = "small text-muted"; itemTitle.textContent = (field.item_label || "Item") + " " + (index + 1); bar.appendChild(itemTitle); const remove = document.createElement("button"); remove.type = "button"; remove.className = "btn btn-sm btn-outline-danger"; remove.textContent = "Remove"; remove.onclick = () => { value.splice(index, 1); draw(); updateDirty(); }; bar.appendChild(remove); card.appendChild(bar); const row = document.createElement("div"); row.className = "row g-2"; (itemSchema.fields || []).forEach(child => row.appendChild(inputFor(child, item[child.key], path + "[" + index + "]." + child.key))); (itemSchema.groups || []).forEach(group => (group.fields || []).forEach(child => row.appendChild(inputFor(child, item[child.key], path + "[" + index + "]." + child.key)))); card.appendChild(row); list.appendChild(card); }); }
      add.onclick = () => { if (field.max_items != null && value.length >= field.max_items) return; value.push(defaultsForSchema(itemSchema)); draw(); updateDirty(); }; draw(); return wrap;
    }
    const label = document.createElement("label"); label.className = "form-label"; label.textContent = field.label || field.key; label.htmlFor = "cfg-" + field.key; wrap.appendChild(label);
    let input;
    if (field.type === "boolean") { input = document.createElement("input"); input.type = "checkbox"; input.className = "form-check-input ms-2"; input.checked = value === true; label.appendChild(input); }
    else if (field.type === "select") { input = document.createElement("select"); input.className = "form-select"; (field.options || []).forEach(o => { const option = new Option(o.label || o.value, o.value); option.selected = value === o.value; input.add(option); }); }
    else if (field.type === "textarea") { input = document.createElement("textarea"); input.className = "form-control"; input.rows = 4; input.value = value == null ? "" : value; }
    else { input = document.createElement("input"); input.className = "form-control"; input.type = field.type === "number" ? "number" : (field.type === "password" || field.type === "secret" ? "password" : "text"); input.value = value == null ? "" : value; if (field.min != null) input.min = field.min; if (field.max != null) input.max = field.max; if (field.step != null) input.step = field.step; }
    input.id = "cfg-" + path.replace(/[^a-zA-Z0-9_-]/g, "-"); input.dataset.path = path; input.dataset.type = field.type || "string"; if (field.required) input.required = true; input.addEventListener("input", () => { input.classList.remove("is-invalid"); const feedback = wrap.querySelector(".invalid-feedback"); if (feedback) feedback.remove(); updateDirty(); }); wrap.appendChild(input);
    if (field.unit) { const small = document.createElement("small"); small.className = "text-muted"; small.textContent = field.unit; wrap.appendChild(small); }
    return wrap;
  }
  function defaultsForSchema(node) { const result = {}; (node.fields || []).forEach(f => { if (f.default !== undefined) result[f.key] = f.default; else if (f.type === "array_group") result[f.key] = []; }); return result; }
  function render() { fields.innerHTML = ""; if (!schema) return; title.textContent = schema.title || project + " Configuration"; (schema.groups || [{ fields: schema.fields || [] }]).forEach(group => { if (group.label) { const h = document.createElement("h6"); h.className = "col-12 border-bottom pt-2"; h.textContent = group.label; fields.appendChild(h); } (group.fields || []).forEach(f => fields.appendChild(inputFor(f, saved[f.key], f.key))); }); updateDirty(); }
  function valuesFromForm() { const result = {}; fields.querySelectorAll("[data-path]").forEach(input => { let v = input.type === "checkbox" ? input.checked : input.value; if (input.dataset.type === "number" && v !== "") v = Number(v); result[input.dataset.path] = v; }); fields.querySelectorAll("[data-array-path]").forEach(node => { result[node.dataset.arrayPath] = node._arrayValue(); }); return result; }
  function updateDirty() { if (!schema) return; const dirty = JSON.stringify(valuesFromForm()) !== savedSnapshot; status.textContent = dirty ? "Unsaved changes" : "Saved"; }
  function setFieldErrors(raw) { fields.querySelectorAll("[data-path]").forEach(input => { input.classList.remove("is-invalid"); const feedback = input.parentElement.querySelector(".invalid-feedback"); if (feedback) feedback.remove(); }); const errors = Array.isArray(raw) ? raw : (raw && raw.field ? [raw] : []); errors.forEach(error => { const input = fields.querySelector(`[data-path="${CSS.escape(error.field || "")}"]`); if (!input) return; input.classList.add("is-invalid"); const feedback = document.createElement("div"); feedback.className = "invalid-feedback"; feedback.textContent = error.message || error.code || "Invalid value"; input.parentElement.appendChild(feedback); }); }
  function select(name) { project = name; status.textContent = "Loading..."; Promise.all([request(`/api/lua/projects/${encodeURIComponent(name)}/config/schema`), request(`/api/lua/projects/${encodeURIComponent(name)}/config`)]).then(([s, v]) => { schema = s; saved = v; savedSnapshot = JSON.stringify(saved); render(); }).catch(e => { status.textContent = e.message; }); }
  function loadProjects() { request("/api/lua/projects").then(projects => { projectList.innerHTML = ""; projects.forEach(name => { const button = document.createElement("button"); button.type = "button"; button.className = "list-group-item list-group-item-action"; button.textContent = name; button.onclick = () => { projectList.querySelectorAll("button").forEach(b => b.classList.remove("active")); button.classList.add("active"); select(name); }; projectList.appendChild(button); }); if (projects.length) projectList.firstChild.click(); else projectList.innerHTML = '<div class="p-3 text-muted">No running Lua project has registered a schema.</div>'; }).catch(e => { projectList.innerHTML = `<div class="p-3 text-danger">${e.message}</div>`; }); }
  document.getElementById("resetConfig").onclick = () => { render(); };
  document.getElementById("configForm").onsubmit = e => { e.preventDefault(); if (!project) return; const next = valuesFromForm(); status.textContent = "Applying..."; request(`/api/lua/projects/${encodeURIComponent(project)}/config`, { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(next) }).then(() => { saved = next; savedSnapshot = JSON.stringify(saved); render(); }).catch(err => { setFieldErrors(err.data && err.data.errors); status.textContent = err.message; }); };
  window.addEventListener("beforeunload", e => { if (schema && JSON.stringify(valuesFromForm()) !== savedSnapshot) { e.preventDefault(); e.returnValue = "Unsaved configuration changes"; } });
  loadProjects();
})();
