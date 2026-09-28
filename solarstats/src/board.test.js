import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  getChartConfig,
  getChartHistory,
  getLoadConfig,
  getShowPie,
  insertSample,
  openDatabase,
  setChartSeries,
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

test("chart switches plot the chosen sensor and stay off until selected", () => {
  const { dir, db } = openTemp();
  setMeta(db, "use_builtin_loads", "0");
  upsertDeviceStates(db, [
    { entityId: "sensor.cabin_soc", name: "Cabin SoC", state: "76", unit: "%" },
    { entityId: "sensor.cabin_out", name: "Cabin output", state: "410", unit: "W" },
  ]);

  assert.equal(getChartConfig(db).battery.show, false);
  assert.equal(getChartConfig(db).battery.legacy, false);
  assert.equal(getChartConfig(db).inverter.show, false);

  setChartSeries(db, "battery", { show: true, entityId: "sensor.cabin_soc" });
  setChartSeries(db, "inverter", { show: true, entityId: "sensor.cabin_out" });
  const ts = Date.now();
  const saved = insertSample(db, {
    ts: new Date(ts).toISOString(),
    devices: [
      { entityId: "sensor.cabin_soc", name: "Cabin SoC", state: "77", unit: "%" },
      { entityId: "sensor.cabin_out", name: "Cabin output", state: "420", unit: "W" },
    ],
  });
  assert.equal(saved.chartPoints.battery.value, 77);
  assert.equal(saved.chartPoints.inverter.value, 420);

  const charts = getChartHistory(db, "1h");
  assert.equal(charts.battery.show, true);
  assert.equal(charts.battery.legacy, false);
  assert.equal(charts.battery.label, "Cabin SoC");
  assert.equal(charts.battery.points.at(-1).y, 77);
  assert.equal(charts.inverter.points.at(-1).y, 420);

  const homeDir = fs.mkdtempSync(path.join(os.tmpdir(), "solarstats-board-"));
  const home = openDatabase(path.join(homeDir, "home.db"));
  assert.equal(getChartConfig(home).battery.show, true);
  assert.equal(getChartConfig(home).battery.legacy, true);
  assert.equal(getChartConfig(home).inverter.legacy, true);
  home.close();
  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
  fs.rmSync(homeDir, { recursive: true, force: true });
});
