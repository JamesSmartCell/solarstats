import "dotenv/config";

function env(name, fallback = "") {
  const raw = process.env[name] ?? fallback;
  return String(raw).trim().replace(/^["']|["']$/g, "");
}

const HA_BASE_URL = env("HA_BASE_URL", "http://192.168.50.41").replace(/\/$/, "");
const HA_TOKEN = env("HA_TOKEN");
const SITE_INGEST_URL = env("SITE_INGEST_URL");
const POLL_INTERVAL_MS = Number(env("POLL_INTERVAL_MS", "15000"));
const INGEST_SECRET = env("INGEST_SECRET");
const DRY_RUN = ["1", "true", "yes"].includes(env("DRY_RUN", "false").toLowerCase());

const ENTITIES = {
  gridVoltage: "sensor.powmr_inverter_grid_voltage",
  pvVoltage: "sensor.powmr_inverter_pv_voltage",
  batteryVoltage: "sensor.powmr_inverter_battery_voltage",
  batterySoc: "sensor.powmr_inverter_battery_soc",
  batteryChargeCurrent: "sensor.powmr_inverter_battery_charge_current",
  loadPercent: "sensor.powmr_inverter_load_percent",
  acFrequency: "sensor.garden_powmr_inverter_ac_frequency",
  pvPower: "sensor.garden_powmr_inverter_pv_power",
  batteryDischargeCurrent: "sensor.garden_powmr_inverter_battery_discharge_current",
  outputPower: "sensor.garden_powmr_inverter_output_power",
};

/** Daily kWh meters for All-devices pie (keep HA _2 IDs where renamed). */
const LOAD_DAILY_ENTITIES = {
  officePc: "sensor.office_pc_synth_energy_daily",
  frontRoomPc: "sensor.front_room_pc_synth_energy_daily",
  pi5: "sensor.pi5_server_energy_daily_2",
  motorbike: "sensor.motorbike_charger_energy_daily_2",
  fridge: "sensor.fridge_energy_daily_2",
  washingMachine: "sensor.inverter_loads_energy_daily",
  otherInverter: "sensor.inverter_unmetered_energy_daily",
};

/** Switches/lights/sensors mirrored on solarstats (fallback if /api/states fails). */
const DEVICE_ENTITIES = [
  "switch.smart_socket_socket_1",
  "switch.zigbeesensor_switch_2",
  "switch.smart_socket_2_socket_1",
  "light.office_office",
  "light.front_bedroom",
  "light.living_room_floor_lamp_2",
  "binary_sensor.ds01_contact",
];

const CLICKABLE_DOMAINS = new Set(["switch", "light"]);
const BOARD_DOMAINS = new Set(["switch", "light", "sensor", "binary_sensor"]);

function mapHaEntity(s, entityId = s.entity_id) {
  const attrs = s.attributes || {};
  return {
    entity_id: entityId,
    state: s.state,
    name: attrs.friendly_name || null,
    device_class: attrs.device_class || null,
    unit: attrs.unit_of_measurement || null,
  };
}

function requireEnv(name, value) {
  if (!value) {
    console.error(`Missing required env var: ${name}`);
    process.exit(1);
  }
}

requireEnv("HA_TOKEN", HA_TOKEN);
if (!DRY_RUN) {
  requireEnv("SITE_INGEST_URL", SITE_INGEST_URL);
}

function parseState(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function formatFetchError(stage, err) {
  const cause = err.cause;
  const detail = cause
    ? `${cause.code || ""} ${cause.message || cause}`.trim()
    : err.message;
  return `${stage}: ${detail}`;
}

function agentHeaders() {
  const headers = { "Content-Type": "application/json" };
  if (INGEST_SECRET) {
    headers.Authorization = `Bearer ${INGEST_SECRET}`;
  }
  return headers;
}

function commandsUrl() {
  if (!SITE_INGEST_URL) return "";
  return SITE_INGEST_URL.replace(/\/api\/ingest\/?$/, "/api/agent/commands");
}

function haAuthHeaders() {
  return {
    Authorization: `Bearer ${HA_TOKEN}`,
    "Content-Type": "application/json",
  };
}

async function fetchEntity(entityId) {
  const url = `${HA_BASE_URL}/api/states/${entityId}`;
  let res;
  try {
    res = await fetch(url, { headers: haAuthHeaders() });
  } catch (err) {
    throw new Error(formatFetchError(`HA GET ${url}`, err));
  }

  if (!res.ok) {
    throw new Error(`HA ${entityId}: HTTP ${res.status}`);
  }

  const data = await res.json();
  return parseState(data.state);
}

async function fetchEntityFull(entityId) {
  const url = `${HA_BASE_URL}/api/states/${entityId}`;
  let res;
  try {
    res = await fetch(url, { headers: haAuthHeaders() });
  } catch (err) {
    throw new Error(formatFetchError(`HA GET ${url}`, err));
  }
  if (!res.ok) {
    throw new Error(`HA ${entityId}: HTTP ${res.status}`);
  }
  return res.json();
}

async function collectMap(map) {
  const entries = await Promise.all(
    Object.entries(map).map(async ([key, entityId]) => {
      try {
        const value = await fetchEntity(entityId);
        return [key, value];
      } catch (err) {
        console.warn(`[warn] ${err.message}`);
        return [key, null];
      }
    }),
  );
  return Object.fromEntries(entries);
}

/** All HA switches, lights, sensors, and binary sensors (for admin ACL + board). */
async function fetchAllBoardDevices() {
  const url = `${HA_BASE_URL}/api/states`;
  let res;
  try {
    res = await fetch(url, { headers: haAuthHeaders() });
  } catch (err) {
    throw new Error(formatFetchError(`HA GET ${url}`, err));
  }
  if (!res.ok) {
    throw new Error(`HA states: HTTP ${res.status}`);
  }
  const states = await res.json();
  if (!Array.isArray(states)) return [];
  return states
    .filter((s) => BOARD_DOMAINS.has(String(s.entity_id || "").split(".")[0]))
    .map((s) => mapHaEntity(s));
}

async function collectDevicesFallback(entityIds) {
  const list = entityIds?.length ? entityIds : DEVICE_ENTITIES;
  return Promise.all(
    list.map(async (entityId) => {
      try {
        const data = await fetchEntityFull(entityId);
        return mapHaEntity(data, entityId);
      } catch (err) {
        console.warn(`[warn] ${err.message}`);
        return {
          entity_id: entityId,
          state: "unavailable",
          name: null,
          device_class: null,
          unit: null,
        };
      }
    }),
  );
}

async function collectDevices() {
  try {
    return await fetchAllBoardDevices();
  } catch (err) {
    console.warn(`[warn] full device list failed, fallback: ${err.message}`);
    return collectDevicesFallback(DEVICE_ENTITIES);
  }
}

async function collectSnapshot() {
  const [core, loadsDailyKwh, devices] = await Promise.all([
    collectMap(ENTITIES),
    collectMap(LOAD_DAILY_ENTITIES),
    collectDevices(),
  ]);

  return {
    ts: new Date().toISOString(),
    ...core,
    loadsDailyKwh,
    devices,
  };
}

async function forwardSnapshot(snapshot) {
  let res;
  try {
    res = await fetch(SITE_INGEST_URL, {
      method: "POST",
      headers: agentHeaders(),
      body: JSON.stringify(snapshot),
    });
  } catch (err) {
    throw new Error(
      formatFetchError(
        `Ingest POST ${SITE_INGEST_URL} (is solarstats running / tunnel up?)`,
        err,
      ),
    );
  }

  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`Ingest HTTP ${res.status}${body ? `: ${body}` : ""}`);
  }
}

async function callHaService(domain, service, entityId) {
  const url = `${HA_BASE_URL}/api/services/${domain}/${service}`;
  const res = await fetch(url, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${HA_TOKEN}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ entity_id: entityId }),
  });
  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`HA service ${domain}.${service} ${entityId}: HTTP ${res.status} ${body}`);
  }
}

async function completeCommand(id, ok) {
  const base = commandsUrl();
  if (!base) return;
  await fetch(`${base}/${id}/complete`, {
    method: "POST",
    headers: agentHeaders(),
    body: JSON.stringify({ ok }),
  }).catch((err) => console.warn(`[warn] command complete failed: ${err.message}`));
}

async function processCommands() {
  const url = commandsUrl();
  if (!url || DRY_RUN) return;

  let res;
  try {
    res = await fetch(url, { headers: agentHeaders() });
  } catch (err) {
    console.warn(`[warn] commands poll: ${err.message}`);
    return;
  }
  if (!res.ok) {
    console.warn(`[warn] commands HTTP ${res.status}`);
    return;
  }

  const data = await res.json();
  const commands = data.commands || [];
  for (const cmd of commands) {
    try {
      const domain = String(cmd.entityId || "").split(".")[0];
      if (!domain) throw new Error("bad entity");
      if (!CLICKABLE_DOMAINS.has(domain)) throw new Error("not toggleable");
      const service = cmd.action === "toggle" ? "toggle" : cmd.action;
      await callHaService(domain, service, cmd.entityId);
      await completeCommand(cmd.id, true);
      console.log(`[cmd] ${service} ${cmd.entityId}`);
    } catch (err) {
      console.error(`[cmd] failed ${cmd.entityId}:`, err.message);
      await completeCommand(cmd.id, false);
    }
  }
}

async function tick() {
  try {
    await processCommands();
    const snapshot = await collectSnapshot();
    if (DRY_RUN) {
      console.log(`[${snapshot.ts}] dry-run snapshot:`, JSON.stringify(snapshot));
      return;
    }
    await forwardSnapshot(snapshot);
    console.log(
      `[${snapshot.ts}] forwarded SoC=${snapshot.batterySoc}% PV=${snapshot.pvPower}W Out=${snapshot.outputPower}W devices=${snapshot.devices?.length ?? 0}`,
    );
  } catch (err) {
    console.error(`[${new Date().toISOString()}] poll failed:`, err.message);
  }
}

console.log(
  DRY_RUN
    ? `solarstatsapi DRY_RUN → HA ${HA_BASE_URL}, every ${POLL_INTERVAL_MS}ms (no ingest)`
    : `solarstatsapi starting → HA ${HA_BASE_URL}, ingest ${SITE_INGEST_URL}, every ${POLL_INTERVAL_MS}ms`,
);

await tick();
setInterval(tick, POLL_INTERVAL_MS);
