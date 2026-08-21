import "dotenv/config";
import { openDatabase, repairDeadSamples } from "./db.js";

const db = openDatabase(process.env.DB_PATH || "./data/solarstats.db");
const result = repairDeadSamples(db);

console.log(`Removed ${result.deleted} zeroed/unavailable sample(s).`);
if (result.latest) {
  console.log(
    `Latest live: ${result.latest.ts} SoC=${result.latest.batterySoc} V=${result.latest.batteryVoltage} PV=${result.latest.pvPower}W Out=${result.latest.outputPower}W`,
  );
} else {
  console.log("No live samples remain in the database.");
}
console.log(`energyKwhTotal=${result.energyKwhTotal}`);
