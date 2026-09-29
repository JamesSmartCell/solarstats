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
  setDeviceDisplayName,
  setDisplayTile,
  setMeta,
  setGroupExposure,
  setPieSlot,
  setSensorTops,
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
  assert.deepEqual(getLoadConfig(db), []);

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

test("pie slots accept watt sensors and reject other units", () => {
  const { dir, db } = openTemp();
  upsertDeviceStates(db, [
    { entityId: "sensor.fridge_power", name: "Fridge", state: "100", device_class: "power", unit: "W" },
    { entityId: "sensor.cabin_daily", name: "Cabin daily", state: "3.2", device_class: "energy", unit: "kWh" },
  ]);
  assert.throws(
    () => setPieSlot(db, 0, { on: true, entityId: "sensor.cabin_daily" }),
    (err) => err.status === 400,
  );

  const now = Date.now();
  db.prepare(`INSERT INTO sensor_history (entity_id, ts, value) VALUES (?, ?, ?)`).run(
    "sensor.fridge_power",
    now - 60 * 1000,
    100,
  );
  setPieSlot(db, 0, { on: true, entityId: "sensor.fridge_power", source: "inverter", color: "#112233" });
  const slots = setPieSlot(db, 1, { on: false, entityId: "", color: "#abcdef" });
  assert.equal(slots.length, 10);
  assert.equal(slots[0].on, true);
  assert.equal(slots[0].source, "inverter");
  assert.equal(slots[1].on, false);

  const config = getLoadConfig(db);
  assert.equal(config.length, 1);
  assert.equal(config[0].key, "slot0");
  assert.equal(config[0].label, "Fridge");
  setPieSlot(db, 0, { label: "Cold box" });
  assert.equal(getLoadConfig(db)[0].label, "Cold box");
  setDeviceDisplayName(db, "sensor.fridge_power", "Kitchen fridge");
  setPieSlot(db, 0, { label: "" });
  assert.equal(getLoadConfig(db)[0].label, "Kitchen fridge");
  assert.equal(config[0].watts, 100);
  assert.ok(config[0].kwh > 0);

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test("select all and select none apply to one column of one group", () => {
  const { dir, db } = openTemp();
  upsertDeviceStates(db, [
    { entityId: "sensor.cabin_soc", name: "Cabin SoC", state: "76", unit: "%" },
    { entityId: "sensor.cabin_out", name: "Cabin output", state: "410", unit: "W" },
    { entityId: "switch.cabin_pump", name: "Pump", state: "off" },
  ]);

  const asUser = setGroupExposure(db, { group: "sensors", column: "user", mode: "all" });
  assert.equal(asUser.find((row) => row.entityId === "sensor.cabin_soc").exposure, "user");
  assert.equal(asUser.find((row) => row.entityId === "sensor.cabin_out").exposure, "user");
  assert.equal(asUser.find((row) => row.entityId === "switch.cabin_pump").exposure, "admin");

  const sensorsOff = setGroupExposure(db, { group: "sensors", column: "user", mode: "none" });
  assert.equal(sensorsOff.find((row) => row.entityId === "sensor.cabin_soc").exposure, "off");
  assert.equal(sensorsOff.find((row) => row.entityId === "switch.cabin_pump").exposure, "admin");

  const switchesAdmin = setGroupExposure(db, { group: "switches", column: "admin", mode: "none" });
  assert.equal(switchesAdmin.find((row) => row.entityId === "switch.cabin_pump").exposure, "off");
  assert.equal(switchesAdmin.find((row) => row.entityId === "sensor.cabin_soc").exposure, "off");

  const topped = setSensorTops(db, true);
  assert.equal(topped.capped, false);
  assert.equal(tileViews(db).length, 2);
  setSensorTops(db, false);
  assert.deepEqual(tileViews(db), []);

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});
