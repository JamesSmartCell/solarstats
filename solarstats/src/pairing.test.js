import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { openDatabase } from "./db.js";
import { checkSiteName, claimPairing, pollPairing, startPairing, updateSiteProfile } from "./pairing.js";
import {
  addSiteViewer,
  allowSiteAccess,
  canViewSite,
  denySiteAccess,
  isSiteAdmin,
  listAccessRequests,
  loadSites,
  removeSiteViewer,
  requestSiteAccess,
  resolveRootSite,
} from "./sites.js";

function fixture() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "solarstats-pair-"));
  const dbPath = path.join(dir, "solarstats.db");
  const authDb = openDatabase(dbPath);
  const sites = loadSites({ authDb, defaultDbPath: dbPath, defaultSecret: "home-secret" });
  return { dir, dbPath, authDb, sites };
}

test("code claim creates a site and reveals the ingest secret once", () => {
  const { dir, dbPath, authDb, sites } = fixture();
  const pollToken = "poll-token-0123456789abcdef";
  startPairing(authDb, sites, {
    code: "AB23Z",
    name: "Rivermill",
    email: "owner@example.com",
    pollToken,
  });

  assert.equal(pollPairing(authDb, { pollToken }).status, "pending");

  const claimed = claimPairing(authDb, sites, dbPath, {
    code: "ab23z",
    claimerEmail: "claimer@example.com",
  });
  assert.equal(claimed.slug, "rivermill");
  assert.equal(claimed.path, "/rivermill");
  assert.equal(sites.get("rivermill").name, "Rivermill");

  const linked = pollPairing(authDb, { pollToken });
  assert.equal(linked.status, "linked");
  assert.equal(linked.secret, sites.get("rivermill").secret);
  assert.equal(linked.secret.length, 64);

  const again = pollPairing(authDb, { pollToken });
  assert.equal(again.secret, undefined);
  assert.equal(again.slug, "rivermill");

  const reloaded = loadSites({
    authDb,
    defaultDbPath: dbPath,
    defaultSecret: "home-secret",
  });
  assert.equal(reloaded.get("rivermill").secret, linked.secret);
  reloaded.get("rivermill").db.close();

  const user = authDb.prepare("SELECT status, role FROM users WHERE email = ?").get("claimer@example.com");
  assert.equal(user.status, "approved");
  assert.equal(user.role, "user");
  assert.equal(canViewSite(authDb, sites.get("rivermill"), "guest@example.com"), false);
  addSiteViewer(authDb, "rivermill", "Guest@Example.com");
  assert.equal(canViewSite(authDb, sites.get("rivermill"), "guest@example.com"), true);
  assert.equal(canViewSite(authDb, sites.get("rivermill"), "claimer@example.com"), true);
  assert.equal(canViewSite(authDb, sites.get("home"), "guest@example.com"), true);
  assert.equal(resolveRootSite(sites, "").slug, "home");
  assert.equal(resolveRootSite(sites, "rivermill").slug, "rivermill");
  assert.equal(resolveRootSite(sites, "missing").slug, "home");
  removeSiteViewer(authDb, "rivermill", "guest@example.com");
  assert.equal(canViewSite(authDb, sites.get("rivermill"), "guest@example.com"), false);
  assert.equal(isSiteAdmin(sites.get("rivermill"), "claimer@example.com"), true);
  assert.equal(isSiteAdmin(sites.get("rivermill"), "owner@example.com"), false);
  assert.equal(isSiteAdmin(sites.get("home"), "owner@example.com"), false);
  assert.throws(
    () => checkSiteName(authDb, sites, "Rivermill"),
    (err) => err.message === "name_taken",
  );
  assert.equal(checkSiteName(authDb, sites, "Other House").slug, "other-house");
  const renamed = updateSiteProfile(authDb, sites, dbPath, {
    slug: "rivermill",
    name: "Mill House",
    adminEmail: "new-owner@example.com",
  });
  assert.equal(renamed.slug, "mill-house");
  assert.equal(renamed.path, "/mill-house");
  assert.equal(isSiteAdmin(sites.get("mill-house"), "new-owner@example.com"), true);
  assert.equal(sites.has("rivermill"), false);
  for (const site of reloaded.values()) site.db.close();
  for (const site of sites.values()) site.db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test("unknown and expired codes are rejected", () => {
  const { dir, authDb, sites, dbPath } = fixture();
  assert.throws(
    () => claimPairing(authDb, sites, dbPath, { code: "ZZZZZ" }),
    (err) => err.message === "code_not_found",
  );
  startPairing(authDb, sites, {
    code: "H3K7M",
    name: "Shed",
    email: "shed@example.com",
    pollToken: "another-poll-token-0123456789",
    now: Date.now() - 11 * 60 * 1000,
  });
  assert.throws(
    () => claimPairing(authDb, sites, dbPath, { code: "H3K7M" }),
    (err) => err.message === "code_not_found",
  );
  for (const site of sites.values()) site.db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test("an access request is allowed onto the viewer list or wiped", () => {
  const { dir, dbPath, authDb, sites } = fixture();
  startPairing(authDb, sites, {
    code: "K7M3P",
    name: "Verdun",
    email: "owner@example.com",
    pollToken: "access-poll-token-0123456789ab",
  });
  const claimed = claimPairing(authDb, sites, dbPath, {
    code: "K7M3P",
    claimerEmail: "claimer@example.com",
  });
  const slug = claimed.slug;
  assert.throws(
    () => requestSiteAccess(authDb, "home", "guest@example.com"),
    (err) => err.message === "not_requestable",
  );
  requestSiteAccess(authDb, slug, "Guest@Example.com");
  requestSiteAccess(authDb, slug, "guest@example.com");
  assert.deepEqual(
    listAccessRequests(authDb, slug).map((row) => row.email),
    ["guest@example.com"],
  );
  assert.equal(canViewSite(authDb, sites.get(slug), "guest@example.com"), false);

  denySiteAccess(authDb, slug, "guest@example.com");
  assert.equal(listAccessRequests(authDb, slug).length, 0);
  assert.equal(canViewSite(authDb, sites.get(slug), "guest@example.com"), false);

  requestSiteAccess(authDb, slug, "guest@example.com");
  const allowed = allowSiteAccess(authDb, slug, "guest@example.com");
  assert.equal(allowed.requests.length, 0);
  assert.equal(canViewSite(authDb, sites.get(slug), "guest@example.com"), true);
  assert.equal(
    allowed.viewers.some((row) => row.email === "guest@example.com"),
    true,
  );

  for (const site of sites.values()) site.db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});
