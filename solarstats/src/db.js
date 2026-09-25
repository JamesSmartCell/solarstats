import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";
import { ensureHaFieldTables, resolveInverterFields, upsertHaCatalog } from "./ha_fields.js";

const MAX_GAP_MS = 5 * 60 * 1000;

/** Admin email from ADMIN_EMAIL in the site .env (lowercased). */
export function getAdminEmail() {
  return String(process.env.ADMIN_EMAIL || "")
    .trim()
    .toLowerCase();
}

export function isAdminEmail(email) {
  const admin = getAdminEmail();
  if (!admin) return false;
  return String(email || "").trim().toLowerCase() === admin;
}

export const LOAD_DEFS = [
  { key: "officePc", label: "Office PC", color: "#42a5f5", defaultSource: "grid", entityId: "sensor.office_pc_synth_energy_daily", powerEntityId: "sensor.smart_socket_2_power" },
  { key: "frontRoomPc", label: "Front Room PC", color: "#5c6bc0", defaultSource: "grid", entityId: "sensor.front_room_pc_synth_energy_daily", powerEntityId: "sensor.smart_socket_power" },
  { key: "pi5", label: "Pi5 Server", color: "#7e57c2", defaultSource: "grid", entityId: "sensor.pi5_server_energy_daily_2", powerEntityId: "sensor.ts011f_power" },
  { key: "motorbike", label: "Motorbike", color: "#26a69a", defaultSource: "grid", entityId: "sensor.motorbike_charger_energy_daily_2", powerEntityId: "sensor.zigbeesensor_power" },
  { key: "fridge", label: "Fridge", color: "#66bb6a", defaultSource: "grid", entityId: "sensor.fridge_energy_daily_2", powerEntityId: "sensor.kitchen_refrigerator_power" },
  { key: "washingMachine", label: "Washing machine", color: "#8bc34a", defaultSource: "inverter", entityId: "sensor.inverter_loads", powerEntityId: "sensor.laundry_room_washer_power_approx" },
  { key: "otherInverter", label: "Other inverter", color: "#cddc39", defaultSource: "inverter", entityId: "sensor.inverter_unmetered", powerEntityId: "sensor.inverter_unmetered_power" },
];

/** Extra daily-energy sensors → their live power (W) entity. */
const EXTRA_POWER_BY_ENERGY_ID = {
  "sensor.router_energy_daily": "sensor.zigbeesensor_power_2",
  "sensor.tv_energy_daily": "sensor.ts011f_power_2",
  "sensor.white_robot_energy_daily": "sensor.ts011f_power_5",
  "sensor.living_room_charger_energy_daily": "sensor.ts011f_power_4",
};

const LOAD_KEYS = LOAD_DEFS.map((d) => d.key);
const BUILTIN_LOAD_ENTITY_IDS = new Set(LOAD_DEFS.map((d) => d.entityId).filter(Boolean));

const EXTRA_PIE_COLORS = [
  "#ef5350",
  "#ab47bc",
  "#26c6da",
  "#ffa726",
  "#8d6e63",
  "#ec407a",
  "#42a5f5",
  "#26a69a",
  "#9ccc65",
  "#ffca28",
  "#7e57c2",
  "#78909c",
];

function colorForKey(key) {
  const s = String(key || "");
  let hash = 0;
  for (let i = 0; i < s.length; i++) hash = (hash * 31 + s.charCodeAt(i)) >>> 0;
  return EXTRA_PIE_COLORS[hash % EXTRA_PIE_COLORS.length];
}

export function isEnergySensor(device) {
  const entityId = String(device?.entityId || device?.entity_id || "");
  const domain = String(device?.domain || entityId.split(".")[0] || "");
  if (domain !== "sensor") return false;
  const deviceClass = String(device?.deviceClass || device?.device_class || "").toLowerCase();
  if (deviceClass === "power") return false;
  if (deviceClass === "energy") return true;
  const unit = String(device?.unit || "").toLowerCase().replace(/\s+/g, "");
  if (unit === "kwh" || unit === "wh" || unit === "mwh") return true;
  return /(^|[._])energy([._]|$)/i.test(entityId);
}

/** Daily synthetic / utility-meter energy sensors suitable for the load pie. */
export function isPieEnergyCandidate(device) {
  const entityId = String(device?.entityId || device?.entity_id || "");
  if (!entityId || BUILTIN_LOAD_ENTITY_IDS.has(entityId)) return false;
  if (!isEnergySensor(device)) return false;
  if (/(yesterday|from_power|snapshot|latched|derived|washer_energy$)/i.test(entityId)) {
    return false;
  }
  if (/(solar_production|inverter_supply|grid_loads_energy|house_metered|inverter_unmetered)/i.test(entityId)) {
    return false;
  }
  return /energy_daily|synth_energy|daily/i.test(entityId);
}

const DEFAULT_LOAD_SOURCES = Object.fromEntries(LOAD_DEFS.map((d) => [d.key, d.defaultSource]));

export function openDatabase(dbPath) {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");

  db.exec(`
    CREATE TABLE IF NOT EXISTS samples (
      ts INTEGER PRIMARY KEY,
      grid_voltage REAL,
      pv_voltage REAL,
      battery_voltage REAL,
      battery_soc REAL,
      battery_charge_current REAL,
      load_percent REAL,
      ac_frequency REAL,
      pv_power REAL,
      battery_discharge_current REAL,
      output_power REAL,
      energy_kwh_cumulative REAL NOT NULL,
      loads_daily_kwh TEXT
    );

    CREATE TABLE IF NOT EXISTS meta (
      key TEXT PRIMARY KEY,
      value TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS users (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      email TEXT NOT NULL UNIQUE COLLATE NOCASE,
      role TEXT NOT NULL DEFAULT 'user',
      status TEXT NOT NULL DEFAULT 'pending',
      display_name TEXT,
      created_at TEXT NOT NULL,
      approved_at TEXT
    );

    CREATE TABLE IF NOT EXISTS passkeys (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id INTEGER NOT NULL,
      credential_id TEXT NOT NULL UNIQUE,
      public_key TEXT NOT NULL,
      counter INTEGER NOT NULL DEFAULT 0,
      transports TEXT,
      created_at TEXT NOT NULL,
      FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS ha_devices (
      entity_id TEXT PRIMARY KEY,
      domain TEXT NOT NULL,
      name TEXT NOT NULL,
      allow_users INTEGER NOT NULL DEFAULT 1,
      allow_admin INTEGER NOT NULL DEFAULT 1,
      state TEXT,
      updated_at TEXT
    );

    CREATE TABLE IF NOT EXISTS device_commands (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      entity_id TEXT NOT NULL,
      action TEXT NOT NULL,
      status TEXT NOT NULL DEFAULT 'pending',
      requested_by INTEGER,
      created_at TEXT NOT NULL,
      completed_at TEXT
    );

    CREATE TABLE IF NOT EXISTS pie_merges (
      child_key TEXT PRIMARY KEY,
      parent_key TEXT NOT NULL
    );
  `);

  ensureColumn(db, "samples", "loads_daily_kwh", "TEXT");
  ensureColumn(db, "ha_devices", "device_class", "TEXT");
  ensureColumn(db, "ha_devices", "unit", "TEXT");
  ensureHaFieldTables(db);

  if (getMeta(db, "allow_new_accounts") == null) {
    setMeta(db, "allow_new_accounts", "1");
  }
  if (getMeta(db, "allow_passkey_enrollment") == null) {
    setMeta(db, "allow_passkey_enrollment", "0");
  }

  seedAdmin(db);
  return db;
}

function ensureColumn(db, table, column, type) {
  const cols = db.prepare(`PRAGMA table_info(${table})`).all();
  if (!cols.some((c) => c.name === column)) {
    db.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${type}`);
  }
}

function seedAdmin(db) {
  const adminEmail = getAdminEmail();
  if (!adminEmail) {
    console.warn(
      "ADMIN_EMAIL is not set — no admin user will be seeded. Set it in .env.",
    );
    return;
  }
  const now = new Date().toISOString();
  db.prepare(
    `INSERT INTO users (email, role, status, display_name, created_at, approved_at)
     VALUES (?, 'admin', 'approved', 'Admin', ?, ?)
     ON CONFLICT(email) DO UPDATE SET
       role = 'admin',
       status = 'approved',
       approved_at = COALESCE(users.approved_at, excluded.approved_at)`,
  ).run(adminEmail, now, now);
}

/** Pack voltage below this means the inverter is not talking (not a real 48/24/12V reading). */
const MIN_LIVE_BATTERY_V = 8;

function isUnavailableValue(value) {
  if (value == null) return true;
  const s = String(value).trim().toLowerCase();
  return !s || s === "unavailable" || s === "unknown" || s === "none" || s === "null";
}

function toNumber(value) {
  if (isUnavailableValue(value)) return null;
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function holdNumber(incoming, previous) {
  return incoming != null ? incoming : previous ?? null;
}

function isLiveInverter({ batteryVoltage, batterySoc, pvPower, outputPower }) {
  if (batteryVoltage != null && batteryVoltage >= MIN_LIVE_BATTERY_V) return true;
  if (batterySoc != null && batterySoc > 1) return true;
  if (pvPower != null && pvPower > 0) return true;
  if (outputPower != null && outputPower > 0) return true;
  return false;
}

function isDeadSampleRow(row) {
  if (!row) return true;
  return !isLiveInverter({
    batteryVoltage: row.battery_voltage,
    batterySoc: row.battery_soc,
    pvPower: row.pv_power,
    outputPower: row.output_power,
  });
}

export function getMeta(db, key) {
  const row = db.prepare("SELECT value FROM meta WHERE key = ?").get(key);
  return row ? row.value : null;
}

export function setMeta(db, key, value) {
  db.prepare(
    `INSERT INTO meta (key, value) VALUES (?, ?)
     ON CONFLICT(key) DO UPDATE SET value = excluded.value`,
  ).run(key, String(value));
}

export function getSettingBool(db, key, defaultValue = false) {
  const raw = getMeta(db, key);
  if (raw == null) return defaultValue;
  return raw === "1" || raw === "true" || raw === "yes";
}

export function setSettingBool(db, key, value) {
  setMeta(db, key, value ? "1" : "0");
}

export function getAuthSettings(db) {
  return {
    allowNewAccounts: getSettingBool(db, "allow_new_accounts", true),
    allowPasskeyEnrollment: getSettingBool(db, "allow_passkey_enrollment", false),
  };
}

export function setAuthSettings(db, patch) {
  if (patch.allowNewAccounts != null) {
    setSettingBool(db, "allow_new_accounts", !!patch.allowNewAccounts);
  }
  if (patch.allowPasskeyEnrollment != null) {
    setSettingBool(db, "allow_passkey_enrollment", !!patch.allowPasskeyEnrollment);
  }
  return getAuthSettings(db);
}

export function getLoadSources(db) {
  const raw = getMeta(db, "load_sources");
  let parsed = {};
  try {
    parsed = raw ? JSON.parse(raw) : {};
  } catch {
    parsed = {};
  }
  const out = { ...DEFAULT_LOAD_SOURCES };
  if (parsed && typeof parsed === "object") {
    for (const [key, v] of Object.entries(parsed)) {
      if (v === "inverter" || v === "grid") out[key] = v;
    }
  }
  return out;
}

export function setLoadSources(db, patch) {
  const current = getLoadSources(db);
  if (patch && typeof patch === "object") {
    for (const [key, v] of Object.entries(patch)) {
      const id = String(key || "").trim();
      if (!id) continue;
      if (v === "inverter" || v === "grid") current[id] = v;
    }
  }
  setMeta(db, "load_sources", JSON.stringify(current));
  return current;
}

export function getPieExtraIds(db) {
  const raw = getMeta(db, "pie_extras");
  let parsed = [];
  try {
    parsed = raw ? JSON.parse(raw) : [];
  } catch {
    parsed = [];
  }
  if (!Array.isArray(parsed)) return [];
  return [...new Set(parsed.map((id) => String(id || "").trim()).filter(Boolean))];
}

function normalizeHexColor(value) {
  const s = String(value || "").trim();
  const short = /^#([0-9a-fA-F]{3})$/.exec(s);
  if (short) {
    const [r, g, b] = short[1];
    return `#${r}${r}${g}${g}${b}${b}`.toLowerCase();
  }
  const full = /^#([0-9a-fA-F]{6})$/.exec(s);
  return full ? `#${full[1]}`.toLowerCase() : null;
}

export function getPieColors(db) {
  const raw = getMeta(db, "pie_colors");
  let parsed = {};
  try {
    parsed = raw ? JSON.parse(raw) : {};
  } catch {
    parsed = {};
  }
  const out = {};
  if (parsed && typeof parsed === "object") {
    for (const [key, v] of Object.entries(parsed)) {
      const hex = normalizeHexColor(v);
      if (key && hex) out[key] = hex;
    }
  }
  return out;
}

export function setPieColor(db, key, color) {
  const id = String(key || "").trim();
  const hex = normalizeHexColor(color);
  if (!id || !hex) {
    const err = new Error("A valid #RGB or #RRGGBB colour is required");
    err.status = 400;
    throw err;
  }
  const colors = getPieColors(db);
  colors[id] = hex;
  setMeta(db, "pie_colors", JSON.stringify(colors));
  return colors;
}

export function getPieVisibility(db) {
  const raw = getMeta(db, "pie_visibility");
  let parsed = {};
  try {
    parsed = raw ? JSON.parse(raw) : {};
  } catch {
    parsed = {};
  }
  const out = {};
  if (parsed && typeof parsed === "object") {
    for (const [key, v] of Object.entries(parsed)) {
      if (key) out[key] = !!v;
    }
  }
  for (const id of getPieExtraIds(db)) {
    if (!(id in out)) out[id] = true;
  }
  return out;
}

function isPieVisible(visibility, key, builtin) {
  if (key in visibility) return !!visibility[key];
  return !!builtin;
}

export function getPieMerges(db) {
  const rows = db
    .prepare(`SELECT child_key, parent_key FROM pie_merges ORDER BY parent_key, child_key`)
    .all();
  const childrenByParent = {};
  const parentByChild = {};
  for (const row of rows) {
    const child = String(row.child_key || "").trim();
    const parent = String(row.parent_key || "").trim();
    if (!child || !parent) continue;
    parentByChild[child] = parent;
    if (!childrenByParent[parent]) childrenByParent[parent] = [];
    childrenByParent[parent].push(child);
  }
  return { childrenByParent, parentByChild };
}

function mergeError(code, message) {
  const err = new Error(message);
  err.status = 400;
  err.code = code;
  return err;
}

export function addPieMerge(db, parentKey, childKey) {
  const parent = String(parentKey || "").trim();
  const child = String(childKey || "").trim();
  if (!parent || !child) {
    throw mergeError("invalid_merge", "Parent and child are required");
  }
  if (parent === child) {
    throw mergeError("invalid_merge", "A feed cannot merge into itself");
  }

  const { childrenByParent, parentByChild } = getPieMerges(db);
  if (parentByChild[parent]) {
    throw mergeError("parent_is_child", "Cannot merge into a feed that is already merged");
  }
  if (childrenByParent[child]?.length) {
    throw mergeError("child_is_parent", "Unmerge this feed's children before merging it into another");
  }

  db.prepare(
    `INSERT INTO pie_merges (child_key, parent_key) VALUES (?, ?)
     ON CONFLICT(child_key) DO UPDATE SET parent_key = excluded.parent_key`,
  ).run(child, parent);
  return getPieMerges(db);
}

export function removePieMerge(db, childKey) {
  const child = String(childKey || "").trim();
  if (!child) {
    throw mergeError("invalid_merge", "Child is required");
  }
  db.prepare(`DELETE FROM pie_merges WHERE child_key = ?`).run(child);
  return getPieMerges(db);
}

function isPowerSensor(device) {
  const entityId = String(device?.entityId || device?.entity_id || "");
  const domain = String(device?.domain || entityId.split(".")[0] || "");
  if (domain !== "sensor") return false;
  if (isEnergySensor(device)) return false;
  const deviceClass = String(device?.deviceClass || device?.device_class || "").toLowerCase();
  if (deviceClass === "power") return true;
  const unit = String(device?.unit || "").toLowerCase().replace(/\s+/g, "");
  if (unit === "w" || unit === "kw" || unit === "mw") return true;
  return /(^|[._])power(_\d+)?$/i.test(entityId);
}

function nameStem(value) {
  return String(value || "")
    .toLowerCase()
    .replace(/energy[_\s-]*(daily|from[_\s-]*power|yesterday).*$/g, "")
    .replace(/\bsynth\b/g, "")
    .replace(/[^a-z0-9]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

function resolvePowerEntityId(row, devicesById, powerSensors) {
  const mapped =
    row.powerEntityId ||
    EXTRA_POWER_BY_ENERGY_ID[row.entityId] ||
    EXTRA_POWER_BY_ENERGY_ID[row.key];
  if (mapped && devicesById.has(mapped)) return mapped;
  if (mapped) return mapped;

  const stem = nameStem(row.label) || nameStem(row.entityId || row.key);
  if (!stem) return null;

  const exact = powerSensors.find((d) => nameStem(d.name) === stem);
  if (exact) return exact.entityId;

  const fromEntity = powerSensors.find((d) => {
    const idStem = nameStem(String(d.entityId || "").replace(/_power(_\d+)?$/i, ""));
    return idStem && idStem === stem;
  });
  return fromEntity?.entityId || null;
}

export function getLatestLoadsPower(db) {
  const devices = listAllDevices(db);
  const devicesById = new Map(devices.map((d) => [d.entityId, d]));
  const powerSensors = devices.filter(isPowerSensor);
  const out = {};

  for (const def of LOAD_DEFS) {
    const powerId = resolvePowerEntityId(def, devicesById, powerSensors);
    out[def.key] = powerId ? toNumber(devicesById.get(powerId)?.state) : null;
  }

  for (const device of devices) {
    if (!isPieEnergyCandidate(device)) continue;
    const row = {
      key: device.entityId,
      label: device.name || device.entityId,
      entityId: device.entityId,
    };
    const powerId = resolvePowerEntityId(row, devicesById, powerSensors);
    out[device.entityId] = powerId ? toNumber(devicesById.get(powerId)?.state) : null;
  }

  return out;
}

export function setPieExtra(db, entityId, onPie) {
  const id = String(entityId || "").trim();
  if (!id) return getPieExtraIds(db);
  const visibility = getPieVisibility(db);
  visibility[id] = !!onPie;
  setMeta(db, "pie_visibility", JSON.stringify(visibility));

  const ids = new Set(getPieExtraIds(db));
  if (onPie) ids.add(id);
  else ids.delete(id);
  const next = [...ids];
  setMeta(db, "pie_extras", JSON.stringify(next));
  return next;
}

function usesBuiltinLoads(db) {
  return getMeta(db, "use_builtin_loads") !== "0";
}

export function getPieAdminRows(db) {
  const latest = getLatestLoadsDaily(db) || {};
  const power = getLatestLoadsPower(db);
  const sources = getLoadSources(db);
  const visibility = getPieVisibility(db);
  const colors = getPieColors(db);
  const { childrenByParent, parentByChild } = getPieMerges(db);
  const labelByKey = Object.fromEntries(LOAD_DEFS.map((d) => [d.key, d.label]));
  const showBuiltins = usesBuiltinLoads(db);

  const builtin = (showBuiltins ? LOAD_DEFS : []).map((d) => ({
    key: d.key,
    label: d.label,
    color: colors[d.key] || d.color,
    source: sources[d.key] || d.defaultSource,
    entityId: d.entityId,
    powerEntityId: d.powerEntityId || null,
    builtin: true,
    onPie: isPieVisible(visibility, d.key, true),
    kwh: latest[d.key] ?? null,
    watts: power[d.key] ?? null,
  }));

  const extras = listAllDevices(db)
    .filter((d) => isPieEnergyCandidate(d))
    .map((d) => {
      labelByKey[d.entityId] = d.name || d.entityId;
      return {
        key: d.entityId,
        label: d.name || d.entityId,
        color: colors[d.entityId] || colorForKey(d.entityId),
        source: sources[d.entityId] || "grid",
        entityId: d.entityId,
        powerEntityId: EXTRA_POWER_BY_ENERGY_ID[d.entityId] || null,
        builtin: false,
        onPie: isPieVisible(visibility, d.entityId, !showBuiltins),
        kwh: latest[d.entityId] ?? toNumber(d.state),
        watts: power[d.entityId] ?? null,
      };
    })
    .sort((a, b) => String(a.label).localeCompare(String(b.label)));

  const rows = [...builtin, ...extras];
  const rowByKey = new Map(rows.map((row) => [row.key, row]));

  return rows.map((row) => {
    const parentKey = parentByChild[row.key] || null;
    const parentRow = parentKey ? rowByKey.get(parentKey) : null;
    const childKeys = childrenByParent[row.key] || [];
    return {
      ...row,
      mergedInto: parentKey
        ? { key: parentKey, label: parentRow?.label || labelByKey[parentKey] || parentKey }
        : null,
      mergeChildren: childKeys.map((key) => ({
        key,
        label: rowByKey.get(key)?.label || labelByKey[key] || key,
      })),
    };
  });
}

export function getLoadConfig(db) {
  return getPieAdminRows(db)
    .filter((row) => row.onPie && !row.mergedInto)
    .map((row) => ({
      key: row.key,
      label: row.label,
      color: row.color,
      source: row.source,
      entityId: row.entityId,
      builtin: row.builtin,
      onPie: true,
      kwh: row.kwh,
      watts: row.watts,
      members: row.mergeChildren.map((child) => child.key),
    }));
}

function normalizeEmail(email) {
  return String(email || "")
    .trim()
    .toLowerCase();
}

export function getUserById(db, id) {
  return db.prepare("SELECT * FROM users WHERE id = ?").get(id) || null;
}

export function getUserByEmail(db, email) {
  const e = normalizeEmail(email);
  if (!e) return null;
  return db.prepare("SELECT * FROM users WHERE email = ? COLLATE NOCASE").get(e) || null;
}

export function listUsers(db) {
  return db
    .prepare(
      `SELECT id, email, role, status, display_name, created_at, approved_at
       FROM users ORDER BY
         CASE status WHEN 'pending' THEN 0 WHEN 'approved' THEN 1 ELSE 2 END,
         created_at ASC`,
    )
    .all();
}

/**
 * After Microsoft login: return { user, outcome } where outcome is
 * approved | pending | denied | registration_closed
 */
export function upsertMicrosoftUser(db, { email, displayName }) {
  const e = normalizeEmail(email);
  if (!e) {
    throw new Error("missing email claim");
  }

  const existing = getUserByEmail(db, e);
  const now = new Date().toISOString();

  if (existing) {
    if (displayName && displayName !== existing.display_name) {
      db.prepare("UPDATE users SET display_name = ? WHERE id = ?").run(
        displayName,
        existing.id,
      );
    }
    return { user: getUserById(db, existing.id), outcome: existing.status };
  }

  if (isAdminEmail(e)) {
    seedAdmin(db);
    const admin = getUserByEmail(db, e);
    if (displayName) {
      db.prepare("UPDATE users SET display_name = ? WHERE id = ?").run(
        displayName,
        admin.id,
      );
    }
    return { user: getUserById(db, admin.id), outcome: "approved" };
  }

  if (!getSettingBool(db, "allow_new_accounts", true)) {
    return { user: null, outcome: "registration_closed" };
  }

  const info = db
    .prepare(
      `INSERT INTO users (email, role, status, display_name, created_at, approved_at)
       VALUES (?, 'user', 'pending', ?, ?, NULL)`,
    )
    .run(e, displayName || null, now);

  return {
    user: getUserById(db, info.lastInsertRowid),
    outcome: "pending",
  };
}

/** Approve an invited email without making them the global admin. */
export function ensureApprovedUser(db, { email, displayName }) {
  const e = normalizeEmail(email);
  if (!e || !e.includes("@")) {
    const err = new Error("invalid_email");
    err.status = 400;
    throw err;
  }
  const existing = getUserByEmail(db, e);
  const now = new Date().toISOString();
  if (existing) {
    if (existing.status !== "approved") setUserStatus(db, existing.id, "approved");
    if (displayName && displayName !== existing.display_name) {
      db.prepare("UPDATE users SET display_name = ? WHERE id = ?").run(displayName, existing.id);
    }
    return getUserById(db, existing.id);
  }
  const info = db
    .prepare(
      `INSERT INTO users (email, role, status, display_name, created_at, approved_at)
       VALUES (?, 'user', 'approved', ?, ?, ?)`,
    )
    .run(e, displayName || null, now, now);
  return getUserById(db, info.lastInsertRowid);
}

export function setUserStatus(db, userId, status) {
  const now = new Date().toISOString();
  const approvedAt = status === "approved" ? now : null;
  db.prepare(
    `UPDATE users SET status = ?, approved_at = CASE
       WHEN ? = 'approved' THEN COALESCE(approved_at, ?)
       ELSE approved_at
     END WHERE id = ?`,
  ).run(status, status, approvedAt, userId);
  return getUserById(db, userId);
}

export function listPasskeysForUser(db, userId) {
  return db
    .prepare(
      `SELECT id, user_id, credential_id, public_key, counter, transports, created_at
       FROM passkeys WHERE user_id = ?`,
    )
    .all(userId);
}

export function getPasskeyByCredentialId(db, credentialId) {
  return (
    db
      .prepare("SELECT * FROM passkeys WHERE credential_id = ?")
      .get(credentialId) || null
  );
}

export function insertPasskey(db, { userId, credentialId, publicKey, counter, transports }) {
  const now = new Date().toISOString();
  db.prepare(
    `INSERT INTO passkeys (user_id, credential_id, public_key, counter, transports, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`,
  ).run(
    userId,
    credentialId,
    publicKey,
    counter ?? 0,
    transports ? JSON.stringify(transports) : null,
    now,
  );
}

export function updatePasskeyCounter(db, credentialId, counter) {
  db.prepare("UPDATE passkeys SET counter = ? WHERE credential_id = ?").run(
    counter,
    credentialId,
  );
}

export function deletePasskey(db, id, userId) {
  return db
    .prepare("DELETE FROM passkeys WHERE id = ? AND user_id = ?")
    .run(id, userId).changes;
}

export function getEnergyTotal(db) {
  const raw = getMeta(db, "energy_kwh_total");
  const n = Number(raw);
  return Number.isFinite(n) ? n : 0;
}

function loadKeysOf(...maps) {
  const keys = new Set(LOAD_KEYS);
  for (const map of maps) {
    if (!map || typeof map !== "object") continue;
    for (const key of Object.keys(map)) {
      if (key && key !== "__proto__") keys.add(key);
    }
  }
  return keys;
}

function energyKwh(device) {
  const n = toNumber(device?.state);
  if (n == null) return null;
  const unit = String(device?.unit || "").toLowerCase().replace(/\s+/g, "");
  if (unit === "wh") return n / 1000;
  if (unit === "mwh") return n * 1000;
  return n;
}

function loadsFromDevices(devices) {
  if (!Array.isArray(devices) || !devices.length) return null;
  const byEntity = new Map(LOAD_DEFS.map((d) => [d.entityId, d.key]));
  const out = {};
  let any = false;
  for (const d of devices) {
    const id = String(d?.entity_id || d?.entityId || "");
    if (!id) continue;
    const row = {
      entityId: id,
      domain: d.domain || id.split(".")[0],
      deviceClass: d.device_class || d.deviceClass,
      unit: d.unit,
      state: d.state,
      name: d.name,
    };
    if (!isEnergySensor(row)) continue;
    const n = energyKwh(row);
    if (n == null) continue;
    const key = byEntity.get(id) || (isPieEnergyCandidate(row) ? id : null);
    if (!key) continue;
    out[key] = n;
    any = true;
  }
  return any ? out : null;
}

function parseLoadsDaily(payload) {
  const src = payload.loadsDailyKwh || payload.loads_daily_kwh || null;
  const named = {};
  let anyNamed = false;
  if (src && typeof src === "object") {
    for (const key of loadKeysOf(src)) {
      const n = toNumber(src[key]);
      named[key] = n;
      if (n != null) anyNamed = true;
    }
  }
  const fromDevices = loadsFromDevices(payload.devices);
  if (!anyNamed && !fromDevices) return null;
  return { ...(fromDevices || {}), ...(anyNamed ? named : {}) };
}

/** Keep last-good daily kWh when a meter drops to unavailable/0 mid-day. Accept a true midnight reset. */
function mergeLoadsDaily(incoming, previous) {
  if (!incoming && !previous) return null;
  if (!incoming) return previous;
  if (!previous) return incoming;

  const keys = loadKeysOf(incoming, previous);
  let prevPositive = 0;
  let incomingNearZero = 0;
  for (const key of keys) {
    if ((previous[key] ?? 0) > 0.05) prevPositive += 1;
    if ((incoming[key] ?? 0) < 0.02) incomingNearZero += 1;
  }
  const midnightReset = prevPositive >= 2 && incomingNearZero >= prevPositive;

  const out = { ...previous };
  for (const key of keys) {
    const next = incoming[key];
    const prev = previous[key];
    if (next == null) {
      out[key] = prev ?? null;
      continue;
    }
    if (!midnightReset && next === 0 && (prev ?? 0) > 0.05) {
      out[key] = prev;
      continue;
    }
    out[key] = next;
  }
  return out;
}

function loadsToJson(loads) {
  return loads ? JSON.stringify(loads) : null;
}

function loadsFromRow(row) {
  if (!row?.loads_daily_kwh) return null;
  try {
    return JSON.parse(row.loads_daily_kwh);
  } catch {
    return null;
  }
}

export function insertSample(db, payload) {
  const ts = Date.parse(payload.ts || "") || Date.now();

  const prev = db
    .prepare(`SELECT * FROM samples ORDER BY ts DESC LIMIT 1`)
    .get();

  if (Array.isArray(payload.devices)) {
    upsertDeviceStates(db, payload.devices);
    upsertHaCatalog(db, payload.devices, ts);
  } else if (Array.isArray(payload.states)) {
    upsertDeviceStates(db, payload.states);
    upsertHaCatalog(db, payload.states, ts);
  }

  const resolved = resolveInverterFields(db, payload, ts);
  const incoming = {
    grid_voltage: resolved.values.gridVoltage,
    pv_voltage: resolved.values.pvVoltage,
    battery_voltage: resolved.values.batteryVoltage,
    battery_soc: resolved.values.batterySoc,
    battery_charge_current: resolved.values.batteryChargeCurrent,
    load_percent: resolved.values.loadPercent,
    ac_frequency: resolved.values.acFrequency,
    pv_power: resolved.values.pvPower,
    battery_discharge_current: resolved.values.batteryDischargeCurrent,
    output_power:
      resolved.values.outputPower != null ? Math.max(0, resolved.values.outputPower) : null,
  };
  setMeta(db, "ha_fields_latest", JSON.stringify(resolved.fields));

  const prevLoads = getLatestLoadsDaily(db) || loadsFromRow(prev);
  const loads = mergeLoadsDaily(parseLoadsDaily(payload), prevLoads);
  if (loads) {
    setMeta(db, "loads_daily_kwh_latest", JSON.stringify(loads));
  }

  const live = isLiveInverter({
    batteryVoltage: incoming.battery_voltage,
    batterySoc: incoming.battery_soc,
    pvPower: incoming.pv_power,
    outputPower: incoming.output_power,
  });

  const lastGood =
    prev && !isDeadSampleRow(prev)
      ? prev
      : db
          .prepare(
            `SELECT * FROM samples
             WHERE IFNULL(battery_voltage, 0) >= ?
                OR IFNULL(pv_power, 0) > 0
                OR IFNULL(output_power, 0) > 0
             ORDER BY ts DESC LIMIT 1`,
          )
          .get(MIN_LIVE_BATTERY_V);

  if (!live) {
    // Keep last good tiles/history. Devices + daily-load merge already applied.
    if (lastGood) {
      return {
        skipped: true,
        reason: "unavailable",
        ...sampleToApi(lastGood),
        energyKwhTotal: getEnergyTotal(db),
        loadsDailyKwh: loads || loadsFromRow(lastGood),
        loadsPowerW: getLatestLoadsPower(db),
      };
    }
    return {
      skipped: true,
      reason: "unavailable",
      energyKwhTotal: getEnergyTotal(db),
      loadsDailyKwh: loads,
      loadsPowerW: getLatestLoadsPower(db),
    };
  }

  const sample = {
    ts,
    grid_voltage: holdNumber(incoming.grid_voltage, lastGood?.grid_voltage),
    pv_voltage: holdNumber(incoming.pv_voltage, lastGood?.pv_voltage),
    battery_voltage: holdNumber(incoming.battery_voltage, lastGood?.battery_voltage),
    battery_soc: holdNumber(incoming.battery_soc, lastGood?.battery_soc),
    battery_charge_current: holdNumber(
      incoming.battery_charge_current,
      lastGood?.battery_charge_current,
    ),
    load_percent: holdNumber(incoming.load_percent, lastGood?.load_percent),
    ac_frequency: holdNumber(incoming.ac_frequency, lastGood?.ac_frequency),
    pv_power: holdNumber(incoming.pv_power, lastGood?.pv_power),
    battery_discharge_current: holdNumber(
      incoming.battery_discharge_current,
      lastGood?.battery_discharge_current,
    ),
    output_power: holdNumber(incoming.output_power, lastGood?.output_power),
    energy_kwh_cumulative: lastGood ? lastGood.energy_kwh_cumulative : 0,
    loads_daily_kwh: loadsToJson(loads),
  };

  let cumulative = sample.energy_kwh_cumulative;
  let total = getEnergyTotal(db);

  // Only integrate when this tick measured output and the previous row was live.
  if (prev && !isDeadSampleRow(prev) && incoming.output_power != null) {
    const dtMs = ts - prev.ts;
    if (dtMs > 0 && dtMs <= MAX_GAP_MS) {
      const avgPower =
        (Math.max(0, prev.output_power ?? 0) + incoming.output_power) / 2;
      const deltaKwh = (avgPower * (dtMs / 3600000)) / 1000;
      cumulative += deltaKwh;
      total += deltaKwh;
    }
  }

  sample.energy_kwh_cumulative = cumulative;
  setMeta(db, "energy_kwh_total", total);

  db.prepare(
    `INSERT OR REPLACE INTO samples (
      ts, grid_voltage, pv_voltage, battery_voltage, battery_soc,
      battery_charge_current, load_percent, ac_frequency, pv_power,
      battery_discharge_current, output_power, energy_kwh_cumulative,
      loads_daily_kwh
    ) VALUES (
      @ts, @grid_voltage, @pv_voltage, @battery_voltage, @battery_soc,
      @battery_charge_current, @load_percent, @ac_frequency, @pv_power,
      @battery_discharge_current, @output_power, @energy_kwh_cumulative,
      @loads_daily_kwh
    )`,
  ).run(sample);

  const pruned = pruneDeadSamples(db);
  if (pruned) {
    console.warn(`ingest: removed ${pruned} zeroed/unavailable sample(s)`);
  }

  return {
    skipped: false,
    ...sampleToApi(sample),
    energyKwhTotal: total,
    loadsPowerW: getLatestLoadsPower(db),
  };
}

/** Drop snapshots written while HA/MQTT was unavailable (zeros / no pack voltage). */
export function pruneDeadSamples(db, { sinceMs } = {}) {
  const since = sinceMs ?? Date.now() - 14 * 86400000;
  const info = db
    .prepare(
      `DELETE FROM samples
       WHERE ts >= ?
         AND IFNULL(battery_voltage, 0) < ?
         AND IFNULL(battery_soc, 0) <= 1
         AND IFNULL(output_power, 0) = 0
         AND IFNULL(pv_power, 0) = 0`,
    )
    .run(since, MIN_LIVE_BATTERY_V);

  if (info.changes) {
    const last = db.prepare(`SELECT * FROM samples ORDER BY ts DESC LIMIT 1`).get();
    if (last?.loads_daily_kwh) {
      setMeta(db, "loads_daily_kwh_latest", last.loads_daily_kwh);
    }
  }
  return info.changes;
}

export function repairDeadSamples(db) {
  const deleted = pruneDeadSamples(db, { sinceMs: 0 });
  const latest = db.prepare(`SELECT * FROM samples ORDER BY ts DESC LIMIT 1`).get();
  return {
    deleted,
    latest: latest ? sampleToApi(latest) : null,
    energyKwhTotal: getEnergyTotal(db),
    loadsDailyKwh: getLatestLoadsDaily(db),
  };
}

function sampleToApi(row) {
  return {
    ts: new Date(row.ts).toISOString(),
    gridVoltage: row.grid_voltage,
    pvVoltage: row.pv_voltage,
    batteryVoltage: row.battery_voltage,
    batterySoc: row.battery_soc,
    batteryChargeCurrent: row.battery_charge_current,
    loadPercent: row.load_percent,
    acFrequency: row.ac_frequency,
    pvPower: row.pv_power,
    batteryDischargeCurrent: row.battery_discharge_current,
    outputPower: row.output_power,
    energyKwhCumulative: row.energy_kwh_cumulative,
    loadsDailyKwh: loadsFromRow(row),
  };
}

function rangeToMs(range) {
  const match = /^(\d+)([hdw])$/i.exec(range || "24h");
  if (!match) return 24 * 3600000;
  const n = Number(match[1]);
  const unit = match[2].toLowerCase();
  if (unit === "w") return n * 7 * 86400000;
  if (unit === "d") return n * 86400000;
  return n * 3600000;
}

/** Target ~maxPoints by averaging numeric fields in equal-sized buckets. */
function downsampleRows(rows, maxPoints = 1500) {
  if (rows.length <= maxPoints) return rows;
  const bucketSize = Math.ceil(rows.length / maxPoints);
  const out = [];
  const numericKeys = [
    "grid_voltage",
    "pv_voltage",
    "battery_voltage",
    "battery_soc",
    "battery_charge_current",
    "load_percent",
    "ac_frequency",
    "pv_power",
    "battery_discharge_current",
    "output_power",
    "energy_kwh_cumulative",
  ];

  for (let i = 0; i < rows.length; i += bucketSize) {
    const chunk = rows.slice(i, i + bucketSize);
    const avg = { ...chunk[chunk.length - 1] };
    for (const key of numericKeys) {
      let sum = 0;
      let count = 0;
      for (const row of chunk) {
        if (row[key] != null && Number.isFinite(row[key])) {
          sum += row[key];
          count += 1;
        }
      }
      avg[key] = count ? sum / count : null;
    }
    avg.energy_kwh_cumulative = chunk[chunk.length - 1].energy_kwh_cumulative;
    avg.loads_daily_kwh = chunk[chunk.length - 1].loads_daily_kwh;
    avg.ts = chunk[Math.floor(chunk.length / 2)].ts;
    out.push(avg);
  }
  return out;
}

export function getLatestLoadsDaily(db) {
  const raw = getMeta(db, "loads_daily_kwh_latest");
  if (raw) {
    try {
      return JSON.parse(raw);
    } catch {
      /* fall through */
    }
  }
  const latest = db
    .prepare(
      `SELECT loads_daily_kwh FROM samples
       WHERE loads_daily_kwh IS NOT NULL
       ORDER BY ts DESC LIMIT 1`,
    )
    .get();
  return loadsFromRow(latest);
}

export function getHistory(db, range = "24h") {
  const since = Date.now() - rangeToMs(range);
  const rows = db
    .prepare(
      `SELECT * FROM samples
       WHERE ts >= ?
       ORDER BY ts ASC`,
    )
    .all(since);

  const latest = db
    .prepare(`SELECT * FROM samples ORDER BY ts DESC LIMIT 1`)
    .get();

  const reduced = downsampleRows(rows, 1500);
  const total = getEnergyTotal(db);
  const loadsLatest = getLatestLoadsDaily(db);

  return {
    range,
    energyKwhTotal: total,
    latest: latest
      ? {
          ...sampleToApi(latest),
          energyKwhTotal: total,
          loadsDailyKwh: sampleToApi(latest).loadsDailyKwh || loadsLatest,
        }
      : null,
    loadsDailyKwh: loadsLatest,
    loadsPowerW: getLatestLoadsPower(db),
    samples: reduced.map(sampleToApi),
  };
}

export function pruneOldSamples(db, retentionDays) {
  const cutoff = Date.now() - retentionDays * 86400000;
  db.prepare("DELETE FROM samples WHERE ts < ?").run(cutoff);
}

export function listTrackedEntityIds(db) {
  return db.prepare(`SELECT entity_id FROM ha_devices ORDER BY entity_id`).all().map((r) => r.entity_id);
}

export function upsertDeviceStates(db, devices) {
  if (!Array.isArray(devices) || !devices.length) return;
  const now = new Date().toISOString();
  const upsert = db.prepare(
    `INSERT INTO ha_devices (entity_id, domain, name, allow_users, allow_admin, state, updated_at, device_class, unit)
     VALUES (@entity_id, @domain, @name, @allow_users, @allow_admin, @state, @updated_at, @device_class, @unit)
     ON CONFLICT(entity_id) DO UPDATE SET
       state = excluded.state,
       name = COALESCE(excluded.name, ha_devices.name),
       device_class = COALESCE(excluded.device_class, ha_devices.device_class),
       unit = COALESCE(excluded.unit, ha_devices.unit),
       updated_at = excluded.updated_at`,
  );
  const tx = db.transaction((rows) => {
    for (const d of rows) {
      const entityId = String(d?.entity_id || d?.entityId || "").trim();
      if (!entityId) continue;
      const domain = entityId.split(".")[0] || "sensor";
      const state = d.state == null ? null : String(d.state);
      upsert.run({
        entity_id: entityId,
        domain,
        name: d.name || d.friendly_name || entityId,
        allow_users: 0,
        allow_admin: 0,
        state,
        updated_at: now,
        device_class: d.device_class || d.deviceClass || null,
        unit: d.unit || d.unit_of_measurement || null,
      });
    }
  });
  tx(devices);
}

export function countDevices(db) {
  return db.prepare(`SELECT COUNT(*) AS n FROM ha_devices`).get().n;
}

function mapDeviceRow(r) {
  const state = r.state == null ? null : String(r.state);
  return {
    entityId: r.entity_id,
    domain: r.domain,
    name: r.name,
    state,
    on: String(state || "").toLowerCase() === "on",
    deviceClass: r.device_class || null,
    unit: r.unit || null,
    allowUsers: r.allow_users === 1,
    allowAdmin: r.allow_admin === 1,
    updatedAt: r.updated_at,
  };
}

export function listDevicesForViewer(db, { isAdmin }) {
  const rows = db
    .prepare(
      `SELECT entity_id, domain, name, allow_users, allow_admin, state, updated_at, device_class, unit
       FROM ha_devices
       ORDER BY domain ASC, name ASC`,
    )
    .all();
  return rows
    .filter((r) => r.allow_users === 1 || (isAdmin && r.allow_admin === 1))
    .map((r) => mapDeviceRow(r));
}

export function listAllDevices(db) {
  return db
    .prepare(
      `SELECT entity_id, domain, name, allow_users, allow_admin, state, updated_at, device_class, unit
       FROM ha_devices ORDER BY domain ASC, name ASC`,
    )
    .all()
    .map((r) => ({
      ...mapDeviceRow(r),
      exposure:
        r.allow_users === 1 ? "user" : r.allow_admin === 1 ? "admin" : "off",
    }));
}

export function setDeviceAcl(db, entityId, { allowUsers, allowAdmin }) {
  const row = db.prepare(`SELECT entity_id FROM ha_devices WHERE entity_id = ?`).get(entityId);
  if (!row) return null;

  // Modes: user => both audiences; admin => admin only; neither => hidden.
  let users = allowUsers ? 1 : 0;
  let admin = allowAdmin ? 1 : 0;
  if (users) admin = 1;

  db.prepare(
    `UPDATE ha_devices SET allow_users = ?, allow_admin = ? WHERE entity_id = ?`,
  ).run(users, admin, entityId);

  return listAllDevices(db).find((d) => d.entityId === entityId) || null;
}

export function getDevice(db, entityId) {
  return (
    db.prepare(`SELECT * FROM ha_devices WHERE entity_id = ?`).get(entityId) ||
    null
  );
}

export function enqueueDeviceCommand(db, { entityId, action, userId }) {
  const now = new Date().toISOString();
  const info = db
    .prepare(
      `INSERT INTO device_commands (entity_id, action, status, requested_by, created_at)
       VALUES (?, ?, 'pending', ?, ?)`,
    )
    .run(entityId, action, userId ?? null, now);
  return info.lastInsertRowid;
}

export function claimPendingCommands(db, limit = 20) {
  const rows = db
    .prepare(
      `SELECT id, entity_id, action FROM device_commands
       WHERE status = 'pending'
       ORDER BY id ASC
       LIMIT ?`,
    )
    .all(limit);
  if (!rows.length) return [];
  const mark = db.prepare(
    `UPDATE device_commands SET status = 'claimed' WHERE id = ? AND status = 'pending'`,
  );
  const claimed = [];
  const tx = db.transaction((list) => {
    for (const row of list) {
      const r = mark.run(row.id);
      if (r.changes) {
        claimed.push({
          id: row.id,
          entityId: row.entity_id,
          action: row.action,
        });
      }
    }
  });
  tx(rows);
  return claimed;
}

export function completeDeviceCommand(db, id, ok) {
  const now = new Date().toISOString();
  db.prepare(
    `UPDATE device_commands
     SET status = ?, completed_at = ?
     WHERE id = ?`,
  ).run(ok ? "done" : "error", now, id);
}

export { LOAD_KEYS, MIN_LIVE_BATTERY_V };
