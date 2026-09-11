import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

const STALE_MS = 2 * 60 * 1000;

const CORE_FIELDS = [
  {
    key: "gridVoltage",
    seeds: ["sensor.powmr_inverter_grid_voltage", "sensor.garden_powmr_inverter_grid_voltage"],
    units: ["v"],
    classes: ["voltage"],
    tokens: ["grid", "voltage"],
    prefer: ["powmr", "inverter"],
    reject: ["pv", "battery", "bus"],
  },
  {
    key: "pvVoltage",
    seeds: ["sensor.powmr_inverter_pv_voltage", "sensor.garden_powmr_inverter_pv_voltage"],
    units: ["v"],
    classes: ["voltage"],
    tokens: ["pv", "voltage"],
    prefer: ["powmr", "inverter", "solar"],
    reject: ["grid", "battery"],
  },
  {
    key: "batteryVoltage",
    seeds: ["sensor.powmr_inverter_battery_voltage", "sensor.garden_powmr_inverter_battery_voltage"],
    units: ["v"],
    classes: ["voltage"],
    tokens: ["battery", "voltage"],
    prefer: ["powmr", "inverter", "pack"],
    reject: ["pv", "grid"],
  },
  {
    key: "batterySoc",
    seeds: ["sensor.powmr_inverter_battery_soc", "sensor.garden_powmr_inverter_battery_soc"],
    units: ["%"],
    classes: ["battery"],
    tokens: ["soc", "battery"],
    prefer: ["powmr", "inverter"],
    reject: ["load"],
  },
  {
    key: "batteryChargeCurrent",
    seeds: ["sensor.powmr_inverter_battery_charge_current", "sensor.garden_powmr_inverter_battery_charge_current"],
    units: ["a"],
    classes: ["current"],
    tokens: ["charge", "current"],
    prefer: ["powmr", "inverter", "battery"],
    reject: ["discharge"],
  },
  {
    key: "loadPercent",
    seeds: ["sensor.powmr_inverter_load_percent", "sensor.garden_powmr_inverter_load_percent"],
    units: ["%"],
    classes: [],
    tokens: ["load", "percent"],
    prefer: ["powmr", "inverter"],
    reject: ["soc", "battery"],
  },
  {
    key: "acFrequency",
    seeds: ["sensor.garden_powmr_inverter_ac_frequency", "sensor.powmr_inverter_ac_frequency"],
    units: ["hz"],
    classes: ["frequency"],
    tokens: ["frequency"],
    prefer: ["powmr", "inverter", "ac"],
    reject: [],
  },
  {
    key: "pvPower",
    seeds: ["sensor.garden_powmr_inverter_pv_power", "sensor.powmr_inverter_pv_power"],
    units: ["w", "kw"],
    classes: ["power"],
    tokens: ["pv", "power"],
    prefer: ["powmr", "inverter", "solar"],
    reject: ["unmetered", "grid_loads", "house_metered", "output", "daily"],
  },
  {
    key: "batteryDischargeCurrent",
    seeds: [
      "sensor.garden_powmr_inverter_battery_discharge_current",
      "sensor.powmr_inverter_battery_discharge_current",
    ],
    units: ["a"],
    classes: ["current"],
    tokens: ["discharge", "current"],
    prefer: ["powmr", "inverter", "battery"],
    reject: ["charge"],
  },
  {
    key: "outputPower",
    seeds: ["sensor.garden_powmr_inverter_output_power", "sensor.powmr_inverter_output_power"],
    units: ["w", "kw"],
    classes: ["power"],
    tokens: ["output", "power"],
    prefer: ["powmr", "inverter"],
    reject: ["unmetered", "grid_loads", "house_metered", "pv", "daily", "washer"],
  },
];

const LOAD_FIELDS = [
  { key: "officePc", seeds: ["sensor.office_pc_synth_energy_daily"], tokens: ["office", "pc"], prefer: ["energy", "daily"] },
  { key: "frontRoomPc", seeds: ["sensor.front_room_pc_synth_energy_daily"], tokens: ["front", "pc"], prefer: ["energy", "daily"] },
  { key: "pi5", seeds: ["sensor.pi5_server_energy_daily_2", "sensor.pi5_server_energy_daily"], tokens: ["pi5", "server"], prefer: ["energy", "daily"] },
  { key: "motorbike", seeds: ["sensor.motorbike_charger_energy_daily_2", "sensor.motorbike_charger_energy_daily"], tokens: ["motorbike"], prefer: ["energy", "daily"] },
  { key: "fridge", seeds: ["sensor.fridge_energy_daily_2", "sensor.fridge_energy_daily"], tokens: ["fridge"], prefer: ["energy", "daily"] },
  { key: "washingMachine", seeds: ["sensor.inverter_loads", "sensor.inverter_loads_energy_daily"], tokens: ["washer", "washing", "inverter_loads"], prefer: ["energy", "daily"] },
  { key: "otherInverter", seeds: ["sensor.inverter_unmetered", "sensor.inverter_unmetered_energy_daily"], tokens: ["unmetered"], prefer: ["energy", "daily"] },
];

function dbPath(filePath) {
  const resolved = path.resolve(filePath);
  return resolved.toLowerCase().endsWith(".json")
    ? resolved.replace(/\.json$/i, ".db")
    : resolved;
}

function ensureTables(db) {
  db.exec(`
    CREATE TABLE IF NOT EXISTS ha_catalog (
      entity_id TEXT PRIMARY KEY,
      name TEXT,
      domain TEXT,
      device_class TEXT,
      unit TEXT,
      state TEXT,
      last_seen INTEGER NOT NULL,
      ha_updated INTEGER
    );
    CREATE TABLE IF NOT EXISTS field_bindings (
      field_key TEXT PRIMARY KEY,
      entity_id TEXT,
      source TEXT,
      bound_at INTEGER,
      last_value REAL,
      last_value_at INTEGER,
      last_ok_at INTEGER,
      last_missing_at INTEGER
    );
    CREATE INDEX IF NOT EXISTS idx_ha_catalog_seen ON ha_catalog(last_seen);
  `);
}

function migrateJson(db, jsonPath) {
  if (!jsonPath || !fs.existsSync(jsonPath)) return;
  let data;
  try {
    data = JSON.parse(fs.readFileSync(jsonPath, "utf8"));
  } catch {
    return;
  }
  const upsertEntity = db.prepare(
    `INSERT INTO ha_catalog (entity_id, name, domain, device_class, unit, state, last_seen, ha_updated)
     VALUES (@entity_id, @name, @domain, @device_class, @unit, @state, @last_seen, @ha_updated)
     ON CONFLICT(entity_id) DO NOTHING`,
  );
  const upsertBinding = db.prepare(
    `INSERT INTO field_bindings (field_key, entity_id, source, bound_at, last_value, last_value_at)
     VALUES (@field_key, @entity_id, @source, @bound_at, @last_value, @last_value_at)
     ON CONFLICT(field_key) DO NOTHING`,
  );
  const tx = db.transaction(() => {
    for (const e of Object.values(data.entities || {})) {
      const id = e.entityId || e.entity_id;
      if (!id) continue;
      upsertEntity.run({
        entity_id: id,
        name: e.name || null,
        domain: e.domain || String(id).split(".")[0] || null,
        device_class: e.deviceClass || e.device_class || null,
        unit: e.unit || null,
        state: e.state == null ? null : String(e.state),
        last_seen: e.lastSeen || e.last_seen || Date.now(),
        ha_updated: e.haUpdated || e.ha_updated || null,
      });
    }
    for (const [key, b] of Object.entries(data.bindings || {})) {
      const last = data.values?.[key];
      upsertBinding.run({
        field_key: key,
        entity_id: b.entityId || b.entity_id || last?.entityId || null,
        source: b.source || "json",
        bound_at: b.boundAt || b.bound_at || Date.now(),
        last_value: last?.value ?? null,
        last_value_at: last?.seenAt || last?.haUpdated || null,
      });
    }
  });
  tx();
}

export function openCatalog(filePath) {
  const resolved = dbPath(filePath);
  fs.mkdirSync(path.dirname(resolved), { recursive: true });
  const db = new Database(resolved);
  db.pragma("journal_mode = WAL");
  ensureTables(db);
  const jsonSibling = resolved.replace(/\.db$/i, ".json");
  if (jsonSibling !== resolved) migrateJson(db, jsonSibling);
  if (filePath && String(filePath).toLowerCase().endsWith(".json")) {
    migrateJson(db, path.resolve(filePath));
  }

  const upsertEntity = db.prepare(
    `INSERT INTO ha_catalog (entity_id, name, domain, device_class, unit, state, last_seen, ha_updated)
     VALUES (@entity_id, @name, @domain, @device_class, @unit, @state, @last_seen, @ha_updated)
     ON CONFLICT(entity_id) DO UPDATE SET
       name = COALESCE(excluded.name, ha_catalog.name),
       domain = excluded.domain,
       device_class = COALESCE(excluded.device_class, ha_catalog.device_class),
       unit = COALESCE(excluded.unit, ha_catalog.unit),
       state = excluded.state,
       last_seen = excluded.last_seen,
       ha_updated = COALESCE(excluded.ha_updated, ha_catalog.ha_updated)`,
  );
  const selectEntity = db.prepare("SELECT * FROM ha_catalog WHERE entity_id = ?");
  const selectAll = db.prepare("SELECT * FROM ha_catalog");
  const selectBinding = db.prepare("SELECT * FROM field_bindings WHERE field_key = ?");
  const writeBinding = db.prepare(
    `INSERT INTO field_bindings (field_key, entity_id, source, bound_at)
     VALUES (?, ?, ?, ?)
     ON CONFLICT(field_key) DO UPDATE SET
       entity_id = excluded.entity_id,
       source = CASE WHEN field_bindings.source = 'manual' AND excluded.source != 'manual'
         THEN field_bindings.source ELSE excluded.source END,
       bound_at = CASE WHEN field_bindings.entity_id = excluded.entity_id
         THEN field_bindings.bound_at ELSE excluded.bound_at END`,
  );
  const touchValue = db.prepare(
    `INSERT INTO field_bindings (field_key, last_value, last_value_at, last_ok_at, last_missing_at)
     VALUES (?, ?, ?, ?, ?)
     ON CONFLICT(field_key) DO UPDATE SET
       last_value = COALESCE(excluded.last_value, field_bindings.last_value),
       last_value_at = CASE WHEN excluded.last_value IS NOT NULL THEN excluded.last_value_at ELSE field_bindings.last_value_at END,
       last_ok_at = CASE WHEN excluded.last_ok_at IS NOT NULL THEN excluded.last_ok_at ELSE field_bindings.last_ok_at END,
       last_missing_at = CASE WHEN excluded.last_missing_at IS NOT NULL THEN excluded.last_missing_at ELSE field_bindings.last_missing_at END`,
  );

  const ingestTx = db.transaction((states, now) => {
    const seen = [];
    for (const s of states) {
      const id = String(s.entity_id || s.entityId || "").trim();
      if (!id) continue;
      seen.push(id);
      const haUpdated = Date.parse(s.last_updated || s.lastUpdated || s.last_changed || "") || now;
      upsertEntity.run({
        entity_id: id,
        name: s.attributes?.friendly_name || s.name || null,
        domain: id.split(".")[0] || null,
        device_class: s.attributes?.device_class || s.device_class || s.deviceClass || null,
        unit: s.attributes?.unit_of_measurement || s.unit || null,
        state: s.state == null ? null : String(s.state),
        last_seen: now,
        ha_updated: Number.isFinite(haUpdated) ? haUpdated : now,
      });
    }
    return seen;
  });

  function ingest(states, now = Date.now()) {
    return ingestTx(Array.isArray(states) ? states : [], now);
  }

  function toEntity(row) {
    if (!row) return null;
    return {
      entityId: row.entity_id,
      name: row.name,
      domain: row.domain,
      deviceClass: row.device_class,
      unit: row.unit,
      state: row.state,
      lastSeen: row.last_seen,
      haUpdated: row.ha_updated,
    };
  }

  function resolveAll(now = Date.now()) {
    const list = selectAll.all().map(toEntity);
    const core = {};
    const mapping = {};
    const loads = {};
    const tx = db.transaction(() => {
      for (const spec of CORE_FIELDS) {
        const item = resolveOne(spec, list, now);
        core[spec.key] = item.value;
        mapping[spec.key] = item;
      }
      for (const spec of LOAD_FIELDS) {
        const item = resolveOne(spec, list, now, { energy: true });
        loads[spec.key] = energyKwh(item);
        mapping[spec.key] = item;
      }
    });
    tx();
    return { core, loads, mapping };
  }

  function resolveOne(spec, list, now, opts = {}) {
    const bound = selectBinding.get(spec.key);
    let entityId = bound?.entity_id;
    let source = bound?.source || "unbound";
    let healed = false;

    const live = entityId ? toEntity(selectEntity.get(entityId)) : null;
    const liveOk =
      live &&
      parseNum(live.state) != null &&
      live.lastSeen &&
      now - live.lastSeen <= STALE_MS;
    if (!liveOk) {
      const seeded = (spec.seeds || [])
        .map((id) => toEntity(selectEntity.get(id)))
        .find((e) => e && parseNum(e.state) != null);
      const auto = seeded || pickAuto(spec, list, opts);
      if (auto && bound?.source !== "manual") {
        entityId = auto.entityId;
        source = seeded && spec.seeds.includes(auto.entityId) ? "seed" : "auto";
        healed = Boolean(bound?.entity_id) && bound.entity_id !== auto.entityId;
        writeBinding.run(spec.key, entityId, source, now);
      }
    } else if (!bound) {
      writeBinding.run(spec.key, entityId, "seed", now);
      source = "seed";
    }

    const row = entityId ? toEntity(selectEntity.get(entityId)) : null;
    const value =
      row && row.lastSeen && now - row.lastSeen <= STALE_MS ? parseNum(row.state) : null;
    const haUpdated = row?.haUpdated || null;
    const stale = value != null && haUpdated != null ? now - haUpdated > STALE_MS : value == null;
    touchValue.run(spec.key, value, value != null ? now : null, value != null ? now : null, value != null ? null : now);
    return {
      entityId: entityId || null,
      value,
      haUpdated,
      stale,
      source,
      status: value != null ? (healed ? "healed" : stale ? "stale" : "ok") : "missing",
      name: row?.name || null,
      unit: row?.unit || null,
    };
  }

  function listBoard(domains) {
    const allow = domains instanceof Set ? domains : new Set(domains || []);
    return selectAll.all()
      .filter((row) => !allow.size || allow.has(row.domain))
      .map((row) => ({
        entity_id: row.entity_id,
        state: row.state,
        name: row.name,
        device_class: row.device_class,
        unit: row.unit,
        last_updated: row.ha_updated ? new Date(row.ha_updated).toISOString() : null,
      }));
  }

  function close() {
    db.close();
  }

  return { ingest, resolveAll, listBoard, close, path: resolved, db };
}

function parseNum(value) {
  if (value == null) return null;
  const s = String(value).trim().toLowerCase();
  if (!s || s === "unavailable" || s === "unknown" || s === "none" || s === "null") return null;
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

function energyKwh(item) {
  const n = item.value;
  if (n == null) return null;
  const unit = String(item.unit || "").toLowerCase().replace(/\s+/g, "");
  if (unit === "wh") return n / 1000;
  if (unit === "mwh") return n * 1000;
  return n;
}

function normUnit(unit) {
  return String(unit || "").toLowerCase().replace(/\s+/g, "");
}

function score(spec, entity, opts = {}) {
  const id = String(entity.entityId || "").toLowerCase();
  if (!id.startsWith("sensor.")) return 0;
  const name = String(entity.name || "").toLowerCase();
  const hay = `${id} ${name}`;
  if (opts.energy && !/(energy|kwh|daily)/i.test(hay) && normUnit(entity.unit) !== "kwh") return 0;
  let n = 0;
  if (spec.seeds?.some((s) => s.toLowerCase() === id)) n += 12;
  if (spec.units?.length && spec.units.includes(normUnit(entity.unit))) n += 3;
  if (spec.classes?.length && spec.classes.includes(String(entity.deviceClass || "").toLowerCase())) n += 3;
  for (const token of spec.tokens || []) {
    if (hay.includes(token)) n += 2;
    else n -= 1;
  }
  for (const token of spec.prefer || []) {
    if (hay.includes(token)) n += 1;
  }
  for (const token of spec.reject || []) {
    if (hay.includes(token)) n -= 6;
  }
  if (parseNum(entity.state) == null) n -= 2;
  return n;
}

function pickAuto(spec, list, opts) {
  const ranked = list
    .map((e) => ({ e, score: score(spec, e, opts) }))
    .filter((x) => x.score >= 5)
    .sort((a, b) => b.score - a.score);
  if (!ranked.length) return null;
  const top = ranked[0];
  const second = ranked[1];
  if (top.score >= 7 && (!second || top.score >= second.score + 3)) return top.e;
  return null;
}

export function summarizeMapping(mapping) {
  const items = Object.entries(mapping).map(([key, v]) => ({ key, ...v }));
  const missing = items.filter((i) => i.status === "missing").map((i) => i.key);
  const healed = items.filter((i) => i.status === "healed").map((i) => `${i.key}→${i.entityId}`);
  const found = items.filter((i) => i.status !== "missing").length;
  return { found, missing, healed };
}
