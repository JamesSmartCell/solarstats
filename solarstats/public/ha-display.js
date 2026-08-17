export const SENSOR_DOMAINS = new Set(["sensor", "binary_sensor"]);
export const CLICKABLE_DOMAINS = new Set(["switch", "light"]);

const OPENING_CLASSES = new Set(["garage_door", "door", "window", "opening"]);

const BINARY_LABELS = {
  lock: ["Unlocked", "Locked"],
  moisture: ["Wet", "Dry"],
  motion: ["Detected", "Clear"],
  occupancy: ["Detected", "Clear"],
  presence: ["Detected", "Clear"],
  moving: ["Detected", "Clear"],
  smoke: ["Detected", "Clear"],
  gas: ["Detected", "Clear"],
  problem: ["Detected", "Clear"],
  safety: ["Detected", "Clear"],
  tamper: ["Detected", "Clear"],
  sound: ["Detected", "Clear"],
  vibration: ["Detected", "Clear"],
  battery: ["Low", "Normal"],
  cold: ["Cold", "Normal"],
  heat: ["Hot", "Normal"],
  light: ["Detected", "Clear"],
  plug: ["Plugged in", "Unplugged"],
  power: ["Power", "No power"],
  running: ["Running", "Not running"],
  update: ["Update available", "Up-to-date"],
};

export function isSensorDomain(domain) {
  return SENSOR_DOMAINS.has(String(domain || ""));
}

export function isClickableDomain(domain) {
  return CLICKABLE_DOMAINS.has(String(domain || ""));
}

function capitalize(value) {
  const s = String(value || "");
  if (!s) return "";
  return s.charAt(0).toUpperCase() + s.slice(1);
}

/** HA-style label for a sensor / binary_sensor / switch state. */
export function formatHaState(device) {
  const raw = device?.state;
  if (raw == null || raw === "") return "—";
  const state = String(raw);
  const lower = state.toLowerCase();
  const domain = String(device?.domain || "");
  const deviceClass = String(device?.deviceClass || device?.device_class || "");
  const unit = device?.unit;

  if (domain === "binary_sensor" || OPENING_CLASSES.has(deviceClass)) {
    const on = lower === "on";
    const off = lower === "off";
    if (OPENING_CLASSES.has(deviceClass) && (on || off)) {
      return on ? "Open" : "Closed";
    }
    const pair = BINARY_LABELS[deviceClass];
    if (pair && (on || off)) return on ? pair[0] : pair[1];
    if (on || off) return capitalize(lower);
  }

  if (unit && lower !== "unavailable" && lower !== "unknown") {
    return `${state} ${unit}`;
  }
  return state;
}

export function stateTone(device) {
  const lower = String(device?.state || "").toLowerCase();
  if (lower === "on") return "on";
  if (lower === "off") return "off";
  return "";
}
