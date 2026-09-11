/** Logical inverter fields ↔ HA entities. Bindings persist; stale times show what to heal. */

export const STALE_MS = 2 * 60 * 1000;

export const INVERTER_FIELDS = [
  {
    key: "gridVoltage",
    column: "grid_voltage",
    label: "Grid voltage",
    seeds: ["sensor.powmr_inverter_grid_voltage", "sensor.garden_powmr_inverter_grid_voltage"],
    units: ["v"],
    classes: ["voltage"],
    tokens: ["grid", "voltage"],
    prefer: ["powmr", "inverter"],
    reject: ["pv", "battery", "bus"],
  },
  {
    key: "pvVoltage",
    column: "pv_voltage",
    label: "PV voltage",
    seeds: ["sensor.powmr_inverter_pv_voltage", "sensor.garden_powmr_inverter_pv_voltage"],
    units: ["v"],
    classes: ["voltage"],
    tokens: ["pv", "voltage"],
    prefer: ["powmr", "inverter", "solar"],
    reject: ["grid", "battery"],
  },
  {
    key: "batteryVoltage",
    column: "battery_voltage",
    label: "Battery voltage",
    seeds: ["sensor.powmr_inverter_battery_voltage", "sensor.garden_powmr_inverter_battery_voltage"],
    units: ["v"],
    classes: ["voltage"],
    tokens: ["battery", "voltage"],
    prefer: ["powmr", "inverter", "pack"],
    reject: ["pv", "grid", "charge_voltage"],
  },
  {
    key: "batterySoc",
    column: "battery_soc",
    label: "Battery SoC",
    seeds: ["sensor.powmr_inverter_battery_soc", "sensor.garden_powmr_inverter_battery_soc"],
    units: ["%"],
    classes: ["battery"],
    tokens: ["soc", "battery"],
    prefer: ["powmr", "inverter"],
    reject: ["load_percent", "load"],
  },
  {
    key: "batteryChargeCurrent",
    column: "battery_charge_current",
    label: "Charge current",
    seeds: ["sensor.powmr_inverter_battery_charge_current", "sensor.garden_powmr_inverter_battery_charge_current"],
    units: ["a"],
    classes: ["current"],
    tokens: ["charge", "current"],
    prefer: ["powmr", "inverter", "battery"],
    reject: ["discharge"],
  },
  {
    key: "loadPercent",
    column: "load_percent",
    label: "Load percent",
    seeds: ["sensor.powmr_inverter_load_percent", "sensor.garden_powmr_inverter_load_percent"],
    units: ["%"],
    classes: [],
    tokens: ["load", "percent"],
    prefer: ["powmr", "inverter"],
    reject: ["soc", "battery"],
  },
  {
    key: "acFrequency",
    column: "ac_frequency",
    label: "AC frequency",
    seeds: ["sensor.garden_powmr_inverter_ac_frequency", "sensor.powmr_inverter_ac_frequency"],
    units: ["hz"],
    classes: ["frequency"],
    tokens: ["frequency"],
    prefer: ["powmr", "inverter", "ac"],
    reject: [],
  },
  {
    key: "pvPower",
    column: "pv_power",
    label: "PV power",
    seeds: ["sensor.garden_powmr_inverter_pv_power", "sensor.powmr_inverter_pv_power"],
    units: ["w", "kw"],
    classes: ["power"],
    tokens: ["pv", "power"],
    prefer: ["powmr", "inverter", "solar"],
    reject: ["unmetered", "grid_loads", "house_metered", "output", "daily"],
  },
  {
    key: "batteryDischargeCurrent",
    column: "battery_discharge_current",
    label: "Discharge current",
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
    column: "output_power",
    label: "Output power",
    seeds: ["sensor.garden_powmr_inverter_output_power", "sensor.powmr_inverter_output_power"],
    units: ["w", "kw"],
    classes: ["power"],
    tokens: ["output", "power"],
    prefer: ["powmr", "inverter"],
    reject: ["unmetered", "grid_loads", "house_metered", "pv", "daily", "washer"],
  },
];

export function ensureHaFieldTables(db) {
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
  `);
}

function normUnit(unit) {
  return String(unit || "").toLowerCase().replace(/\s+/g, "");
}

function parseHaTime(value) {
  if (value == null || value === "") return null;
  if (typeof value === "number" && Number.isFinite(value)) {
    return value < 1e12 ? value * 1000 : value;
  }
  const ms = Date.parse(value);
  return Number.isFinite(ms) ? ms : null;
}

export function parseNumericState(value) {
  if (value == null) return null;
  const s = String(value).trim().toLowerCase();
  if (!s || s === "unavailable" || s === "unknown" || s === "none" || s === "null") return null;
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

export function upsertHaCatalog(db, devices, seenAt = Date.now()) {
  if (!Array.isArray(devices) || !devices.length) return;
  const upsert = db.prepare(
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
  const tx = db.transaction((rows) => {
    for (const d of rows) {
      const entityId = String(d?.entity_id || d?.entityId || "").trim();
      if (!entityId) continue;
      upsert.run({
        entity_id: entityId,
        name: d.name || d.friendly_name || null,
        domain: entityId.split(".")[0] || null,
        device_class: d.device_class || d.deviceClass || null,
        unit: d.unit || d.unit_of_measurement || null,
        state: d.state == null ? null : String(d.state),
        last_seen: seenAt,
        ha_updated: parseHaTime(d.last_updated || d.lastUpdated || d.ha_updated),
      });
    }
  });
  tx(devices);
}

export function scoreFieldMatch(spec, entity, now = Date.now()) {
  const id = String(entity.entity_id || "").toLowerCase();
  if (!id.startsWith("sensor.")) return 0;
  const name = String(entity.name || "").toLowerCase();
  const hay = `${id} ${name}`;
  const unit = normUnit(entity.unit);
  const cls = String(entity.device_class || "").toLowerCase();
  let score = 0;

  if (spec.seeds?.some((s) => s.toLowerCase() === id)) score += 12;
  if (spec.units?.length && spec.units.includes(unit)) score += 3;
  if (spec.classes?.length && spec.classes.includes(cls)) score += 3;
  for (const token of spec.tokens || []) {
    if (hay.includes(token)) score += 2;
    else score -= 1;
  }
  for (const token of spec.prefer || []) {
    if (hay.includes(token)) score += 1;
  }
  for (const token of spec.reject || []) {
    if (hay.includes(token)) score -= 6;
  }
  if (parseNumericState(entity.state) == null) score -= 2;
  if (entity.last_seen && now - entity.last_seen > STALE_MS) score -= 4;
  return score;
}

function catalogRows(db) {
  return db.prepare("SELECT * FROM ha_catalog").all();
}

function isFresh(row, now) {
  return row && row.last_seen && now - row.last_seen <= STALE_MS;
}

function candidatesFor(spec, rows, now, limit = 8) {
  return rows
    .map((row) => ({ ...row, score: scoreFieldMatch(spec, row, now) }))
    .filter((row) => row.score >= 5)
    .sort((a, b) => b.score - a.score)
    .slice(0, limit);
}

function pickAuto(spec, rows, boundId, now) {
  const ranked = candidatesFor(spec, rows, now, 6);
  if (!ranked.length) return null;
  const top = ranked[0];
  if (boundId && top.entity_id === boundId) return top;
  const second = ranked[1];
  if (top.score >= 7 && (!second || top.score >= second.score + 3)) return top;
  return null;
}

function writeBinding(db, fieldKey, entityId, source, now) {
  db.prepare(
    `INSERT INTO field_bindings (field_key, entity_id, source, bound_at)
     VALUES (?, ?, ?, ?)
     ON CONFLICT(field_key) DO UPDATE SET
       entity_id = excluded.entity_id,
       source = CASE WHEN field_bindings.source = 'manual' AND excluded.source != 'manual'
         THEN field_bindings.source ELSE excluded.source END,
       bound_at = CASE WHEN field_bindings.entity_id = excluded.entity_id
         THEN field_bindings.bound_at ELSE excluded.bound_at END`,
  ).run(fieldKey, entityId, source, now);
}

function touchValue(db, fieldKey, value, now, ok) {
  db.prepare(
    `INSERT INTO field_bindings (field_key, last_value, last_value_at, last_ok_at, last_missing_at)
     VALUES (?, ?, ?, ?, ?)
     ON CONFLICT(field_key) DO UPDATE SET
       last_value = COALESCE(excluded.last_value, field_bindings.last_value),
       last_value_at = CASE WHEN excluded.last_value IS NOT NULL THEN excluded.last_value_at ELSE field_bindings.last_value_at END,
       last_ok_at = CASE WHEN excluded.last_ok_at IS NOT NULL THEN excluded.last_ok_at ELSE field_bindings.last_ok_at END,
       last_missing_at = CASE WHEN excluded.last_missing_at IS NOT NULL THEN excluded.last_missing_at ELSE field_bindings.last_missing_at END`,
  ).run(fieldKey, value, value != null ? now : null, ok ? now : null, ok ? null : now);
}

function mappingHint(payload, key) {
  const raw = payload?.mapping?.[key] || payload?.fields?.[key];
  if (!raw || typeof raw !== "object") return null;
  return {
    entityId: String(raw.entityId || raw.entity_id || "").trim() || null,
    value: parseNumericState(raw.value ?? raw.state),
    haUpdated: parseHaTime(raw.haUpdated || raw.ha_updated || raw.last_updated),
    status: raw.status || null,
  };
}

export function resolveInverterFields(db, payload = {}, now = Date.now()) {
  ensureHaFieldTables(db);
  const rows = catalogRows(db);
  const byId = new Map(rows.map((r) => [r.entity_id, r]));
  const bindings = new Map(
    db.prepare("SELECT * FROM field_bindings").all().map((r) => [r.field_key, r]),
  );

  const values = {};
  const report = [];

  for (const spec of INVERTER_FIELDS) {
    const binding = bindings.get(spec.key);
    const hint = mappingHint(payload, spec.key);
    const payloadValue = parseNumericState(payload[spec.key]);
    let entityId = binding?.source === "manual" ? binding.entity_id : hint?.entityId || binding?.entity_id;
    let source = binding?.source || (hint?.entityId ? "agent" : null);
    let healed = false;

    const boundRow = entityId ? byId.get(entityId) : null;
    const boundLive = isFresh(boundRow, now) && parseNumericState(boundRow.state) != null;

    if ((!entityId || !boundLive) && binding?.source !== "manual") {
      const seeded = (spec.seeds || [])
        .map((id) => byId.get(id))
        .find((row) => isFresh(row, now) && parseNumericState(row.state) != null);
      const auto = seeded || pickAuto(spec, rows, entityId, now);
      if (auto) {
        const nextId = auto.entity_id;
        if (nextId !== entityId) {
          healed = Boolean(entityId);
          entityId = nextId;
          source = seeded && spec.seeds.includes(nextId) ? "seed" : "auto";
          writeBinding(db, spec.key, entityId, source, now);
        }
      }
    } else if (entityId && (!binding?.entity_id || binding.entity_id !== entityId)) {
      writeBinding(db, spec.key, entityId, source || "agent", now);
    }

    const row = entityId ? byId.get(entityId) : null;
    let value = isFresh(row, now) ? parseNumericState(row.state) : null;
    if (value == null) value = hint?.value ?? null;
    if (value == null) value = payloadValue;
    if (spec.key === "outputPower" && value != null) value = Math.max(0, value);

    const haUpdated = row?.ha_updated || hint?.haUpdated || null;
    const freshAt = haUpdated || (value != null ? now : null);
    const stale = value != null && freshAt != null ? now - freshAt > STALE_MS : value == null;
    const ok = value != null;
    touchValue(db, spec.key, value, now, ok);

    values[spec.key] = value;
    report.push({
      key: spec.key,
      label: spec.label,
      entityId: entityId || null,
      source: source || "unbound",
      value,
      haUpdated,
      lastSeen: row?.last_seen || null,
      stale,
      status: ok ? (healed ? "healed" : stale ? "stale" : "ok") : "missing",
      name: row?.name || null,
      unit: row?.unit || null,
      candidates: ok ? [] : candidatesFor(spec, rows, now, 6).map((c) => ({
        entityId: c.entity_id,
        name: c.name,
        state: c.state,
        unit: c.unit,
        score: c.score,
      })),
    });
  }

  return { values, fields: report };
}

export function listHaFields(db) {
  ensureHaFieldTables(db);
  const now = Date.now();
  const { fields } = resolveInverterFields(db, {}, now);
  return {
    staleMs: STALE_MS,
    found: fields.filter((f) => f.status !== "missing").length,
    missing: fields.filter((f) => f.status === "missing").map((f) => f.key),
    healed: fields.filter((f) => f.status === "healed").map((f) => `${f.key}→${f.entityId}`),
    fields,
  };
}

export function setFieldBinding(db, fieldKey, entityId) {
  ensureHaFieldTables(db);
  const spec = INVERTER_FIELDS.find((f) => f.key === fieldKey);
  if (!spec) {
    const err = new Error("unknown_field");
    err.status = 404;
    throw err;
  }
  const id = String(entityId || "").trim();
  if (!id) {
    const err = new Error("entity_id required");
    err.status = 400;
    throw err;
  }
  writeBinding(db, fieldKey, id, "manual", Date.now());
  return listHaFields(db);
}
