import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

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

const LOAD_KEYS = [
  "officePc",
  "frontRoomPc",
  "pi5",
  "motorbike",
  "fridge",
  "washingMachine",
  "otherInverter",
];

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
  `);

  ensureColumn(db, "samples", "loads_daily_kwh", "TEXT");

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

function toNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
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

function parseLoadsDaily(payload) {
  const src = payload.loadsDailyKwh || payload.loads_daily_kwh || null;
  if (!src || typeof src !== "object") return null;
  const out = {};
  let any = false;
  for (const key of LOAD_KEYS) {
    const n = toNumber(src[key]);
    out[key] = n;
    if (n != null) any = true;
  }
  return any ? out : null;
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
  const outputPower = Math.max(0, toNumber(payload.outputPower) ?? 0);
  const loads = parseLoadsDaily(payload);

  const prev = db
    .prepare(
      `SELECT ts, output_power, energy_kwh_cumulative
       FROM samples
       ORDER BY ts DESC
       LIMIT 1`,
    )
    .get();

  let cumulative = prev ? prev.energy_kwh_cumulative : 0;
  let total = getEnergyTotal(db);

  if (prev) {
    const dtMs = ts - prev.ts;
    if (dtMs > 0 && dtMs <= MAX_GAP_MS) {
      const avgPower = (Math.max(0, prev.output_power ?? 0) + outputPower) / 2;
      const deltaKwh = (avgPower * (dtMs / 3600000)) / 1000;
      cumulative += deltaKwh;
      total += deltaKwh;
    }
  }

  setMeta(db, "energy_kwh_total", total);
  if (loads) {
    setMeta(db, "loads_daily_kwh_latest", JSON.stringify(loads));
  }

  const sample = {
    ts,
    grid_voltage: toNumber(payload.gridVoltage),
    pv_voltage: toNumber(payload.pvVoltage),
    battery_voltage: toNumber(payload.batteryVoltage),
    battery_soc: toNumber(payload.batterySoc),
    battery_charge_current: toNumber(payload.batteryChargeCurrent),
    load_percent: toNumber(payload.loadPercent),
    ac_frequency: toNumber(payload.acFrequency),
    pv_power: toNumber(payload.pvPower),
    battery_discharge_current: toNumber(payload.batteryDischargeCurrent),
    output_power: outputPower,
    energy_kwh_cumulative: cumulative,
    loads_daily_kwh: loadsToJson(loads),
  };

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

  return {
    ...sampleToApi(sample),
    energyKwhTotal: total,
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
    samples: reduced.map(sampleToApi),
  };
}

export function pruneOldSamples(db, retentionDays) {
  const cutoff = Date.now() - retentionDays * 86400000;
  db.prepare("DELETE FROM samples WHERE ts < ?").run(cutoff);
}

export { LOAD_KEYS };
