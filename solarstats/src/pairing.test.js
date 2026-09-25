import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { openDatabase } from "./db.js";
import { checkSiteName, claimPairing, pollPairing, startPairing } from "./pairing.js";
import { isSiteAdmin, loadSites } from "./sites.js";

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

  const claimed = claimPairing(authDb, sites, dbPath, { code: "ab23z" });
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

  const user = authDb.prepare("SELECT status, role FROM users WHERE email = ?").get("owner@example.com");
  assert.equal(user.status, "approved");
  assert.equal(user.role, "user");
  assert.equal(isSiteAdmin(sites.get("rivermill"), "owner@example.com"), true);
  assert.equal(isSiteAdmin(sites.get("rivermill"), "other@example.com"), false);
  assert.equal(isSiteAdmin(sites.get("home"), "owner@example.com"), false);
  assert.throws(
    () => checkSiteName(authDb, sites, "Rivermill"),
    (err) => err.message === "name_taken",
  );
  assert.equal(checkSiteName(authDb, sites, "Other House").slug, "other-house");
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
