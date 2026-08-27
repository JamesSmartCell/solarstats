import { formatHaState, isSensorDomain, stateTone } from "./ha-display.js";

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
  renderPieRows(data.pieRows || data.loadConfig || []);

  if (devicesRes.ok) {
    const devicesData = await devicesRes.json();
    renderDeviceGroups(devicesData.devices || []);
  } else {
    console.warn("devices HTTP", devicesRes.status);
    renderDeviceGroups([]);
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

function flashSaved(id) {
  const note = document.getElementById(id);
  if (!note) return;
  note.hidden = false;
  setTimeout(() => {
    note.hidden = true;
  }, 1500);
}

function formatKwh(value) {
  const n = Number(value);
  return Number.isFinite(n) ? `${n.toFixed(3)} kWh` : "—";
}

function pieRowKey(load) {
  return load.builtin ? load.key : load.entityId || load.key;
}

function mergeOptionsFor(parentKey, rows) {
  return rows.filter((row) => {
    const key = pieRowKey(row);
    if (key === parentKey) return false;
    if (row.mergedInto) return false;
    if ((row.mergeChildren || []).length) return false;
    return true;
  });
}

function fillMergeSelect(select, rows, selectedKey) {
  const blank = document.createElement("option");
  blank.value = "";
  blank.textContent = "Choose a feed…";
  select.appendChild(blank);
  for (const row of rows) {
    const option = document.createElement("option");
    option.value = pieRowKey(row);
    option.textContent = row.label || row.key;
    if (selectedKey && option.value === selectedKey) option.selected = true;
    select.appendChild(option);
  }
}

function renderMergeCell(load, rows) {
  const td = document.createElement("td");
  td.className = "merge-cell";
  const key = pieRowKey(load);

  if (load.mergedInto) {
    const note = document.createElement("span");
    note.className = "merge-into";
    note.textContent = `→ ${load.mergedInto.label || load.mergedInto.key}`;
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "merge-remove";
    remove.title = "Unmerge";
    remove.textContent = "×";
    remove.addEventListener("click", () => savePieUnmerge(key));
    td.append(note, remove);
    return td;
  }

  const stack = document.createElement("div");
  stack.className = "merge-stack";
  const children = Array.isArray(load.mergeChildren) ? load.mergeChildren : [];
  const available = mergeOptionsFor(key, rows);

  for (const child of children) {
    const row = document.createElement("div");
    row.className = "merge-row";
    const select = document.createElement("select");
    select.className = "merge-select";
    select.title = "Merged into this feed";
    fillMergeSelect(select, [{ key: child.key, label: child.label }, ...available], child.key);
    select.addEventListener("change", () => {
      if (!select.value || select.value === child.key) return;
      savePieUnmerge(child.key).then((ok) => {
        if (ok) savePieMerge(key, select.value);
      });
    });
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "merge-remove";
    remove.title = "Unmerge";
    remove.textContent = "×";
    remove.addEventListener("click", () => savePieUnmerge(child.key));
    row.append(select, remove);
    stack.appendChild(row);
  }

  const add = document.createElement("button");
  add.type = "button";
  add.className = "merge-add";
  add.title = "Merge another feed into this one";
  add.textContent = "+";
  add.disabled = available.length === 0;
  add.addEventListener("click", () => {
    if (add.dataset.open === "1" || available.length === 0) return;
    add.dataset.open = "1";
    const row = document.createElement("div");
    row.className = "merge-row";
    const select = document.createElement("select");
    select.className = "merge-select";
    fillMergeSelect(select, available, "");
    select.addEventListener("change", () => {
      if (!select.value) return;
      savePieMerge(key, select.value);
    });
    row.appendChild(select);
    stack.insertBefore(row, add);
    select.focus();
  });
  stack.appendChild(add);
  td.appendChild(stack);
  return td;
}

function renderPieRows(rows) {
  const tbody = document.querySelector("#loadsTable tbody");
  const empty = document.getElementById("loadsEmpty");
  tbody.innerHTML = "";
  const list = Array.isArray(rows) ? rows : [];
  if (empty) empty.hidden = list.length > 0;

  for (const load of list) {
    const key = pieRowKey(load);
    const source = load.source === "inverter" ? "inverter" : "grid";
    const tr = document.createElement("tr");
    if (load.mergedInto) tr.classList.add("is-merged");

    const nameTd = document.createElement("td");
    nameTd.textContent = load.label || load.key;

    const entityTd = document.createElement("td");
    entityTd.className = "entity-id";
    entityTd.textContent = load.entityId || "—";

    const kwhTd = document.createElement("td");
    kwhTd.textContent = formatKwh(load.kwh);

    const invTd = document.createElement("td");
    invTd.className = "acl-cell";
    const inv = document.createElement("input");
    inv.type = "radio";
    inv.name = `load-src-${key}`;
    inv.checked = source === "inverter";
    inv.title = "Inverter load";
    inv.addEventListener("change", () => {
      if (inv.checked) saveLoadSource(key, "inverter");
    });
    invTd.appendChild(inv);

    const gridTd = document.createElement("td");
    gridTd.className = "acl-cell";
    const grid = document.createElement("input");
    grid.type = "radio";
    grid.name = `load-src-${key}`;
    grid.checked = source === "grid";
    grid.title = "Grid load";
    grid.addEventListener("change", () => {
      if (grid.checked) saveLoadSource(key, "grid");
    });
    gridTd.appendChild(grid);

    const includeTd = document.createElement("td");
    includeTd.className = "acl-cell";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = load.onPie !== false;
    cb.disabled = !!load.mergedInto;
    cb.title = load.mergedInto
      ? `Merged into ${load.mergedInto.label || load.mergedInto.key}`
      : "Show this energy sensor on the pie";
    cb.addEventListener("change", () => {
      savePieExtra(key, cb.checked);
    });
    includeTd.appendChild(cb);

    tr.append(nameTd, entityTd, kwhTd, invTd, gridTd, includeTd, renderMergeCell(load, list));
    tbody.appendChild(tr);
  }
}

async function saveLoadSource(key, source) {
  const res = await fetch("/api/admin/settings", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ loadSources: { [key]: source } }),
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "Load source update failed");
    load().catch(console.error);
    return;
  }
  flashSaved("loadsSaved");
}

async function postPieSettings(body) {
  const res = await fetch("/api/admin/settings", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    alert(data.error || "Pie update failed");
    load().catch(console.error);
    return null;
  }
  flashSaved("loadsSaved");
  if (data.pieRows) renderPieRows(data.pieRows);
  return data;
}

async function savePieExtra(entityId, onPie) {
  return postPieSettings({ pieExtra: { entityId, onPie } });
}

async function savePieMerge(parentKey, childKey) {
  return postPieSettings({ pieMerge: { parentKey, childKey } });
}

async function savePieUnmerge(childKey) {
  return postPieSettings({ pieUnmerge: { childKey } });
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

function renderDeviceGroups(devices) {
  const list = Array.isArray(devices) ? devices : [];
  renderDeviceTable(
    list.filter((d) => isSensorDomain(d.domain)),
    "sensorsTable",
    "sensorsEmpty",
  );
  renderDeviceTable(
    list.filter((d) => !isSensorDomain(d.domain)),
    "devicesTable",
    "devicesEmpty",
  );
}

function renderDeviceTable(devices, tableId, emptyId) {
  const tbody = document.querySelector(`#${tableId} tbody`);
  const empty = document.getElementById(emptyId);
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

    const tone = stateTone(d);
    const stateTd = document.createElement("td");
    stateTd.innerHTML = `<span class="status-pill ${tone}">${escapeHtml(formatHaState(d))}</span>`;

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
  for (const id of ["devicesSaved", "sensorsSaved"]) {
    const note = document.getElementById(id);
    if (!note) continue;
    note.hidden = false;
    setTimeout(() => {
      note.hidden = true;
    }, 1200);
  }
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
