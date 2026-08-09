import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

const MAX_GAP_MS = 5 * 60 * 1000;

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
      energy_kwh_cumulative REAL NOT NULL
    );

    CREATE TABLE IF NOT EXISTS meta (
      key TEXT PRIMARY KEY,
      value TEXT NOT NULL
    );
  `);

  return db;
}

function toNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function getMeta(db, key) {
  const row = db.prepare("SELECT value FROM meta WHERE key = ?").get(key);
  return row ? row.value : null;
}

function setMeta(db, key, value) {
  db.prepare(
    `INSERT INTO meta (key, value) VALUES (?, ?)
     ON CONFLICT(key) DO UPDATE SET value = excluded.value`,
  ).run(key, String(value));
}

export function getEnergyTotal(db) {
  const raw = getMeta(db, "energy_kwh_total");
  const n = Number(raw);
  return Number.isFinite(n) ? n : 0;
}

export function insertSample(db, payload) {
  const ts = Date.parse(payload.ts || "") || Date.now();
  const outputPower = Math.max(0, toNumber(payload.outputPower) ?? 0);

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
    // Gaps larger than MAX_GAP_MS are ignored so offline periods do not invent energy.
  }

  setMeta(db, "energy_kwh_total", total);

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
  };

  db.prepare(
    `INSERT OR REPLACE INTO samples (
      ts, grid_voltage, pv_voltage, battery_voltage, battery_soc,
      battery_charge_current, load_percent, ac_frequency, pv_power,
      battery_discharge_current, output_power, energy_kwh_cumulative
    ) VALUES (
      @ts, @grid_voltage, @pv_voltage, @battery_voltage, @battery_soc,
      @battery_charge_current, @load_percent, @ac_frequency, @pv_power,
      @battery_discharge_current, @output_power, @energy_kwh_cumulative
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
    // Keep last cumulative energy in the bucket (monotonic), not the average.
    avg.energy_kwh_cumulative =
      chunk[chunk.length - 1].energy_kwh_cumulative;
    avg.ts = chunk[Math.floor(chunk.length / 2)].ts;
    out.push(avg);
  }
  return out;
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

  return {
    range,
    energyKwhTotal: getEnergyTotal(db),
    latest: latest ? { ...sampleToApi(latest), energyKwhTotal: getEnergyTotal(db) } : null,
    samples: reduced.map(sampleToApi),
  };
}

export function pruneOldSamples(db, retentionDays) {
  const cutoff = Date.now() - retentionDays * 86400000;
  db.prepare("DELETE FROM samples WHERE ts < ?").run(cutoff);
}
