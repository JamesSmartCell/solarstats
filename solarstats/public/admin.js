async function load() {
  const [usersRes, devicesRes] = await Promise.all([
    fetch("/api/admin/users"),
    fetch("/api/admin/devices"),
  ]);

  if (usersRes.status === 401 || usersRes.status === 403) {
    location.href = "/login";
    return;
  }
  if (!usersRes.ok) throw new Error(`users HTTP ${usersRes.status}`);

  const data = await usersRes.json();
  renderSettings(data.settings);
  renderUsers(data.users);

  if (devicesRes.ok) {
    const devicesData = await devicesRes.json();
    renderDevices(devicesData.devices || []);
  } else {
    console.warn("devices HTTP", devicesRes.status);
    renderDevices([]);
  }
}

function renderSettings(settings) {
  const allowNew = document.getElementById("allowNewAccounts");
  const allowPk = document.getElementById("allowPasskeyEnrollment");
  allowNew.checked = !!settings.allowNewAccounts;
  allowPk.checked = !!settings.allowPasskeyEnrollment;

  async function save() {
    const res = await fetch("/api/admin/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        allowNewAccounts: allowNew.checked,
        allowPasskeyEnrollment: allowPk.checked,
      }),
    });
    if (!res.ok) return;
    const note = document.getElementById("settingsSaved");
    note.hidden = false;
    setTimeout(() => {
      note.hidden = true;
    }, 1500);
  }

  allowNew.onchange = save;
  allowPk.onchange = save;
}

function renderUsers(users) {
  const tbody = document.querySelector("#usersTable tbody");
  tbody.innerHTML = "";
  for (const u of users) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escapeHtml(u.email)}</td>
      <td>${escapeHtml(u.role)}</td>
      <td><span class="status-pill ${escapeHtml(u.status)}">${escapeHtml(u.status)}</span></td>
      <td>${escapeHtml(formatDate(u.created_at))}</td>
      <td class="admin-actions"></td>
    `;
    const actions = tr.querySelector(".admin-actions");
    if (u.role !== "admin") {
      if (u.status !== "approved") {
        actions.appendChild(actionBtn("Approve", () => setStatus(u.id, "approved")));
      }
      if (u.status !== "denied") {
        actions.appendChild(actionBtn("Deny", () => setStatus(u.id, "denied")));
      }
      if (u.status === "approved") {
        actions.appendChild(actionBtn("Revoke", () => setStatus(u.id, "denied")));
      }
    } else {
      actions.textContent = "—";
    }
    tbody.appendChild(tr);
  }
}

function renderDevices(devices) {
  const tbody = document.querySelector("#devicesTable tbody");
  const empty = document.getElementById("devicesEmpty");
  tbody.innerHTML = "";
  empty.hidden = devices.length > 0;

  for (const d of devices) {
    const exposure = d.exposure || (d.allowUsers ? "user" : d.allowAdmin ? "admin" : "off");
    const tr = document.createElement("tr");
    tr.dataset.entityId = d.entityId;

    const nameTd = document.createElement("td");
    nameTd.textContent = d.name || d.entityId;

    const entityTd = document.createElement("td");
    entityTd.className = "entity-id";
    entityTd.textContent = d.entityId;

    const stateTd = document.createElement("td");
    stateTd.innerHTML = `<span class="status-pill ${d.state === "on" ? "on" : d.state === "off" ? "off" : ""}">${escapeHtml(d.state || "—")}</span>`;

    const adminTd = document.createElement("td");
    adminTd.className = "acl-cell";
    const adminCb = document.createElement("input");
    adminCb.type = "checkbox";
    adminCb.title = "Admin only on the board";
    adminCb.checked = exposure === "admin";
    adminCb.addEventListener("change", () => {
      if (adminCb.checked) {
        userCb.checked = false;
        setDeviceExposure(d.entityId, "admin");
      } else if (!userCb.checked) {
        setDeviceExposure(d.entityId, "off");
      }
    });
    adminTd.appendChild(adminCb);

    const userTd = document.createElement("td");
    userTd.className = "acl-cell";
    const userCb = document.createElement("input");
    userCb.type = "checkbox";
    userCb.title = "Everyone on the board (admin + users)";
    userCb.checked = exposure === "user";
    userCb.addEventListener("change", () => {
      if (userCb.checked) {
        adminCb.checked = false;
        setDeviceExposure(d.entityId, "user");
      } else if (!adminCb.checked) {
        setDeviceExposure(d.entityId, "off");
      }
    });
    userTd.appendChild(userCb);

    tr.append(nameTd, entityTd, stateTd, adminTd, userTd);
    tbody.appendChild(tr);
  }
}

async function setDeviceExposure(entityId, exposure) {
  const allowUsers = exposure === "user";
  const allowAdmin = exposure === "user" || exposure === "admin";
  const res = await fetch(`/api/admin/devices/${encodeURIComponent(entityId)}/acl`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ allowUsers, allowAdmin }),
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "ACL update failed");
    load().catch(console.error);
    return;
  }
  const note = document.getElementById("devicesSaved");
  note.hidden = false;
  setTimeout(() => {
    note.hidden = true;
  }, 1200);
}

function actionBtn(label, onClick) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.textContent = label;
  btn.addEventListener("click", onClick);
  return btn;
}

async function setStatus(id, status) {
  const res = await fetch(`/api/admin/users/${id}/status`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ status }),
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "Update failed");
    return;
  }
  load().catch(console.error);
}

function formatDate(iso) {
  if (!iso) return "—";
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso;
  }
}

function escapeHtml(s) {
  return String(s ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

load().catch((err) => {
  console.error(err);
  alert("Failed to load admin data");
});
