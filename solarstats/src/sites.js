import fs from "node:fs";
import path from "node:path";
import { getAdminEmail, getMeta, isAdminEmail, openDatabase, setMeta } from "./db.js";

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

/** Home admin is ADMIN_EMAIL. Every other site uses the email stored for that site. */
export function isSiteAdmin(site, email) {
  if (!site || !email) return false;
  if (site.default) return isAdminEmail(email);
  const admin = String(site.adminEmail || "").trim().toLowerCase();
  return Boolean(admin) && admin === String(email).trim().toLowerCase();
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

    CREATE TABLE IF NOT EXISTS site_viewers (
      slug TEXT NOT NULL,
      email TEXT NOT NULL COLLATE NOCASE,
      created_at TEXT NOT NULL,
      PRIMARY KEY (slug, email)
    );
  `);
}

function viewerEmail(email) {
  return String(email || "").trim().toLowerCase();
}

export function listSiteViewers(authDb, slug) {
  ensureSiteRegistry(authDb);
  return authDb
    .prepare(
      `SELECT email, created_at FROM site_viewers WHERE slug = ? ORDER BY email COLLATE NOCASE`,
    )
    .all(String(slug || "").toLowerCase());
}

export function addSiteViewer(authDb, slug, email) {
  ensureSiteRegistry(authDb);
  const address = viewerEmail(email);
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(address)) {
    const err = new Error("invalid_email");
    err.status = 400;
    throw err;
  }
  const key = String(slug || "").toLowerCase();
  authDb
    .prepare(
      `INSERT INTO site_viewers (slug, email, created_at) VALUES (?, ?, ?)
       ON CONFLICT(slug, email) DO NOTHING`,
    )
    .run(key, address, new Date().toISOString());
  return listSiteViewers(authDb, key);
}

export function removeSiteViewer(authDb, slug, email) {
  ensureSiteRegistry(authDb);
  const key = String(slug || "").toLowerCase();
  authDb
    .prepare(`DELETE FROM site_viewers WHERE slug = ? AND email = ? COLLATE NOCASE`)
    .run(key, viewerEmail(email));
  return listSiteViewers(authDb, key);
}

/** Home stays open to every approved login. A paired page is the owner plus people they add. */
export function canViewSite(authDb, site, email) {
  if (!site || !email) return false;
  if (site.default) return true;
  if (isSiteAdmin(site, email)) return true;
  ensureSiteRegistry(authDb);
  const row = authDb
    .prepare(`SELECT 1 AS ok FROM site_viewers WHERE slug = ? AND email = ? COLLATE NOCASE`)
    .get(site.slug, viewerEmail(email));
  return Boolean(row);
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
    adminEmail: getAdminEmail() || null,
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
      adminEmail: String(process.env[envName(slug, "ADMIN")] || "").trim().toLowerCase() || null,
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

function renameSiteFiles(fromPath, toPath) {
  for (const suffix of ["", "-wal", "-shm"]) {
    const from = `${fromPath}${suffix}`;
    const to = `${toPath}${suffix}`;
    if (fs.existsSync(from)) fs.renameSync(from, to);
  }
}

/** Change the dashboard name, address, and admin for a paired home. */
export function applyLinkedSiteProfile(authDb, sites, defaultDbPath, { slug, name, adminEmail, nextSlug }) {
  const site = sites.get(slug);
  if (!site || site.default) {
    const err = new Error("unknown_site");
    err.status = 404;
    throw err;
  }
  const displayName = String(name || "").trim();
  const email = String(adminEmail || "").trim().toLowerCase();
  const target = nextSlug || slug;
  if (target !== slug) {
    const nextPath = siteDbPath(defaultDbPath, target);
    site.db.close();
    renameSiteFiles(site.dbPath, nextPath);
    site.db = openExtraSite(nextPath);
    authDb
      .prepare("UPDATE linked_sites SET slug = ?, name = ?, admin_email = ? WHERE slug = ?")
      .run(target, displayName, email, slug);
    sites.delete(slug);
    site.slug = target;
    site.dbPath = nextPath;
    sites.set(target, site);
  } else {
    authDb
      .prepare("UPDATE linked_sites SET name = ?, admin_email = ? WHERE slug = ?")
      .run(displayName, email, slug);
  }
  site.name = displayName;
  site.adminEmail = email;
  return { slug: site.slug, name: site.name, adminEmail: email, path: `/${site.slug}` };
}

export function setLinkedSiteAdmin(authDb, sites, slug, email) {
  const site = sites.get(slug);
  if (!site || site.default) {
    const err = new Error("unknown_site");
    err.status = 404;
    throw err;
  }
  const adminEmail = String(email || "").trim().toLowerCase();
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(adminEmail)) {
    const err = new Error("invalid_email");
    err.status = 400;
    throw err;
  }
  authDb.prepare("UPDATE linked_sites SET admin_email = ? WHERE slug = ?").run(adminEmail, slug);
  site.adminEmail = adminEmail;
  return { slug: site.slug, name: site.name, adminEmail };
}

/** Which installation `/` shows. Unset or `home` keeps the original dashboard. */
export function resolveRootSite(sites, raw) {
  const slug = String(raw || "home").trim().toLowerCase();
  const home = sites.get("home");
  if (!slug || slug === "home") return home;
  const site = sites.get(slug);
  if (!site) {
    console.warn(`DEFAULT_SITE=${slug} is not a known installation; / stays on home`);
    return home;
  }
  return site;
}

export function listPublicSites(sites) {
  return [...sites.values()].map((s) => ({
    slug: s.slug,
    name: s.name,
    path: s.default ? "/" : `/${s.slug}`,
  }));
}
