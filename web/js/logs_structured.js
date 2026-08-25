/* Structured log search -- request/upgrade.md sections 13-16.
 *
 * Distinct from logs.js, which follows the runtime tail over the WebSocket.
 * Nothing here streams: every result set comes from an explicit query against
 * /api/logs/structured, one page at a time, because the whole point of section
 * 16 is that the browser must never hold the entire SQLite table. */
(function () {
  const fromEl = document.getElementById("sFrom");
  const toEl = document.getElementById("sTo");
  const typeButton = document.getElementById("sTypeButton");
  const typeMenu = document.getElementById("sTypeMenu");
  const levelEl = document.getElementById("sLevel");
  const scriptEl = document.getElementById("sScript");
  const keywordEl = document.getElementById("sKeyword");
  const limitEl = document.getElementById("sLimit");
  const countEl = document.getElementById("sCount");
  const rangeEl = document.getElementById("sRange");
  const pagerEl = document.getElementById("sPager");
  const table = document.getElementById("sTable");
  const emptyEl = document.getElementById("sEmpty");
  const detailEl = document.getElementById("sDetail");
  const detailTitle = document.getElementById("sDetailTitle");
  const detailFields = document.getElementById("sDetailFields");

  // Field lists per log type, so the result table can use a type's own fields
  // as columns and the detail view can order them as declared.
  let definitions = {};
  let selectedTypes = [];
  let offset = 0;
  let lastResult = { entries: [], total: 0, limit: 50, offset: 0 };
  // Columns currently rendered after the four fixed ones, so the CSV export
  // and the row builder can't disagree about the shape.
  let dynamicColumns = [];
  let loaded = false;

  function limit() {
    return parseInt(limitEl.value, 10) || 50;
  }

  function updateTypeButton() {
    if (!selectedTypes.length) {
      typeButton.textContent = "All types";
    } else if (selectedTypes.length === 1) {
      typeButton.textContent = selectedTypes[0];
    } else {
      typeButton.textContent = selectedTypes.length + " types";
    }
  }

  function renderTypeMenu(types) {
    typeMenu.innerHTML = "";

    if (!types.length) {
      const none = document.createElement("div");
      none.className = "text-muted small px-1";
      none.textContent = "No log types defined.";
      typeMenu.appendChild(none);
      return;
    }

    const all = document.createElement("button");
    all.type = "button";
    all.className = "btn btn-sm btn-outline-secondary w-100 mb-2";
    all.textContent = "All types";
    all.addEventListener("click", () => {
      selectedTypes = [];
      typeMenu.querySelectorAll("input[type=checkbox]").forEach((cb) => (cb.checked = false));
      updateTypeButton();
      search(0);
    });
    typeMenu.appendChild(all);

    types.forEach((def) => {
      const wrap = document.createElement("div");
      wrap.className = "form-check";

      const cb = document.createElement("input");
      cb.className = "form-check-input";
      cb.type = "checkbox";
      cb.id = "sType_" + def.type;
      cb.value = def.type;
      cb.addEventListener("change", () => {
        selectedTypes = Array.from(typeMenu.querySelectorAll("input[type=checkbox]:checked")).map(
          (el) => el.value
        );
        updateTypeButton();
        search(0);
      });

      const label = document.createElement("label");
      label.className = "form-check-label small";
      label.htmlFor = cb.id;
      // textContent throughout: type names and descriptions come from a
      // user-edited JSON file, and this page has been an injection route once
      // already (see the log-message note in logs.js).
      label.textContent = def.type;
      if (def.description) label.title = def.description;

      wrap.append(cb, label);
      typeMenu.appendChild(wrap);
    });
  }

  function loadDefinitions() {
    return fetch("/api/logs/definitions")
      .then((r) => r.json())
      .then((data) => {
        definitions = {};
        (data.types || []).forEach((def) => (definitions[def.type] = def));
        renderTypeMenu(data.types || []);

        // Rebuilt from scratch each time, preserving the selection: a script
        // that logs for the first time mid-session should appear here without
        // a page reload.
        const previous = scriptEl.value;
        scriptEl.innerHTML = "";
        const allOption = document.createElement("option");
        allOption.value = "";
        allOption.textContent = "All scripts";
        scriptEl.appendChild(allOption);
        (data.scripts || []).forEach((name) => {
          const option = document.createElement("option");
          option.value = name;
          option.textContent = name;
          scriptEl.appendChild(option);
        });
        scriptEl.value = previous;

        if (data.available === false) {
          emptyEl.textContent =
            "The structured log store is unavailable. Check the gateway log for why config/logs.db could not be opened.";
        }
      })
      .catch(() => {
        emptyEl.textContent = "Failed to load log definitions.";
      });
  }

  function queryString(pageOffset) {
    const params = new URLSearchParams();
    if (selectedTypes.length) params.set("type", selectedTypes.join(","));
    if (levelEl.value && levelEl.value !== "all") params.set("level", levelEl.value);
    if (scriptEl.value) params.set("script", scriptEl.value);
    if (keywordEl.value.trim()) params.set("keyword", keywordEl.value.trim());
    // Sent as the input's own "YYYY-MM-DDTHH:MM"; the backend normalises the
    // 'T' and widens a minute-precision upper bound to that whole minute.
    if (fromEl.value) params.set("from", fromEl.value);
    if (toEl.value) params.set("to", toEl.value);
    params.set("limit", String(limit()));
    params.set("offset", String(pageOffset));
    return params.toString();
  }

  /* Column set for the result table. With exactly one log type selected, that
     type's own fields become columns -- which is what section 15's example
     shows (Result, Card UID). Across several types there is no shared field
     list, so one Details column summarises each row instead. */
  function computeColumns() {
    if (selectedTypes.length === 1 && definitions[selectedTypes[0]]) {
      return definitions[selectedTypes[0]].fields.map((f) => f.name);
    }
    return null;
  }

  function summarise(entry) {
    const fields = entry.fields || {};
    return Object.keys(fields)
      .map((key) => key + "=" + fields[key])
      .join("  ");
  }

  function levelBadgeClass(level) {
    if (level === "ERROR") return "hsf-badge hsf-badge-bad";
    if (level === "WARNING") return "hsf-badge hsf-badge-warn";
    return "hsf-badge hsf-badge-idle";
  }

  function renderHead() {
    const tr = table.querySelector("thead tr");
    tr.innerHTML = "";
    const fixed = ["Time", "Type", "Script", "Level"];
    const headers = dynamicColumns ? fixed.concat(dynamicColumns) : fixed.concat(["Details"]);
    headers.forEach((text) => {
      const th = document.createElement("th");
      th.textContent = text;
      tr.appendChild(th);
    });
  }

  function renderRows(entries) {
    const tbody = table.querySelector("tbody");
    tbody.innerHTML = "";

    entries.forEach((entry) => {
      const tr = document.createElement("tr");
      tr.style.cursor = "pointer";
      tr.addEventListener("click", () => showDetail(entry));

      const cells = [entry.timestamp, entry.log_type, entry.script_name];
      cells.forEach((text) => {
        const td = document.createElement("td");
        td.textContent = text || "";
        tr.appendChild(td);
      });

      const levelTd = document.createElement("td");
      const badge = document.createElement("span");
      badge.className = levelBadgeClass(entry.level);
      badge.textContent = entry.level || "";
      levelTd.appendChild(badge);
      tr.appendChild(levelTd);

      if (dynamicColumns) {
        dynamicColumns.forEach((name) => {
          const td = document.createElement("td");
          td.textContent = (entry.fields || {})[name] || "";
          tr.appendChild(td);
        });
      } else {
        const td = document.createElement("td");
        td.className = "hsf-log-msg";
        td.textContent = summarise(entry);
        tr.appendChild(td);
      }

      tbody.appendChild(tr);
    });
  }

  function renderPager(total, pageLimit, pageOffset) {
    pagerEl.innerHTML = "";
    const pages = Math.max(1, Math.ceil(total / pageLimit));
    const current = Math.floor(pageOffset / pageLimit);

    function button(label, targetPage, disabled, active) {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btn btn-sm " + (active ? "btn-primary" : "btn-outline-secondary");
      btn.textContent = label;
      btn.disabled = !!disabled;
      if (!disabled) btn.addEventListener("click", () => search(targetPage * pageLimit));
      return btn;
    }

    pagerEl.appendChild(button("< Previous", current - 1, current === 0, false));

    // A window around the current page rather than every page: months of card
    // history is hundreds of pages, and a row of 400 buttons is not a control.
    const first = Math.max(0, Math.min(current - 2, pages - 5));
    const last = Math.min(pages - 1, first + 4);
    for (let page = first; page <= last; page++) {
      pagerEl.appendChild(button(String(page + 1), page, false, page === current));
    }

    pagerEl.appendChild(button("Next >", current + 1, current >= pages - 1, false));
  }

  function search(pageOffset) {
    offset = pageOffset || 0;
    dynamicColumns = computeColumns();
    renderHead();

    fetch("/api/logs/structured?" + queryString(offset))
      .then((r) => r.json())
      .then((data) => {
        lastResult = data;
        const entries = data.entries || [];
        renderRows(entries);

        emptyEl.hidden = entries.length > 0;
        countEl.textContent = (data.total || 0) + " entries";
        rangeEl.textContent = entries.length
          ? "showing " + (data.offset + 1) + "-" + (data.offset + entries.length)
          : "";
        renderPager(data.total || 0, data.limit || limit(), data.offset || 0);
      })
      .catch(() => {
        emptyEl.hidden = false;
        emptyEl.textContent = "Search failed.";
      });
  }

  /* Section 15's detail view. Refetched rather than rendered from the row we
     already have: the list response carries only the fields that were stored,
     while this also returns the declared field ORDER, so the panel reads the
     way the definitions file describes the type instead of alphabetically. */
  function showDetail(entry) {
    fetch("/api/logs/structured/" + encodeURIComponent(entry.id))
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error("not found"))))
      .then((full) => renderDetail(full))
      // Falling back to the row we have beats an empty panel if the entry was
      // pruned between the search and the click.
      .catch(() => renderDetail(entry));
  }

  function renderDetail(entry) {
    detailEl.hidden = false;
    detailTitle.textContent = entry.log_type || "Entry";

    detailFields.innerHTML = "";

    function addRow(label, value) {
      const dt = document.createElement("dt");
      dt.className = "text-muted fw-normal";
      dt.textContent = label;
      const dd = document.createElement("dd");
      dd.className = "mb-2";
      dd.style.wordBreak = "break-word";
      dd.textContent = value === "" || value === undefined || value === null ? "--" : String(value);
      detailFields.append(dt, dd);
    }

    addRow("Time", entry.timestamp);
    addRow("Script", entry.script_name);
    addRow("Level", entry.level);

    const fields = entry.fields || {};
    // Declared order first, then anything stored that the definition no longer
    // mentions -- a field removed from log_definitions.json must not make the
    // old rows' values invisible.
    const order = (entry.field_order && entry.field_order.length
      ? entry.field_order
      : (definitions[entry.log_type] || { fields: [] }).fields.map((f) => f.name)
    ).slice();
    Object.keys(fields).forEach((name) => {
      if (order.indexOf(name) === -1) order.push(name);
    });

    order.forEach((name) => {
      if (Object.prototype.hasOwnProperty.call(fields, name)) addRow(name, fields[name]);
    });
  }

  document.getElementById("btnCloseDetail").addEventListener("click", () => {
    detailEl.hidden = true;
  });

  document.getElementById("btnSearch").addEventListener("click", () => search(0));
  document.getElementById("btnResetSearch").addEventListener("click", () => {
    fromEl.value = "";
    toEl.value = "";
    levelEl.value = "all";
    scriptEl.value = "";
    keywordEl.value = "";
    selectedTypes = [];
    typeMenu.querySelectorAll("input[type=checkbox]").forEach((cb) => (cb.checked = false));
    updateTypeButton();
    detailEl.hidden = true;
    search(0);
  });

  [levelEl, scriptEl, limitEl].forEach((el) => el.addEventListener("change", () => search(0)));
  keywordEl.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      search(0);
    }
  });
  [fromEl, toEl].forEach((el) => el.addEventListener("change", () => search(0)));

  document.getElementById("btnExportCsv").addEventListener("click", () => {
    const entries = lastResult.entries || [];
    if (!entries.length) return;

    const columns = dynamicColumns || [];
    const header = ["timestamp", "log_type", "script_name", "level"].concat(
      columns.length ? columns : ["fields"]
    );

    function cell(value) {
      const text = value === undefined || value === null ? "" : String(value);
      return '"' + text.replace(/"/g, '""') + '"';
    }

    const rows = entries.map((entry) => {
      const base = [entry.timestamp, entry.log_type, entry.script_name, entry.level];
      const rest = columns.length
        ? columns.map((name) => (entry.fields || {})[name] || "")
        : [summarise(entry)];
      return base.concat(rest).map(cell).join(",");
    });

    const blob = new Blob([header.map(cell).join(",") + "\n" + rows.join("\n")], {
      type: "text/csv",
    });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "hsf_structured_logs.csv";
    link.click();
    URL.revokeObjectURL(link.href);
  });

  /* Loaded when the tab is first opened rather than on page load: most visits
     to this page are to watch the runtime tail, and a search plus a
     definitions fetch on every one of those is work nobody asked for. */
  document.getElementById("tabStructuredBtn").addEventListener("shown.bs.tab", () => {
    if (loaded) return;
    loaded = true;
    loadDefinitions().then(() => search(0));
  });
})();
