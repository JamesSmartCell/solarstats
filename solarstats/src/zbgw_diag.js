import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

const DEVICE_ID_RE = /^[a-f0-9]{12,16}$/i;
const KINDS = new Set(["boot", "ok", "error", "poll"]);
const EVENT_KEEP_MS = 30 * 24 * 60 * 60 * 1000;
const OK_MIN_GAP_MS = 50 * 60 * 1000;
const RESEND_MS = 5 * 60 * 1000;

export function openZbgwDiagDb(dbPath) {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.exec(`
    CREATE TABLE IF NOT EXISTS gateways (
      device_id TEXT PRIMARY KEY,
      fw TEXT,
      last_kind TEXT,
      last_ok INTEGER NOT NULL DEFAULT 1,
      last_message TEXT,
      last_code TEXT,
      last_seen INTEGER NOT NULL,
      last_ok_at INTEGER,
      last_error_at INTEGER,
      uptime_s INTEGER,
      reset_reason TEXT,
      heap INTEGER,
      wifi_rssi INTEGER,
      mqtt_ok INTEGER,
      zigbee_ok INTEGER,
      devices INTEGER,
      first_seen INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id TEXT NOT NULL,
      ts INTEGER NOT NULL,
      kind TEXT NOT NULL,
      ok INTEGER NOT NULL,
      code TEXT,
      message TEXT,
      fw TEXT,
      uptime_s INTEGER,
      reset_reason TEXT,
      heap INTEGER,
      wifi_rssi INTEGER,
      mqtt_ok INTEGER,
      zigbee_ok INTEGER,
      devices INTEGER
    );
    CREATE INDEX IF NOT EXISTS events_device_ts ON events(device_id, ts DESC);

    CREATE TABLE IF NOT EXISTS commands (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id TEXT NOT NULL,
      action TEXT NOT NULL,
      status TEXT NOT NULL DEFAULT 'pending',
      created_at INTEGER NOT NULL,
      sent_at INTEGER,
      done_at INTEGER
    );
    CREATE INDEX IF NOT EXISTS commands_device_status ON commands(device_id, status);
  `);
  return db;
}

function cleanId(raw) {
  const id = String(raw || "").trim().toLowerCase();
  return DEVICE_ID_RE.test(id) ? id : "";
}

function clip(value, max) {
  const s = String(value ?? "").trim();
  return s.length > max ? s.slice(0, max) : s;
}

function intOrNull(value) {
  if (value === null || value === undefined || value === "") return null;
  const n = Number(value);
  return Number.isFinite(n) ? Math.trunc(n) : null;
}

function boolInt(value, fallback = 0) {
  if (value === true || value === 1 || value === "1" || value === "true") return 1;
  if (value === false || value === 0 || value === "0" || value === "false") return 0;
  return fallback;
}

function pruneEvents(db) {
  const cutoff = Date.now() - EVENT_KEEP_MS;
  db.prepare("DELETE FROM events WHERE ts < ?").run(cutoff);
}

function claimCommands(db, deviceId) {
  const now = Date.now();
  const rows = db
    .prepare(
      `SELECT id, action FROM commands
       WHERE device_id = ? AND action = 'restart'
         AND (status = 'pending' OR (status = 'sent' AND sent_at IS NOT NULL AND sent_at < ?))
       ORDER BY id ASC LIMIT 1`,
    )
    .all(deviceId, now - RESEND_MS);

  const mark = db.prepare("UPDATE commands SET status = 'sent', sent_at = ? WHERE id = ?");
  for (const row of rows) {
    mark.run(now, row.id);
  }
  return rows.map((row) => ({ id: row.id, action: row.action }));
}

function completeRestarts(db, deviceId) {
  const now = Date.now();
  db.prepare(
    `UPDATE commands SET status = 'done', done_at = ?
     WHERE device_id = ? AND action = 'restart' AND status IN ('pending', 'sent')`,
  ).run(now, deviceId);
}

export function receiveZbgwDiag(db, body) {
  const deviceId = cleanId(body?.device_id);
  if (!deviceId) {
    const err = new Error("invalid_device_id");
    err.status = 400;
    throw err;
  }

  const kind = String(body?.kind || "poll").toLowerCase();
  if (!KINDS.has(kind)) {
    const err = new Error("invalid_kind");
    err.status = 400;
    throw err;
  }

  const now = Date.now();
  const ok = body?.ok !== false && kind !== "error";
  const fw = clip(body?.fw, 32);
  const code = clip(body?.code, 32);
  const message = clip(body?.message, 200);
  const uptimeS = intOrNull(body?.uptime_s);
  const resetReason = clip(body?.reset || body?.reset_reason, 24);
  const heap = intOrNull(body?.heap);
  const wifiRssi = intOrNull(body?.wifi_rssi);
  const mqttOk = boolInt(body?.mqtt_ok, ok ? 1 : 0);
  const zigbeeOk = boolInt(body?.zigbee_ok, 0);
  const devices = intOrNull(body?.devices);

  const existing = db.prepare("SELECT device_id, last_ok_at FROM gateways WHERE device_id = ?").get(deviceId);
  db.prepare(
    `INSERT INTO gateways (
       device_id, fw, last_kind, last_ok, last_message, last_code, last_seen,
       last_ok_at, last_error_at, uptime_s, reset_reason, heap, wifi_rssi,
       mqtt_ok, zigbee_ok, devices, first_seen
     ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT(device_id) DO UPDATE SET
       fw = excluded.fw,
       last_kind = excluded.last_kind,
       last_ok = excluded.last_ok,
       last_message = excluded.last_message,
       last_code = excluded.last_code,
       last_seen = excluded.last_seen,
       last_ok_at = CASE WHEN excluded.last_kind = 'ok' THEN excluded.last_seen ELSE last_ok_at END,
       last_error_at = CASE WHEN excluded.last_kind = 'error' THEN excluded.last_seen ELSE last_error_at END,
       uptime_s = excluded.uptime_s,
       reset_reason = excluded.reset_reason,
       heap = excluded.heap,
       wifi_rssi = excluded.wifi_rssi,
       mqtt_ok = excluded.mqtt_ok,
       zigbee_ok = excluded.zigbee_ok,
       devices = excluded.devices`,
  ).run(
    deviceId,
    fw,
    kind,
    ok ? 1 : 0,
    message || (kind === "ok" ? "Device working correctly" : ""),
    code,
    now,
    kind === "ok" ? now : null,
    kind === "error" ? now : null,
    uptimeS,
    resetReason,
    heap,
    wifiRssi,
    mqttOk,
    zigbeeOk,
    devices,
    now,
  );

  const storeEvent =
    kind === "error" ||
    kind === "boot" ||
    (kind === "ok" && (!existing?.last_ok_at || now - existing.last_ok_at >= OK_MIN_GAP_MS));

  if (storeEvent) {
    db.prepare(
      `INSERT INTO events (
         device_id, ts, kind, ok, code, message, fw, uptime_s, reset_reason,
         heap, wifi_rssi, mqtt_ok, zigbee_ok, devices
       ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    ).run(
      deviceId,
      now,
      kind,
      ok ? 1 : 0,
      code,
      message || (kind === "ok" ? "Device working correctly" : ""),
      fw,
      uptimeS,
      resetReason,
      heap,
      wifiRssi,
      mqttOk,
      zigbeeOk,
      devices,
    );
    pruneEvents(db);
  }

  if (kind === "boot") {
    completeRestarts(db, deviceId);
  }

  return {
    ok: true,
    commands: claimCommands(db, deviceId),
  };
}

export function listZbgwGateways(db) {
  const now = Date.now();
  return db
    .prepare(
      `SELECT g.*,
         (SELECT COUNT(*) FROM commands c
          WHERE c.device_id = g.device_id AND c.action = 'restart'
            AND c.status IN ('pending', 'sent')) AS pending_restart
       FROM gateways g
       ORDER BY g.last_seen DESC`,
    )
    .all()
    .map((row) => ({
      ...row,
      age_s: Math.max(0, Math.round((now - row.last_seen) / 1000)),
    }));
}

export function listZbgwEvents(db, deviceId, limit = 40) {
  const n = Math.min(Math.max(Number(limit) || 40, 1), 200);
  if (deviceId) {
    const id = cleanId(deviceId);
    if (!id) return [];
    return db.prepare("SELECT * FROM events WHERE device_id = ? ORDER BY ts DESC LIMIT ?").all(id, n);
  }
  return db.prepare("SELECT * FROM events ORDER BY ts DESC LIMIT ?").all(n);
}

export function enqueueZbgwRestart(db, deviceId) {
  const id = cleanId(deviceId);
  if (!id) {
    const err = new Error("unknown_gateway");
    err.status = 404;
    throw err;
  }
  const gateway = db.prepare("SELECT device_id FROM gateways WHERE device_id = ?").get(id);
  if (!gateway) {
    const err = new Error("unknown_gateway");
    err.status = 404;
    throw err;
  }
  const open = db
    .prepare(
      `SELECT id FROM commands
       WHERE device_id = ? AND action = 'restart' AND status IN ('pending', 'sent')
       ORDER BY id DESC LIMIT 1`,
    )
    .get(id);
  if (open) {
    return { id: open.id, queued: false };
  }
  const info = db
    .prepare("INSERT INTO commands (device_id, action, status, created_at) VALUES (?, 'restart', 'pending', ?)")
    .run(id, Date.now());
  return { id: Number(info.lastInsertRowid), queued: true };
}
