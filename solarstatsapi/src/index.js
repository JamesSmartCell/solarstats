import "dotenv/config";
import { openCatalog } from "./catalog.js";

function env(name, fallback = "") {
  const raw = process.env[name] ?? fallback;
  return String(raw).trim().replace(/^["']|["']$/g, "");
}

const HA_BASE_URL = env("HA_BASE_URL", "http://192.168.50.41").replace(/\/$/, "");
const HA_TOKEN = env("HA_TOKEN");
const SITE_INGEST_URL = env("SITE_INGEST_URL");
const SITE_SLUG = env("SITE_SLUG");
const POLL_INTERVAL_MS = Number(env("POLL_INTERVAL_MS", "15000"));
const INGEST_SECRET = env("INGEST_SECRET");
const DRY_RUN = ["1", "true", "yes"].includes(env("DRY_RUN", "false").toLowerCase());
const CATALOG_PATH = env("CATALOG_PATH", "./data/catalog.db");
const catalog = openCatalog(CATALOG_PATH);

const CLICKABLE_DOMAINS = new Set(["switch", "light"]);
const BOARD_DOMAINS = new Set(["switch", "light", "sensor", "binary_sensor"]);

function mapHaEntity(s, entityId = s.entity_id) {
  const attrs = s.attributes || {};
  return {
    entity_id: entityId,
    state: s.state,
    name: attrs.friendly_name || s.name || null,
    device_class: attrs.device_class || s.device_class || null,
    unit: attrs.unit_of_measurement || s.unit || null,
    last_updated: s.last_updated || s.last_changed || null,
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

function ingestUrl() {
  if (!SITE_INGEST_URL) return "";
  if (/\/api\/ingest\/[^/]+\/?$/.test(SITE_INGEST_URL)) return SITE_INGEST_URL;
  if (!SITE_SLUG) return SITE_INGEST_URL;
  return SITE_INGEST_URL.replace(/\/api\/ingest\/?$/, `/api/ingest/${SITE_SLUG}`);
}

function commandsUrl() {
  const url = ingestUrl();
  if (!url) return "";
  const withSlug = url.match(/^(.*)\/api\/ingest\/([^/]+)\/?$/);
  if (withSlug) return `${withSlug[1]}/api/agent/commands/${withSlug[2]}`;
  return url.replace(/\/api\/ingest\/?$/, "/api/agent/commands");
}

function haAuthHeaders() {
  return {
    Authorization: `Bearer ${HA_TOKEN}`,
    "Content-Type": "application/json",
  };
}

async function fetchAllStates() {
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
  return Array.isArray(states) ? states : [];
}

function toBoardDevices(states) {
  return (states || [])
    .filter((s) => BOARD_DOMAINS.has(String(s.entity_id || "").split(".")[0]))
    .map((s) => mapHaEntity(s));
}

async function collectSnapshot() {
  try {
    const states = await fetchAllStates();
    catalog.ingest(states);
    return {
      ts: new Date().toISOString(),
      devices: toBoardDevices(states),
    };
  } catch (err) {
    const cached = catalog.listBoard(BOARD_DOMAINS);
    if (!cached.length) throw err;
    console.warn(`[warn] HA states failed, sending last catalog: ${err.message}`);
    return {
      ts: new Date().toISOString(),
      devices: cached,
    };
  }
}

async function forwardSnapshot(snapshot) {
  let res;
  try {
    res = await fetch(ingestUrl(), {
      method: "POST",
      headers: agentHeaders(),
      body: JSON.stringify(snapshot),
    });
  } catch (err) {
    throw new Error(
      formatFetchError(
        `Ingest POST ${ingestUrl()} (is solarstats running / tunnel up?)`,
        err,
      ),
    );
  }

  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`Ingest HTTP ${res.status}${body ? `: ${body}` : ""}`);
  }
  return res.json().catch(() => ({}));
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
      console.log(`[${snapshot.ts}] dry-run devices=${snapshot.devices?.length ?? 0}`);
      return;
    }
    await forwardSnapshot(snapshot);
    console.log(`[${snapshot.ts}] forwarded devices=${snapshot.devices?.length ?? 0}`);
  } catch (err) {
    console.error(`[${new Date().toISOString()}] poll failed:`, err.message);
  }
}

console.log(
  DRY_RUN
    ? `solarstatsapi DRY_RUN → HA ${HA_BASE_URL}, every ${POLL_INTERVAL_MS}ms (no ingest)`
    : `solarstatsapi starting → HA ${HA_BASE_URL}, ingest ${SITE_INGEST_URL}, catalog ${catalog.path}, every ${POLL_INTERVAL_MS}ms`,
);

await tick();
setInterval(tick, POLL_INTERVAL_MS);
