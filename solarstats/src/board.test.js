import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  getLoadConfig,
  getShowPie,
  openDatabase,
  setDisplayTile,
  setMeta,
  setPieExtra,
  tilesMode,
  tileViews,
  upsertDeviceStates,
} from "./db.js";

function openTemp() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "solarstats-board-"));
  const db = openDatabase(path.join(dir, "site.db"));
  return { dir, db };
}

test("a linked site hides the home pie and uses chosen top boxes", () => {
  const { dir, db } = openTemp();
  setMeta(db, "use_builtin_loads", "0");
  upsertDeviceStates(db, [
    {
      entityId: "sensor.cabin_pv_power",
      name: "PV power",
      state: "840",
      device_class: "power",
      unit: "W",
    },
    {
      entityId: "sensor.cabin_daily",
      name: "Cabin daily",
      state: "3.2",
      device_class: "energy",
      unit: "kWh",
    },
  ]);

  assert.equal(getShowPie(db), false);
  assert.equal(tilesMode(db), "custom");
  assert.equal(getLoadConfig(db).some((row) => row.key === "fridge"), false);
  assert.equal(getLoadConfig(db).some((row) => row.key === "sensor.cabin_daily"), false);

  setPieExtra(db, "sensor.cabin_daily", true);
  assert.deepEqual(
    getLoadConfig(db).map((row) => row.key),
    ["sensor.cabin_daily"],
  );

  setDisplayTile(db, "sensor.cabin_pv_power", true);
  assert.equal(tilesMode(db), "custom");
  assert.deepEqual(tileViews(db), [
    {
      entityId: "sensor.cabin_pv_power",
      label: "PV power",
      state: "840",
      unit: "W",
      domain: "sensor",
      deviceClass: "power",
    },
  ]);

  for (let i = 0; i < 7; i += 1) {
    const entityId = `sensor.extra_${i}`;
    upsertDeviceStates(db, [{ entityId, name: `Extra ${i}`, state: "1", unit: "W" }]);
    setDisplayTile(db, entityId, true);
  }
  assert.throws(
    () => setDisplayTile(db, "sensor.one_too_many", true),
    (err) => err.status === 400,
  );

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});
