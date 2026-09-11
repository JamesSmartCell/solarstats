import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { openCatalog, summarizeMapping } from "../src/catalog.js";

/** Subset of a live GET /api/states dump after the Garden PowMr rename. */
const states = [
  { entity_id: "sensor.powmr_inverter_grid_voltage", state: "17.3999996185303", attributes: { unit_of_measurement: "V", device_class: "voltage", friendly_name: "PowMr Inverter Grid Voltage" }, last_updated: "2026-09-10T22:49:25.848810+00:00" },
  { entity_id: "sensor.garden_powmr_inverter_ac_frequency", state: "unknown", attributes: { unit_of_measurement: "Hz", device_class: "frequency", friendly_name: "PowMr Inverter AC Frequency" }, last_updated: "2026-09-10T04:49:49.559736+00:00" },
  { entity_id: "sensor.powmr_inverter_pv_voltage", state: "232.900009155273", attributes: { unit_of_measurement: "V", device_class: "voltage", friendly_name: "PowMr Inverter PV Voltage" }, last_updated: "2026-09-10T22:49:40.852568+00:00" },
  { entity_id: "sensor.garden_powmr_inverter_pv_power", state: "197.0", attributes: { unit_of_measurement: "W", device_class: "power", friendly_name: "PV Power" }, last_updated: "2026-09-10T22:49:40.853256+00:00" },
  { entity_id: "sensor.powmr_inverter_battery_voltage", state: "26.3000011444092", attributes: { unit_of_measurement: "V", device_class: "voltage", friendly_name: "PowMr Inverter Battery Voltage" }, last_updated: "2026-09-10T22:35:40.786235+00:00" },
  { entity_id: "sensor.powmr_inverter_battery_soc", state: "89.0", attributes: { unit_of_measurement: "%", device_class: "battery", friendly_name: "PowMr Inverter Battery SoC" }, last_updated: "2026-09-10T22:42:10.824149+00:00" },
  { entity_id: "sensor.powmr_inverter_battery_charge_current", state: "4.0", attributes: { unit_of_measurement: "A", device_class: "current", friendly_name: "PowMr Inverter Battery Charge Current" }, last_updated: "2026-09-10T22:48:25.832123+00:00" },
  { entity_id: "sensor.garden_powmr_inverter_battery_discharge_current", state: "0.0", attributes: { unit_of_measurement: "A", device_class: "current", friendly_name: "PowMr Inverter Battery Discharge Current" }, last_updated: "2026-09-10T21:33:55.696467+00:00" },
  { entity_id: "sensor.garden_powmr_inverter_output_power", state: "41.0", attributes: { unit_of_measurement: "W", device_class: "power", friendly_name: "Inverter Output" }, last_updated: "2026-09-10T22:49:26.808662+00:00" },
  { entity_id: "sensor.garden_powmr_inverter_output_power_inverted", state: "-41.0", attributes: { unit_of_measurement: "W", device_class: "power", friendly_name: "PowMr Inverter Inverter Output Inverted" }, last_updated: "2026-09-10T22:49:26.809078+00:00" },
  { entity_id: "sensor.powmr_inverter_load_percent", state: "0.75", attributes: { unit_of_measurement: "%", friendly_name: "PowMr Inverter Load Percent" }, last_updated: "2026-09-10T22:49:41.808246+00:00" },
  { entity_id: "sensor.solar_production_power", state: "197.0", attributes: { unit_of_measurement: "W", device_class: "power", friendly_name: "Solar production power" }, last_updated: "2026-09-10T22:49:40.855279+00:00" },
  { entity_id: "sensor.inverter_supply_power", state: "41.0", attributes: { unit_of_measurement: "W", device_class: "power", friendly_name: "Inverter supply power" }, last_updated: "2026-09-10T22:49:26.811484+00:00" },
  { entity_id: "sensor.office_pc_synth_energy_daily", state: "0.07847", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Office PC energy" }, last_updated: "2026-09-10T22:49:00.101508+00:00" },
  { entity_id: "sensor.front_room_pc_synth_energy_daily", state: "0.001117", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Front Room PC energy" }, last_updated: "2026-09-10T21:49:00.105390+00:00" },
  { entity_id: "sensor.pi5_server_energy_daily_2", state: "0.01772", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Pi5 Server energy" }, last_updated: "2026-09-10T22:49:00.110730+00:00" },
  { entity_id: "sensor.motorbike_charger_energy_daily_2", state: "0.034237", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Motorbike charger" }, last_updated: "2026-09-10T22:31:00.112419+00:00" },
  { entity_id: "sensor.fridge_energy_daily_2", state: "0.427514", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Fridge" }, last_updated: "2026-09-10T22:49:00.117674+00:00" },
  { entity_id: "sensor.inverter_loads", state: "0", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Inverter loads" }, last_updated: "2026-09-10T14:00:00.012327+00:00" },
  { entity_id: "sensor.inverter_unmetered", state: "0.261", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Inverter unmetered" }, last_updated: "2026-09-10T22:48:41.809123+00:00" },
  { entity_id: "sensor.router_energy_daily", state: "0.16046", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "Router" }, last_updated: "2026-09-10T22:49:00.124797+00:00" },
  { entity_id: "sensor.nbn_modem_energy_daily", state: "unavailable", attributes: { unit_of_measurement: "kWh", device_class: "energy", friendly_name: "NBN Modem energy daily" }, last_updated: "2026-09-10T04:50:05.128466+00:00" },
  { entity_id: "sensor.ts011f_power_3", state: "unknown", attributes: { unit_of_measurement: "W", device_class: "power", friendly_name: "NBN modem Power" }, last_updated: "2026-09-10T04:49:52.799721+00:00" },
];

const dir = mkdtempSync(path.join(tmpdir(), "solarstats-catalog-"));
const catalog = openCatalog(path.join(dir, "catalog.db"));
const now = Date.parse("2026-09-10T22:50:00Z");
catalog.ingest(states, now);
const { core, loads, mapping } = catalog.resolveAll(now);
const health = summarizeMapping(mapping);
const bound = catalog.db.prepare("SELECT field_key, entity_id, source, last_value FROM field_bindings ORDER BY field_key").all();
catalog.close();

const rows = Object.entries(mapping).map(([key, item]) => ({
  key,
  status: item.status,
  source: item.source,
  entityId: item.entityId,
  value: item.value,
  stale: item.stale,
}));

console.log(JSON.stringify({ health, core, loads, rows, bound }, null, 2));
writeFileSync(path.join(dir, "out.json"), JSON.stringify({ health, core, loads, rows, bound }, null, 2));
rmSync(dir, { recursive: true, force: true });
