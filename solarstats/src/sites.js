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
  "fw",
  "connect",
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

export function ensureSiteRegistry(authDb) {
  authDb.exec(`
    CREATE TABLE IF NOT EXISTS linked_sites (
      slug TEXT PRIMARY KEY,
      name TEXT NOT NULL,
      secret TEXT NOT NULL,
      admin_email TEXT,
      created_at TEXT NOT NULL
    );
  `);
}

function openExtraSite(dbPath) {
  const db = openDatabase(dbPath);
  if (getMeta(db, "use_builtin_loads") == null) {
    setMeta(db, "use_builtin_loads", "0");
  }
  return db;
}

export function siteDbPath(defaultDbPath, slug) {
  return path.join(path.dirname(defaultDbPath), "sites", `${slug}.db`);
}

/** Open a paired site DB and add it to the live map. Persists the secret in the auth DB. */
export function attachLinkedSite(sites, authDb, defaultDbPath, { slug, name, secret, adminEmail }) {
  ensureSiteRegistry(authDb);
  const dbPath = siteDbPath(defaultDbPath, slug);
  const now = new Date().toISOString();
  authDb
    .prepare(
      `INSERT INTO linked_sites (slug, name, secret, admin_email, created_at)
       VALUES (?, ?, ?, ?, ?)
       ON CONFLICT(slug) DO UPDATE SET
         name = excluded.name,
         secret = excluded.secret,
         admin_email = excluded.admin_email`,
    )
    .run(slug, name, secret, adminEmail || null, now);
  const site = {
    slug,
    name,
    secret,
    db: openExtraSite(dbPath),
    dbPath,
    default: false,
    adminEmail: adminEmail || null,
  };
  sites.set(slug, site);
  return site;
}

/**
 * Home uses the existing DB + INGEST_SECRET.
 * Extra slugs in SITES=rivermill,other each need SITE_<SLUG>_SECRET
 * and get data/sites/<slug>.db
 */
export function loadSites({ authDb, defaultDbPath, defaultSecret }) {
  ensureSiteRegistry(authDb);
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
    sites.set(slug, {
      slug,
      name: process.env[envName(slug, "NAME")] || slug,
      secret,
      db: openExtraSite(siteDbPath(defaultDbPath, slug)),
      dbPath: siteDbPath(defaultDbPath, slug),
      default: false,
    });
  }

  const linked = authDb.prepare("SELECT slug, name, secret, admin_email FROM linked_sites").all();
  for (const row of linked) {
    if (sites.has(row.slug) || row.slug === "home" || isReservedSlug(row.slug)) {
      console.warn(`sites: skip linked slug "${row.slug}"`);
      continue;
    }
    sites.set(row.slug, {
      slug: row.slug,
      name: row.name,
      secret: row.secret,
      db: openExtraSite(siteDbPath(defaultDbPath, row.slug)),
      dbPath: siteDbPath(defaultDbPath, row.slug),
      default: false,
      adminEmail: row.admin_email || null,
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
