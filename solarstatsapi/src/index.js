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

async function fetchEntity(entityId) {
  const url = `${HA_BASE_URL}/api/states/${entityId}`;
  let res;
  try {
    res = await fetch(url, {
      headers: {
        Authorization: `Bearer ${HA_TOKEN}`,
        "Content-Type": "application/json",
      },
    });
  } catch (err) {
    throw new Error(formatFetchError(`HA GET ${url}`, err));
  }

  if (!res.ok) {
    throw new Error(`HA ${entityId}: HTTP ${res.status}`);
  }

  const data = await res.json();
  return parseState(data.state);
}

async function collectSnapshot() {
  const entries = await Promise.all(
    Object.entries(ENTITIES).map(async ([key, entityId]) => {
      const value = await fetchEntity(entityId);
      return [key, value];
    }),
  );

  return {
    ts: new Date().toISOString(),
    ...Object.fromEntries(entries),
  };
}

async function forwardSnapshot(snapshot) {
  const headers = {
    "Content-Type": "application/json",
  };
  if (INGEST_SECRET) {
    headers.Authorization = `Bearer ${INGEST_SECRET}`;
  }

  let res;
  try {
    res = await fetch(SITE_INGEST_URL, {
      method: "POST",
      headers,
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

async function tick() {
  try {
    const snapshot = await collectSnapshot();
    if (DRY_RUN) {
      console.log(`[${snapshot.ts}] dry-run snapshot:`, JSON.stringify(snapshot));
      return;
    }
    await forwardSnapshot(snapshot);
    console.log(
      `[${snapshot.ts}] forwarded SoC=${snapshot.batterySoc}% PV=${snapshot.pvPower}W Out=${snapshot.outputPower}W`,
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
