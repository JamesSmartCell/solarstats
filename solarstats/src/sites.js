import path from "node:path";
import { getMeta, openDatabase, setMeta } from "./db.js";

const RESERVED = new Set([
  "login",
  "logout",
  "pending",
  "admin",
  "auth",
  "api",
  "ws",
  "create-passkey",
  "setup-passkey",
  "solarstats",
]);

function envName(slug, suffix) {
  return `SITE_${String(slug).replace(/-/g, "_").toUpperCase()}_${suffix}`;
}

export function isReservedSlug(slug) {
  return RESERVED.has(String(slug || "").toLowerCase());
}

/**
 * Home uses the existing DB + INGEST_SECRET.
 * Extra slugs in SITES=rivermill,other each need SITE_<SLUG>_SECRET
 * and get data/sites/<slug>.db
 */
export function loadSites({ authDb, defaultDbPath, defaultSecret }) {
  const sites = new Map();
  sites.set("home", {
    slug: "home",
    name: process.env.SITE_HOME_NAME || "Home",
    secret: defaultSecret || "",
    db: authDb,
    dbPath: defaultDbPath,
    default: true,
  });

  const extra = String(process.env.SITES || "")
    .split(",")
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean);

  for (const slug of extra) {
    if (slug === "home" || isReservedSlug(slug)) {
      console.warn(`sites: ignoring reserved slug "${slug}"`);
      continue;
    }
    const secret = String(process.env[envName(slug, "SECRET")] || "").trim();
    if (!secret) {
      console.warn(`sites: skip ${slug} — set ${envName(slug, "SECRET")}`);
      continue;
    }
    const dbPath = path.join(path.dirname(defaultDbPath), "sites", `${slug}.db`);
    const db = openDatabase(dbPath);
    if (getMeta(db, "use_builtin_loads") == null) {
      setMeta(db, "use_builtin_loads", "0");
    }
    sites.set(slug, {
      slug,
      name: process.env[envName(slug, "NAME")] || slug,
      secret,
      db,
      dbPath,
      default: false,
    });
  }

  return sites;
}

export function listPublicSites(sites) {
  return [...sites.values()].map((s) => ({
    slug: s.slug,
    name: s.name,
    path: s.default ? "/" : `/${s.slug}`,
  }));
}
