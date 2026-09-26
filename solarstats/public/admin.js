import { formatHaState, isSensorDomain, stateTone } from "./ha-display.js";

const SITE = new URLSearchParams(location.search).get("site") || "home";

function adminPath(path) {
  const u = new URL(path, location.origin);
  if (SITE && SITE !== "home") u.searchParams.set("site", SITE);
  return `${u.pathname}${u.search}`;
}

let selectedGateway = "";

async function load() {
  const [usersRes, devicesRes, zbgwRes, fieldsRes, fwRes] = await Promise.all([
    fetch(adminPath("/api/admin/users")),
    fetch(adminPath("/api/admin/devices")),
    fetch(zbgwUrl()),
    fetch(adminPath("/api/admin/fields")),
    fetch("/api/admin/fw"),
  ]);

  if (usersRes.status === 401 || usersRes.status === 403) {
    location.href = "/login";
    return;
  }
  if (!usersRes.ok) throw new Error(`users HTTP ${usersRes.status}`);

  const data = await usersRes.json();
  const dash = document.getElementById("dashLink");
  if (dash) dash.href = data.site?.slug && data.site.slug !== "home" ? `/${data.site.slug}` : "/";
  const sub = document.getElementById("adminSub");
  if (sub && data.site?.name) sub.textContent = `${data.site.name} · devices, sensors, and the daily pie`;
  document.querySelectorAll("[data-home-admin]").forEach((el) => {
    el.hidden = !data.isHomeAdmin;
  });
  if (data.settings) renderSettings(data.settings);
  if (data.isHomeAdmin) {
    renderUsers(data.users);
    loadSiteAdmins().catch((err) => console.warn(err));
  }
  renderPieRows(data.pieRows || data.loadConfig || []);

  if (devicesRes.ok) {
    const devicesData = await devicesRes.json();
    renderDeviceGroups(devicesData.devices || []);
  } else {
    console.warn("devices HTTP", devicesRes.status);
    renderDeviceGroups([]);
  }

  if (zbgwRes.ok) {
    const zbgwData = await zbgwRes.json();
    renderGateways(zbgwData.gateways || [], zbgwData.events || []);
  } else {
    console.warn("zbgw HTTP", zbgwRes.status);
    renderGateways([], []);
  }

  if (fieldsRes.ok) {
    renderHaFields(await fieldsRes.json());
  } else {
    console.warn("fields HTTP", fieldsRes.status);
    renderHaFields({ fields: [] });
  }

  if (fwRes.ok) {
    renderFw(await fwRes.json());
  } else {
    console.warn("fw HTTP", fwRes.status);
    renderFw({ present: false });
  }
}

function renderFw(info) {
  const el = document.getElementById("fwMeta");
  if (!el) return;
  if (!info?.present) {
    el.textContent = "none — upload zigbee-gateway.bin from the IDF build";
    return;
  }
  const mb = ((info.size || 0) / 1048576).toFixed(2);
  const ver = info.version || "unknown";
  const url = info.publicUrl || info.publicPath || "/fw/zigbee-gateway.bin";
  el.textContent = `${ver} · ${mb} MB · ${url}`;
}

function renderHaFields(data) {
  const tbody = document.querySelector("#fieldsTable tbody");
  const empty = document.getElementById("fieldsEmpty");
  const fields = data.fields || [];
  tbody.replaceChildren();
  empty.hidden = fields.length > 0;

  for (const field of fields) {
    const tr = document.createElement("tr");
    const age = field.haUpdated || field.lastSeen
      ? formatAge(Math.round((Date.now() - (field.haUpdated || field.lastSeen)) / 1000))
      : "—";
    const statusCls =
      field.status === "ok" || field.status === "healed"
        ? "on"
        : field.status === "stale"
          ? "pending"
          : "denied";
    tr.innerHTML = `
      <td>${escapeHtml(field.label || field.key)}</td>
      <td class="entity-id">${escapeHtml(field.entityId || "—")}${field.name ? `<div>${escapeHtml(field.name)}</div>` : ""}</td>
      <td>${field.value == null ? "—" : escapeHtml(String(field.value))}</td>
      <td>${escapeHtml(age)}</td>
      <td><span class="status-pill ${statusCls}">${escapeHtml(field.status)}</span></td>
      <td class="admin-actions"></td>
    `;
    const actions = tr.querySelector(".admin-actions");
    const candidates = field.candidates || [];
    if (candidates.length) {
      const select = document.createElement("select");
      select.innerHTML = `<option value="">${candidates.length} candidate(s)</option>`;
      for (const c of candidates) {
        const opt = document.createElement("option");
        opt.value = c.entityId;
        opt.textContent = `${c.entityId} (${c.state ?? "?"} ${c.unit || ""})`;
        select.appendChild(opt);
      }
      select.addEventListener("change", () => {
        if (select.value) bindField(field.key, select.value);
      });
      actions.appendChild(select);
    } else if (field.status === "missing") {
      actions.textContent = "none found";
    }
    tbody.appendChild(tr);
  }
}

async function bindField(key, entityId) {
  const res = await fetch(adminPath(`/api/admin/fields/${encodeURIComponent(key)}`), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ entityId }),
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "Bind failed");
    return;
  }
  flashSaved("fieldsSaved");
  renderHaFields(await res.json());
}

function zbgwUrl() {
  const q = selectedGateway ? `?device=${encodeURIComponent(selectedGateway)}` : "";
  return `/api/admin/zbgw${q}`;
}

async function loadSiteAdmins() {
  const root = document.getElementById("siteAdmins");
  if (!root) return;
  const res = await fetch("/api/admin/sites");
  if (!res.ok) return;
  const data = await res.json();
  const sites = data.sites || [];
  root.replaceChildren();
  if (!sites.length) {
    root.textContent = "No other homes yet.";
    return;
  }
  for (const site of sites) {
    const row = document.createElement("form");
    row.className = "toggle-row";
    row.innerHTML = `<span>${site.name} <code>/${site.slug}</code></span>`;
    const input = document.createElement("input");
    input.type = "email";
    input.value = site.adminEmail || "";
    input.required = true;
    const button = document.createElement("button");
    button.type = "submit";
    button.className = "toolbar-btn";
    button.textContent = "Save admin";
    row.append(input, button);
    row.addEventListener("submit", async (event) => {
      event.preventDefault();
      const save = await fetch(`/api/admin/sites/${encodeURIComponent(site.slug)}/admin`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ email: input.value }),
      });
      if (!save.ok) {
        alert((await save.json().catch(() => ({}))).error || "Could not save");
        return;
      }
      button.textContent = "Saved";
    });
    root.append(row);
  }
}

function renderSettings(settings) {
  const allowNew = document.getElementById("allowNewAccounts");
  const allowPk = document.getElementById("allowPasskeyEnrollment");
  allowNew.checked = !!settings.allowNewAccounts;
  allowPk.checked = !!settings.allowPasskeyEnrollment;

  async function save() {
    const res = await fetch(adminPath("/api/admin/settings"), {
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

    const colorTd = document.createElement("td");
    colorTd.className = "pie-color-cell";
    const color = document.createElement("input");
    color.type = "color";
    color.className = "pie-color";
    color.value = /^#[0-9a-fA-F]{6}$/.test(load.color || "") ? load.color : "#90a4ae";
    color.title = "Doughnut slice colour";
    color.addEventListener("change", () => savePieColor(key, color.value));
    colorTd.appendChild(color);

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

    tr.append(nameTd, colorTd, entityTd, kwhTd, invTd, gridTd, includeTd, renderMergeCell(load, list));
    tbody.appendChild(tr);
  }
}

async function saveLoadSource(key, source) {
  const res = await fetch(adminPath("/api/admin/settings"), {
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
  const res = await fetch(adminPath("/api/admin/settings"), {
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

async function savePieColor(key, color) {
  return postPieSettings({ pieColor: { key, color } });
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
  const res = await fetch(adminPath(`/api/admin/devices/${encodeURIComponent(entityId)}/acl`), {
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

function renderGateways(gateways, events) {
  const tbody = document.querySelector("#zbgwTable tbody");
  const empty = document.getElementById("zbgwEmpty");
  tbody.replaceChildren();
  empty.hidden = gateways.length > 0;

  for (const gw of gateways) {
    const tr = document.createElement("tr");
    if (gw.device_id === selectedGateway) tr.classList.add("is-selected");
    tr.addEventListener("click", (ev) => {
      if (ev.target.closest("button")) return;
      selectedGateway = selectedGateway === gw.device_id ? "" : gw.device_id;
      load().catch(console.error);
    });

    const age = formatAge(gw.age_s);
    const state = gatewayState(gw);
    const mqtt = gw.mqtt_ok ? "up" : "down";
    const zig = gw.zigbee_ok ? "up" : "down";
    const pending = gw.pending_restart ? "Queued" : "Restart";

    tr.innerHTML = `
      <td class="entity-id">${escapeHtml(gw.device_id)}</td>
      <td>${escapeHtml(age)}</td>
      <td><span class="status-pill ${state.cls}">${escapeHtml(state.label)}</span></td>
      <td>${escapeHtml(gw.fw || "—")}</td>
      <td>${escapeHtml(formatUptime(gw.uptime_s))}</td>
      <td>${escapeHtml(`${mqtt} / ${zig}`)}</td>
      <td class="admin-actions"></td>
    `;
    const actions = tr.querySelector(".admin-actions");
    const btn = actionBtn(pending, () => restartGateway(gw.device_id));
    if (gw.pending_restart) btn.disabled = true;
    actions.appendChild(btn);
    tbody.appendChild(tr);
  }

  const wrap = document.getElementById("zbgwEventsWrap");
  const evBody = document.querySelector("#zbgwEventsTable tbody");
  evBody.replaceChildren();
  wrap.hidden = events.length === 0;
  for (const ev of events) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escapeHtml(formatDate(new Date(ev.ts).toISOString()))}</td>
      <td class="entity-id">${escapeHtml(ev.device_id)}</td>
      <td>${escapeHtml(ev.kind)}</td>
      <td>${escapeHtml(ev.code && ev.message ? `${ev.code}: ${ev.message}` : ev.message || ev.code || "—")}</td>
    `;
    evBody.appendChild(tr);
  }
}

function gatewayState(gw) {
  if (gw.last_kind === "error" || gw.last_ok === 0) {
    return { cls: "denied", label: gw.last_code || "error" };
  }
  if ((gw.age_s || 0) > 2 * 60 * 60) {
    return { cls: "pending", label: "quiet" };
  }
  if (gw.last_kind === "ok") {
    return { cls: "on", label: "ok" };
  }
  return { cls: "on", label: gw.last_kind || "seen" };
}

function formatAge(ageS) {
  const n = Number(ageS);
  if (!Number.isFinite(n)) return "—";
  if (n < 90) return `${n}s`;
  if (n < 90 * 60) return `${Math.round(n / 60)}m`;
  return `${(n / 3600).toFixed(1)}h`;
}

function formatUptime(sec) {
  const n = Number(sec);
  if (!Number.isFinite(n) || n < 0) return "—";
  if (n < 3600) return `${Math.round(n / 60)}m`;
  if (n < 48 * 3600) return `${(n / 3600).toFixed(1)}h`;
  return `${(n / 86400).toFixed(1)}d`;
}

async function restartGateway(deviceId) {
  const res = await fetch(`/api/admin/zbgw/${encodeURIComponent(deviceId)}/restart`, {
    method: "POST",
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "Restart failed");
    return;
  }
  flashSaved("zbgwSaved");
  load().catch(console.error);
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

document.getElementById("fwUpload")?.addEventListener("click", async () => {
  const input = document.getElementById("fwFile");
  const file = input?.files?.[0];
  if (!file) {
    alert("Choose zigbee-gateway.bin first");
    return;
  }
  const res = await fetch("/api/admin/fw/zigbee-gateway", {
    method: "PUT",
    headers: { "Content-Type": "application/octet-stream" },
    body: file,
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "Upload failed");
    return;
  }
  renderFw(await res.json());
  flashSaved("fwSaved");
});

load().catch((err) => {
  console.error(err);
  alert("Failed to load admin data");
});
