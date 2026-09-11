(function () {
  const levelFilter = document.getElementById("levelFilter");
  const categoryFilter = document.getElementById("categoryFilter");
  const searchBox = document.getElementById("searchBox");
  const autoScroll = document.getElementById("autoScroll");
  const livePause = document.getElementById("livePause");
  const logCount = document.getElementById("logCount");
  const scroller = document.getElementById("logScroll");
  const emptyMsg = document.getElementById("logsEmpty");
  const tbody = document.querySelector("#logsTable tbody");

  // Hard ceiling on retained entries. A gateway left open for days would
  // otherwise accumulate every line ever logged and grow the tab's memory
  // without bound (updateUI.md section 33).
  const MAX_ENTRIES = 3000;

  // Ranked so a level filter means "this and worse", which is how people
  // actually read logs -- picking WARNING should not hide ERROR.
  const LEVEL_RANK = { DEBUG: 0, INFO: 1, WARNING: 2, ERROR: 3 };

  let entries = [];
  let lastSeq = 0;

  function passesFilters(entry) {
    const wanted = levelFilter.value;
    if (wanted !== "all") {
      const min = LEVEL_RANK[wanted];
      const rank = LEVEL_RANK[entry.level];
      // "ERROR only" is the one exact match; the rest are thresholds.
      if (wanted === "ERROR" ? rank !== min : rank < min) return false;
    }

    if (categoryFilter.value !== "all" && entry.category !== categoryFilter.value) return false;

    const term = searchBox.value.trim().toLowerCase();
    if (term && entry.message.toLowerCase().indexOf(term) === -1) return false;

    return true;
  }

  function levelClass(level) {
    if (level === "ERROR") return "hsf-log-error";
    if (level === "WARNING") return "hsf-log-warn";
    if (level === "DEBUG") return "hsf-log-debug";
    return "";
  }

  function buildRow(entry) {
    const tr = document.createElement("tr");

    const tdTime = document.createElement("td");
    tdTime.className = "hsf-log-time";
    tdTime.textContent = entry.timestamp;

    const tdLevel = document.createElement("td");
    tdLevel.className = levelClass(entry.level);
    tdLevel.textContent = entry.level;

    const tdCat = document.createElement("td");
    tdCat.textContent = entry.category;

    const tdMsg = document.createElement("td");
    tdMsg.className = "hsf-log-msg";
    // textContent, never innerHTML: log messages carry Lua script output,
    // device payloads and error strings. Interpolating those as markup was
    // an injection route straight into this page.
    tdMsg.textContent = entry.message;

    tr.append(tdTime, tdLevel, tdCat, tdMsg);
    return tr;
  }

  function atBottom() {
    return scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 40;
  }

  function render() {
    const visible = entries.filter(passesFilters);

    // Rebuilds the list wholesale. Fine at this scale (bounded by
    // MAX_ENTRIES) and far simpler than reconciling incremental inserts
    // against a live filter and search box.
    const frag = document.createDocumentFragment();
    visible.forEach((e) => frag.appendChild(buildRow(e)));
    tbody.innerHTML = "";
    tbody.appendChild(frag);

    emptyMsg.hidden = visible.length > 0;
    logCount.textContent = visible.length + " shown" +
      (visible.length !== entries.length ? " of " + entries.length : "");

    if (autoScroll.checked) scroller.scrollTop = scroller.scrollHeight;
  }

  function addEntries(incoming) {
    if (!incoming || !incoming.length) return false;

    let added = false;
    incoming.forEach((e) => {
      // seq is the dedupe key: the same entry can arrive from both the
      // initial REST fetch and the WebSocket stream.
      if (e.seq && e.seq <= lastSeq) return;
      if (e.seq) lastSeq = Math.max(lastSeq, e.seq);
      entries.push(e);
      added = true;
    });

    if (entries.length > MAX_ENTRIES) {
      entries.splice(0, entries.length - MAX_ENTRIES);
    }
    return added;
  }

  function loadHistory() {
    // Always fetches across categories; filtering happens client-side so
    // switching the category selector doesn't require a refetch and doesn't
    // lose entries already streamed in.
    fetch("/api/logs?category=all&limit=1000")
      .then((r) => r.json())
      .then((rows) => {
        addEntries(rows);
        render();
        // Stick to the bottom on first load -- newest lines are the point.
        scroller.scrollTop = scroller.scrollHeight;
      })
      .catch(() => {
        emptyMsg.hidden = false;
        emptyMsg.textContent = "Failed to load logs.";
      });
  }

  // Live tail over the WebSocket rather than polling: the gateway pushes
  // only entries created since its last tick.
  HsfWs.onMessage((payload) => {
    if (payload.type !== "status") return;

    // Always ingest, even while paused. The gateway sends each entry exactly
    // once (everything since its previous tick), so skipping the frame would
    // discard those lines permanently rather than deferring them -- pause
    // must freeze the VIEW, not drop data.
    const added = addEntries(payload.logs);
    if (added && !livePause.checked) render();
  });

  // Resuming shows whatever accumulated while paused.
  livePause.addEventListener("change", () => {
    if (!livePause.checked) render();
  });

  [levelFilter, categoryFilter].forEach((el) => el.addEventListener("change", render));
  searchBox.addEventListener("input", render);

  // Turning auto-scroll on should jump to the newest line immediately,
  // rather than waiting for the next entry to arrive.
  autoScroll.addEventListener("change", () => {
    if (autoScroll.checked) scroller.scrollTop = scroller.scrollHeight;
  });

  // Scrolling up is an implicit "let me read this" -- keeping auto-scroll on
  // would yank the view back down on the next entry.
  scroller.addEventListener("scroll", () => {
    if (autoScroll.checked && !atBottom()) autoScroll.checked = false;
  });

  document.getElementById("btnClear").addEventListener("click", () => {
    fetch("/api/logs/clear", { method: "POST" })
      .then(() => {
        entries = [];
        render();
      })
      .catch(() => {
        // Clear the local view even if the server call failed, so the button
        // still does something visible.
        entries = [];
        render();
      });
  });

  document.getElementById("btnDownload").addEventListener("click", () => {
    // Downloads what is on screen, matching the current filters -- that is
    // usually the subset worth sending to someone.
    const text = entries
      .filter(passesFilters)
      .map((e) => `[${e.timestamp}] [${e.level}] [${e.category}] ${e.message}`)
      .join("\n");
    const blob = new Blob([text], { type: "text/plain" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "hsf_gateway_logs.txt";
    link.click();
    URL.revokeObjectURL(link.href);
  });

  loadHistory();
})();
