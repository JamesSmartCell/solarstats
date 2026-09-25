import crypto from "node:crypto";
import { ensureApprovedUser } from "./db.js";
import { attachLinkedSite, isReservedSlug } from "./sites.js";

export const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const CODE_RE = new RegExp(`^[${CODE_ALPHABET}]{5}$`);
const PAIR_TTL_MS = 10 * 60 * 1000;

function hashValue(value) {
  return crypto.createHash("sha256").update(String(value)).digest("hex");
}

export function ensurePairingTables(authDb) {
  authDb.exec(`
    CREATE TABLE IF NOT EXISTS pending_links (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      code_hash TEXT NOT NULL,
      poll_token_hash TEXT NOT NULL UNIQUE,
      name TEXT NOT NULL,
      email TEXT NOT NULL,
      slug TEXT,
      secret TEXT,
      status TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL
    );
  `);
}

export function normalizeCode(code) {
  return String(code || "")
    .trim()
    .toUpperCase()
    .replace(/[^A-Z0-9]/g, "");
}

export function slugFromName(name) {
  const base = String(name || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 32);
  if (!base || base === "home" || isReservedSlug(base)) return "";
  return base;
}

/** The typed name must map to a free address. Taken names are refused, not renamed. */
export function checkSiteName(authDb, sites, name, now = Date.now()) {
  ensurePairingTables(authDb);
  const displayName = String(name || "").trim();
  if (!displayName || displayName.length > 80) fail(400, "invalid_name");
  const slug = slugFromName(displayName);
  if (!slug) fail(400, "invalid_name");
  if (sites.has(slug)) fail(409, "name_taken");
  const pending = authDb
    .prepare(
      `SELECT id FROM pending_links
       WHERE slug = ? AND status = 'pending' AND expires_at > ?`,
    )
    .get(slug, now);
  if (pending) fail(409, "name_taken");
  return { ok: true, name: displayName, slug };
}

function fail(status, message) {
  const err = new Error(message);
  err.status = status;
  throw err;
}

export function startPairing(authDb, sites, { code, name, email, pollToken, now = Date.now() }) {
  ensurePairingTables(authDb);
  const normalized = normalizeCode(code);
  if (!CODE_RE.test(normalized)) fail(400, "invalid_code");
  const { name: displayName, slug } = checkSiteName(authDb, sites, name, now);
  const adminEmail = String(email || "").trim().toLowerCase();
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(adminEmail)) fail(400, "invalid_email");
  const token = String(pollToken || "").trim();
  if (token.length < 20 || token.length > 200) fail(400, "invalid_poll_token");

  const codeHash = hashValue(normalized);
  const pending = authDb
    .prepare(
      `SELECT id FROM pending_links
       WHERE code_hash = ? AND status = 'pending' AND expires_at > ?`,
    )
    .get(codeHash, now);
  if (pending) fail(409, "code_in_use");

  authDb
    .prepare(
      `INSERT INTO pending_links
        (code_hash, poll_token_hash, name, email, slug, status, created_at, expires_at)
       VALUES (?, ?, ?, ?, ?, 'pending', ?, ?)`,
    )
    .run(codeHash, hashValue(token), displayName, adminEmail, slug, now, now + PAIR_TTL_MS);

  return { ok: true, expiresAt: now + PAIR_TTL_MS };
}

/**
 * User typed the code. Creates the site and holds the ingest secret for the HACS poller.
 */
export function claimPairing(authDb, sites, defaultDbPath, { code, now = Date.now() }) {
  ensurePairingTables(authDb);
  const normalized = normalizeCode(code);
  if (!CODE_RE.test(normalized)) fail(400, "invalid_code");

  const row = authDb
    .prepare(
      `SELECT * FROM pending_links
       WHERE code_hash = ? AND status = 'pending'
       ORDER BY id DESC LIMIT 1`,
    )
    .get(hashValue(normalized));
  if (!row || row.expires_at <= now) fail(400, "code_not_found");

  const slug = row.slug || slugFromName(row.name);
  if (!slug || sites.has(slug)) fail(409, "name_taken");
  const secret = crypto.randomBytes(32).toString("hex");
  const site = attachLinkedSite(sites, authDb, defaultDbPath, {
    slug,
    name: row.name,
    secret,
    adminEmail: row.email,
  });
  ensureApprovedUser(authDb, { email: row.email, displayName: row.name });

  const updated = authDb
    .prepare(
      `UPDATE pending_links
       SET status = 'claimed', slug = ?, secret = ?
       WHERE id = ? AND status = 'pending'`,
    )
    .run(slug, secret, row.id);
  if (!updated.changes) fail(409, "code_not_found");

  return {
    ok: true,
    slug: site.slug,
    name: site.name,
    path: `/${site.slug}`,
    email: row.email,
  };
}

/** HACS polls with the secret token. The ingest secret is returned once. */
export function pollPairing(authDb, { pollToken, now = Date.now() }) {
  ensurePairingTables(authDb);
  const token = String(pollToken || "").trim();
  if (token.length < 20) fail(400, "invalid_poll_token");
  const row = authDb
    .prepare("SELECT * FROM pending_links WHERE poll_token_hash = ?")
    .get(hashValue(token));
  if (!row) fail(404, "unknown_pair");
  if (row.status === "pending") {
    if (row.expires_at <= now) return { status: "expired" };
    return { status: "pending", expiresAt: row.expires_at };
  }
  if (row.status === "claimed" && row.secret) {
    authDb
      .prepare("UPDATE pending_links SET status = 'delivered', secret = NULL WHERE id = ?")
      .run(row.id);
    return { status: "linked", slug: row.slug, secret: row.secret };
  }
  if (row.status === "delivered" || row.status === "claimed") {
    return { status: "linked", slug: row.slug };
  }
  return { status: "expired" };
}
