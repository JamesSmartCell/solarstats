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
  getTheme,
  insertSample,
  openDatabase,
  setChartSeries,
  setDeviceDisplayName,
  setDisplayTile,
  setMeta,
  setGroupExposure,
  setPieSlot,
  setSensorTops,
  setTheme,
  tilesMode,
  tileViews,
  upsertDeviceStates,
  getLatestLoadsPower,
  completeDeviceCommand,
  enqueueDeviceCommand,
  getDevice,
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
  assert.equal(getTheme(db), "standard");
  assert.equal(setTheme(db, "lcars"), "lcars");
  assert.equal(getTheme(db), "lcars");
  assert.equal(setTheme(db, "jarvis"), "jarvis");
  assert.equal(getTheme(db), "jarvis");
  assert.equal(setTheme(db, "expanse"), "expanse");
  assert.equal(setTheme(db, "nope"), "standard");
  assert.equal(setTheme(db, "standard"), "standard");
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

test("a completed switch keeps its reported state through the next snapshot", () => {
  const { dir, db } = openTemp();
  upsertDeviceStates(db, [{ entityId: "switch.pump", name: "Pump", state: "on" }]);
  const id = enqueueDeviceCommand(db, { entityId: "switch.pump", action: "toggle", userId: null });
  const done = completeDeviceCommand(db, id, true, "off");
  assert.equal(done.state, "off");
  assert.equal(getDevice(db, "switch.pump").state, "off");

  upsertDeviceStates(db, [{ entityId: "switch.pump", name: "Pump", state: "on" }]);
  assert.equal(getDevice(db, "switch.pump").state, "off");

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test("solar-marked loads are removed from inverter supply", () => {
  const { dir, db } = openTemp();
  upsertDeviceStates(db, [
    {
      entityId: "sensor.powmr_inverter_output_power",
      name: "Inverter supply",
      state: "142",
      unit: "W",
      device_class: "power",
    },
    {
      entityId: "sensor.office_pc_power",
      name: "Office PC",
      state: "112",
      unit: "W",
      device_class: "power",
    },
    {
      entityId: "sensor.fridge_power",
      name: "Fridge",
      state: "80",
      unit: "W",
      device_class: "power",
    },
  ]);
  setChartSeries(db, "inverter", { show: true, entityId: "sensor.powmr_inverter_output_power" });
  setPieSlot(db, 0, { on: true, entityId: "sensor.powmr_inverter_output_power", source: "inverter" });
  setPieSlot(db, 1, { on: true, entityId: "sensor.office_pc_power", source: "inverter" });
  setPieSlot(db, 2, { on: true, entityId: "sensor.fridge_power", source: "grid" });

  const power = getLatestLoadsPower(db);
  assert.equal(power.slot0, 30);
  assert.equal(power.slot1, 112);
  assert.equal(power.slot2, 80);

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test("a plug keeps its last watts while its switch stays on", () => {
  const { dir, db } = openTemp();
  const plug = (power, switchState) =>
    insertSample(db, {
      devices: [
        {
          entityId: "sensor.ts011f_power",
          name: "Pi5 power",
          state: power,
          unit: "W",
          device_class: "power",
          device_id: "plug1",
        },
        {
          entityId: "switch.pi5_server",
          name: "Pi5",
          state: switchState,
          device_id: "plug1",
        },
      ],
    });

  plug("4", "on");
  setPieSlot(db, 0, { on: true, entityId: "sensor.ts011f_power", source: "grid" });
  assert.equal(getLatestLoadsPower(db).slot0, 4);

  plug("unavailable", "on");
  assert.equal(getLatestLoadsPower(db).slot0, 4);

  plug("0", "on");
  assert.equal(getLatestLoadsPower(db).slot0, 0);

  plug("4", "on");
  plug("unavailable", "off");
  assert.equal(getLatestLoadsPower(db).slot0, 0);

  plug("4", "on");
  plug("4", "unavailable");
  assert.equal(getLatestLoadsPower(db).slot0, 0);

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});
