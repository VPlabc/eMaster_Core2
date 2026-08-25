(function () {
  const clientsTbody = document.querySelector("#clientsTable tbody");
  const testApiKeySelect = document.getElementById("testApiKeySelect");
  const newClientKeyBox = document.getElementById("newClientKeyBox");
  const historyTbody = document.querySelector("#testHistoryTable tbody");

  function fmtTimestamp(unixSeconds) {
    if (!unixSeconds) return "--";
    return new Date(unixSeconds * 1000).toLocaleString();
  }

  // Builds a <td>. `tag` wraps the text in <code>/<strong>/etc when given.
  // Always textContent, never interpolated markup: client names are free text
  // typed into the form above and card data comes off a reader, so building
  // these rows as HTML strings let either one inject into this page.
  function cell(text, tag) {
    const td = document.createElement("td");
    if (tag) {
      const inner = document.createElement(tag);
      inner.textContent = text;
      td.appendChild(inner);
    } else {
      td.textContent = text;
    }
    return td;
  }

  function renderClients(clients) {
    clientsTbody.innerHTML = "";
    testApiKeySelect.innerHTML = '<option value="">Select a client...</option>';

    clients.forEach((c) => {
      const tr = document.createElement("tr");

      const tdStatus = document.createElement("td");
      const badge = document.createElement("span");
      badge.className = c.enabled ? "badge bg-success" : "badge bg-secondary";
      badge.textContent = c.enabled ? "Enabled" : "Disabled";
      tdStatus.appendChild(badge);

      const tdActions = document.createElement("td");
      const toggle = document.createElement("button");
      toggle.className = "btn btn-sm btn-outline-secondary btn-toggle";
      toggle.dataset.id = c.id;
      toggle.dataset.enabled = String(c.enabled);
      toggle.textContent = c.enabled ? "Disable" : "Enable";
      const del = document.createElement("button");
      del.className = "btn btn-sm btn-outline-danger btn-delete ms-1";
      del.dataset.id = c.id;
      del.textContent = "Delete";
      tdActions.append(toggle, del);

      tr.append(
        cell(c.id),
        cell(c.name),
        cell(c.api_key, "code"),
        tdStatus,
        cell(fmtTimestamp(c.created_at)),
        cell(fmtTimestamp(c.last_access)),
        cell(fmtTimestamp(c.expires_at)),
        tdActions
      );
      clientsTbody.appendChild(tr);

      const option = document.createElement("option");
      option.value = c.api_key;
      option.textContent = `${c.name} (id ${c.id})`;
      testApiKeySelect.appendChild(option);
    });
  }

  function refreshClients() {
    return fetch("/api/card/clients")
      .then((r) => r.json())
      .then((data) => renderClients(data.clients || []));
  }

  document.getElementById("createClientForm").addEventListener("submit", (e) => {
    e.preventDefault();
    const name = document.getElementById("newClientName").value.trim();
    const expiresInput = document.getElementById("newClientExpires").value;
    const expiresAt = expiresInput ? Math.floor(new Date(expiresInput).getTime() / 1000) : 0;

    fetch("/api/card/clients", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name, expires_at: expiresAt }),
    })
      .then((r) => r.json())
      .then((client) => {
        if (client.error) {
          newClientKeyBox.className = "alert alert-danger";
          newClientKeyBox.textContent = client.error;
        } else {
          newClientKeyBox.className = "alert alert-success";
          newClientKeyBox.textContent = "Created ";
          const nameEl = document.createElement("strong");
          nameEl.textContent = client.name;
          const keyEl = document.createElement("code");
          keyEl.textContent = client.api_key;
          newClientKeyBox.append(nameEl, " — API-Key: ", keyEl);
        }
        newClientKeyBox.classList.remove("d-none");
        document.getElementById("newClientName").value = "";
        document.getElementById("newClientExpires").value = "";
        refreshClients();
      });
  });

  clientsTbody.addEventListener("click", (e) => {
    const toggleBtn = e.target.closest(".btn-toggle");
    const deleteBtn = e.target.closest(".btn-delete");

    if (toggleBtn) {
      const id = toggleBtn.dataset.id;
      const currentlyEnabled = toggleBtn.dataset.enabled === "true";
      fetch(`/api/card/clients/${id}/${currentlyEnabled ? "disable" : "enable"}`, { method: "POST" }).then(
        refreshClients
      );
    }

    if (deleteBtn) {
      const id = deleteBtn.dataset.id;
      fetch(`/api/card/clients/${id}`, { method: "DELETE" }).then(refreshClients);
    }
  });

  document.getElementById("btnRefreshClients").addEventListener("click", refreshClients);

  document.getElementById("cardTestForm").addEventListener("submit", (e) => {
    e.preventDefault();
    const apiKey = testApiKeySelect.value;
    const cardData = document.getElementById("testCardData").value;
    const position = document.getElementById("testPosition").value;
    const sentAt = new Date().toLocaleTimeString();

    const form = new FormData();
    form.append("CardData", cardData);
    form.append("Position", position);

    fetch("/api/card/input", {
      method: "POST",
      headers: { "API-Key": apiKey },
      body: form,
    })
      .then((r) => r.json().then((body) => ({ status: r.status, body })))
      .then(({ status, body }) => {
        document.getElementById("testHttpStatus").textContent = status;
        document.getElementById("testJsonResponse").textContent = JSON.stringify(body, null, 2);

        const tr = document.createElement("tr");
        tr.append(
          cell(sentAt),
          cell(cardData),
          cell(position),
          cell(status),
          cell(JSON.stringify(body), "code")
        );
        historyTbody.prepend(tr);
      });
  });

  refreshClients();
})();
