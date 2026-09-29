import { formatHaState, isSensorDomain, stateTone } from "./ha-display.js";

const SITE =
  document.querySelector('meta[name="solarstats-site"]')?.content || "home";

function withSite(path) {
  const u = new URL(path, location.origin);
  if (SITE && SITE !== "home") {
    u.searchParams.set("site", SITE);
  }
  return `${u.pathname}${u.search}`;
}

const RANGE_LABELS = {
  "1h": "1 hour",
  "6h": "6 hours",
  "12h": "12 hours",
  "24h": "24 hours",
  "3d": "3 days",
  "7d": "7 days",
  "30d": "30 days",
};

function isSolarSource(source) {
  return source === "inverter" || source === "solar";
}

const LOAD_SLICES = [
  { key: "officePc", label: "Office PC", color: "#42a5f5", source: "grid" },
  { key: "frontRoomPc", label: "Front Room PC", color: "#5c6bc0", source: "grid" },
  { key: "pi5", label: "Pi5 Server", color: "#7e57c2", source: "grid" },
  { key: "motorbike", label: "Motorbike", color: "#26a69a", source: "grid" },
  { key: "fridge", label: "Fridge", color: "#66bb6a", source: "grid" },
  { key: "washingMachine", label: "Washing machine", color: "#8bc34a", source: "inverter" },
  { key: "otherInverter", label: "Other inverter", color: "#cddc39", source: "inverter" },
];

const els = {
  connection: document.getElementById("connection"),
  lastUpdate: document.getElementById("lastUpdate"),
  batterySoc: document.getElementById("batterySoc"),
  batteryVoltage: document.getElementById("batteryVoltage"),
  chargeCurrent: document.getElementById("chargeCurrent"),
  dischargeCurrent: document.getElementById("dischargeCurrent"),
  pvVoltage: document.getElementById("pvVoltage"),
  pvPower: document.getElementById("pvPower"),
  outputPower: document.getElementById("outputPower"),
  loadPercent: document.getElementById("loadPercent"),
  energyTotal: document.getElementById("energyTotal"),
  rangeSelect: document.getElementById("rangeSelect"),
  resetZoom: document.getElementById("resetZoom"),
  footNote: document.getElementById("footNote"),
  socHint: document.getElementById("socHint"),
  inverterHint: document.getElementById("inverterHint"),
  adminLink: document.getElementById("adminLink"),
  deviceGrid: document.getElementById("deviceGrid"),
  sensorSection: document.getElementById("sensorSection"),
  sensorList: document.getElementById("sensorList"),
  currentLoadsList: document.getElementById("currentLoadsList"),
  currentLoadsTotal: document.getElementById("currentLoadsTotal"),
};

const state = {
  range: els.rangeSelect.value || "24h",
  samples: [],
  energyKwhTotal: 0,
  rangeStartMs: 0,
  loadsDailyKwh: null,
  loadsPowerW: null,
  loadSlices: SITE === "home" ? LOAD_SLICES.map((s) => ({ ...s })) : [],
  loadConfigApplied: SITE !== "home",
  tilesMode: SITE === "home" ? "legacy" : "custom",
  showPie: SITE === "home",
  customTiles: [],
  charts: {
    battery: { show: SITE === "home", legacy: SITE === "home", points: [] },
    inverter: { show: SITE === "home", legacy: SITE === "home", points: [] },
  },
  devices: [],
  pendingToggles: new Map(),
  pieRows: [],
};

const TOGGLE_LOCK_MS = 20000;

function switchState(device) {
  const value = String(device?.state || "").toLowerCase();
  if (value === "on") return "on";
  if (value === "off") return "off";
  return null;
}

function clearPendingToggle(entityId) {
  const pending = state.pendingToggles.get(entityId);
  if (pending?.timer) clearTimeout(pending.timer);
  state.pendingToggles.delete(entityId);
}

function expirePendingToggle(entityId) {
  const pending = state.pendingToggles.get(entityId);
  if (!pending) return;
  if (pending.timer) clearTimeout(pending.timer);
  state.pendingToggles.delete(entityId);
  state.devices = state.devices.map((device) =>
    device.entityId === entityId
      ? { ...device, state: pending.from, on: pending.from === "on" }
      : device,
  );
  paintDevices();
}

function startPendingToggle(entityId, from) {
  clearPendingToggle(entityId);
  const to = from === "on" ? "off" : "on";
  const timer = setTimeout(() => expirePendingToggle(entityId), TOGGLE_LOCK_MS);
  state.pendingToggles.set(entityId, { from, to, timer });
}

function overlayPendingDevice(device) {
  const pending = state.pendingToggles.get(device.entityId);
  if (!pending) return device;
  return { ...device, state: pending.to, on: pending.to === "on" };
}

function confirmPendingFrom(devices) {
  for (const device of devices) {
    const pending = state.pendingToggles.get(device.entityId);
    if (!pending) continue;
    if (switchState(device) === pending.to) {
      clearPendingToggle(device.entityId);
    }
  }
}

function applyLoadConfig(config) {
  if (!Array.isArray(config)) return;
  state.loadConfigApplied = true;
  state.loadSlices = config
    .filter((s) => s.onPie !== false)
    .map((s) => ({
      key: s.key,
      label: s.label || s.key,
      color: s.color || "#90a4ae",
      source: isSolarSource(s.source) ? "inverter" : "grid",
      members: Array.isArray(s.members) ? s.members.filter(Boolean) : [],
    }));
}

function applyCharts(charts) {
  if (!charts) return;
  for (const key of ["battery", "inverter"]) {
    const next = charts[key];
    if (!next) continue;
    const prev = state.charts[key] || {};
    state.charts[key] = {
      ...prev,
      ...next,
      points: Array.isArray(next.points) ? next.points : prev.points || [],
    };
  }
}

function chartCaption(slot, legacyText) {
  const label = RANGE_LABELS[state.range] || state.range;
  if (slot?.legacy) return `${legacyText} · ${label}`;
  if (!slot?.entityId) return `Choose a sensor in admin · ${label}`;
  const name = slot.label || slot.entityId;
  const unit = slot.unit ? ` · ${slot.unit}` : "";
  return `${name}${unit} · ${label}`;
}

function paintChartPanels() {
  const battery = state.charts.battery || {};
  const inverter = state.charts.inverter || {};
  const batteryPanel = document.getElementById("batteryPanel");
  const inverterPanel = document.getElementById("inverterPanel");
  const reveal = (panel, show, chart) => {
    if (!panel || !chart) return;
    const wasHidden = panel.hidden;
    panel.hidden = !show;
    if (wasHidden && show) {
      chart.resize();
      chart.update("none");
    }
  };
  reveal(batteryPanel, !!battery.show, typeof socChart !== "undefined" ? socChart : null);
  reveal(inverterPanel, !!inverter.show, typeof outChart !== "undefined" ? outChart : null);
  if (els.socHint) els.socHint.textContent = chartCaption(battery, "State of charge");
  if (els.inverterHint) {
    els.inverterHint.textContent = chartCaption(
      inverter,
      "Output power (W) with cumulative energy under the curve (kWh)",
    );
  }
}

function appendChartPoint(key, point) {
  if (!point || point.value == null) return;
  const slot = state.charts[key];
  if (!slot?.entityId) return;
  const x = new Date(point.ts).toISOString();
  const points = slot.points || (slot.points = []);
  const last = points[points.length - 1];
  if (last && last.x === x) last.y = point.value;
  else points.push({ x, y: point.value });
}

function applyBoard(payload) {
  if (!payload) return;
  if (payload.tilesMode) state.tilesMode = payload.tilesMode;
  if (payload.showPie != null) state.showPie = !!payload.showPie;
  if (Array.isArray(payload.tiles)) state.customTiles = payload.tiles;
  applyCharts(payload.charts);
  paintTiles();
  paintPie();
  paintChartPanels();
}

function paintPie() {
  const panel = document.getElementById("piePanel");
  if (!panel) return;
  const show = !!state.showPie;
  const reveal = panel.hidden && show;
  panel.hidden = !show;
  if (reveal) {
    loadsPieChart.resize();
    loadsPieChart.update("none");
  }
}

function paintTiles() {
  const legacy = document.getElementById("legacyTiles");
  const custom = document.getElementById("customTiles");
  const row = document.getElementById("tileRow");
  const customMode = state.tilesMode === "custom";
  if (legacy) legacy.hidden = customMode;
  if (customMode) renderCustomTiles();
  if (custom) custom.hidden = !customMode || state.customTiles.length === 0;
  if (row) row.hidden = customMode && state.customTiles.length === 0;
}

function renderCustomTiles() {
  const root = document.getElementById("customTiles");
  if (!root) return;
  root.replaceChildren();
  for (const tile of state.customTiles) {
    const article = document.createElement("article");
    article.className = "tile";
    const label = document.createElement("div");
    label.className = "label";
    label.textContent = tile.label || tile.entityId;
    const value = document.createElement("div");
    value.className = "value";
    const raw = String(tile.state ?? "").toLowerCase();
    value.textContent =
      tile.state == null || tile.state === "" || raw === "unavailable" || raw === "unknown"
        ? "—"
        : formatHaState(tile);
    article.append(label, value);
    root.appendChild(article);
  }
}

function sliceKeys(slice) {
  return [slice.key, ...(slice.members || [])];
}

function sumMapValues(map, keys) {
  let total = 0;
  for (const key of keys) {
    const n = Number(map?.[key]);
    if (Number.isFinite(n) && n > 0) total += n;
  }
  return total;
}

function currentLoadSlices() {
  if (state.loadConfigApplied || state.tilesMode === "custom") return state.loadSlices;
  return state.loadSlices.length ? state.loadSlices : LOAD_SLICES;
}

function pieChartModel(slices, kwhMap) {
  const inverter = [];
  const grid = [];
  for (const slice of slices) {
    (isSolarSource(slice.source) ? inverter : grid).push(slice);
  }
  const invVals = inverter.map((s) => sumMapValues(kwhMap, sliceKeys(s)));
  const gridVals = grid.map((s) => sumMapValues(kwhMap, sliceKeys(s)));
  const rows = [];
  inverter.forEach((slice, i) => rows.push({ slice, value: invVals[i] }));
  grid.forEach((slice, i) => rows.push({ slice, value: gridVals[i] }));
  return rows;
}

function rangeToMs(range) {
  const match = /^(\d+)([hdw])$/i.exec(range || "24h");
  if (!match) return 24 * 3600000;
  const n = Number(match[1]);
  const unit = match[2].toLowerCase();
  if (unit === "w") return n * 7 * 86400000;
  if (unit === "d") return n * 86400000;
  return n * 3600000;
}

/** HA-like light EMA smoothing for display (does not alter stored samples). */
function smoothSeries(points, alpha = 0.22) {
  let ema = null;
  return points.map((p) => {
    if (p.y == null || Number.isNaN(Number(p.y))) return p;
    const y = Number(p.y);
    ema = ema == null ? y : alpha * y + (1 - alpha) * ema;
    return { x: p.x, y: ema };
  });
}

const zoomOptions = {
  pan: {
    enabled: true,
    mode: "x",
    modifierKey: null,
  },
  zoom: {
    wheel: { enabled: true, speed: 0.1 },
    pinch: { enabled: true },
    mode: "x",
  },
  limits: {
    x: { min: "original", max: "original" },
    y: undefined,
  },
};

const chartDefaults = {
  responsive: true,
  maintainAspectRatio: false,
  animation: false,
  interaction: { mode: "index", intersect: false },
  scales: {
    x: {
      type: "time",
      time: {
        tooltipFormat: "MMM d HH:mm:ss",
        displayFormats: {
          minute: "HH:mm",
          hour: "MMM d HH:mm",
          day: "MMM d",
        },
      },
      ticks: { color: "#8b9aab", maxRotation: 0, autoSkipPadding: 12 },
      grid: { color: "rgba(42,53,64,0.7)" },
    },
    y: {
      ticks: { color: "#8b9aab" },
      grid: { color: "rgba(42,53,64,0.7)" },
    },
  },
  plugins: {
    legend: { display: false },
    tooltip: {
      backgroundColor: "#12181e",
      borderColor: "#2a3540",
      borderWidth: 1,
      titleColor: "#e8eef3",
      bodyColor: "#e8eef3",
    },
    zoom: zoomOptions,
  },
};

function makeLineChart(canvasId, color, label, ySuggested) {
  const ctx = document.getElementById(canvasId);
  return new Chart(ctx, {
    type: "line",
    data: {
      datasets: [
        {
          label,
          data: [],
          borderColor: color,
          backgroundColor: color + "33",
          fill: true,
          tension: 0.4,
          cubicInterpolationMode: "monotone",
          pointRadius: 0,
          borderWidth: 2,
          spanGaps: true,
        },
      ],
    },
    options: {
      ...chartDefaults,
      scales: {
        ...chartDefaults.scales,
        y: {
          ...chartDefaults.scales.y,
          suggestedMin: ySuggested?.min,
          suggestedMax: ySuggested?.max,
        },
      },
    },
  });
}

const socChart = makeLineChart("socChart", "#3ecf8e", "Battery SoC %", {
  min: 0,
  max: 100,
});
const pvChart = makeLineChart("pvChart", "#e6b35a", "PV Power W", { min: 0 });
const outChart = new Chart(document.getElementById("outChart"), {
  type: "line",
  data: {
    datasets: [
      {
        label: "Output Power W",
        data: [],
        borderColor: "#5ec8d6",
        backgroundColor: "#5ec8d633",
        fill: true,
        tension: 0.4,
        cubicInterpolationMode: "monotone",
        pointRadius: 0,
        borderWidth: 2,
        spanGaps: true,
        yAxisID: "y",
      },
      {
        label: "Cumulative kWh",
        data: [],
        borderColor: "#3ecf8e",
        backgroundColor: "transparent",
        fill: false,
        tension: 0.25,
        cubicInterpolationMode: "monotone",
        pointRadius: 0,
        borderWidth: 1.5,
        borderDash: [5, 4],
        yAxisID: "y1",
      },
    ],
  },
  options: {
    ...chartDefaults,
    scales: {
      ...chartDefaults.scales,
      y: {
        ...chartDefaults.scales.y,
        position: "left",
        title: { display: true, text: "W", color: "#8b9aab" },
        suggestedMin: 0,
      },
      y1: {
        position: "right",
        ticks: { color: "#8b9aab" },
        grid: { drawOnChartArea: false },
        title: { display: true, text: "kWh", color: "#8b9aab" },
        suggestedMin: 0,
      },
    },
    plugins: {
      ...chartDefaults.plugins,
      legend: {
        display: true,
        labels: { color: "#8b9aab", boxWidth: 14 },
      },
    },
  },
});

function drawSolarRing(chart) {
  const rows = state.pieRows || [];
  const meta = chart.getDatasetMeta(0);
  if (!meta?.data?.length) return;
  let start = null;
  let end = null;
  let arc = null;
  rows.forEach((row, index) => {
    if (!row?.value || row.slice?.gap || !isSolarSource(row.slice?.source)) return;
    const piece = meta.data[index];
    if (!piece) return;
    if (start == null) {
      start = piece.startAngle;
      arc = piece;
    }
    end = piece.endAngle;
  });
  if (start == null || end == null || !arc || end - start < 0.02) return;
  const ctx = chart.ctx;
  ctx.save();
  ctx.beginPath();
  ctx.strokeStyle = "#3ecf8e";
  ctx.lineWidth = 5;
  ctx.lineCap = "butt";
  ctx.arc(arc.x, arc.y, arc.outerRadius + 8, start, end);
  ctx.stroke();
  ctx.restore();
}

const loadsPieChart = new Chart(document.getElementById("loadsPieChart"), {
  type: "doughnut",
  plugins: [
    {
      id: "pieGroupLabels",
      afterDatasetsDraw(chart) {
        drawSolarRing(chart);
      },
    },
  ],
  data: {
    labels: LOAD_SLICES.map((s) => s.label),
    datasets: [
      {
        data: LOAD_SLICES.map(() => 0),
        backgroundColor: LOAD_SLICES.map((s) => s.color),
        borderColor: "#12181e",
        borderWidth: 2,
        offset: 0,
      },
    ],
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    layout: { padding: 14 },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: "#12181e",
        borderColor: "#2a3540",
        borderWidth: 1,
        filter: (item) => !!item.label,
        callbacks: {
          label(ctx) {
            const v = Number(ctx.raw);
            const row = state.pieRows?.[ctx.dataIndex];
            if (row?.slice?.gap) return "";
            const extra = row?.slice?.members?.length
              ? ` · ${row.slice.members.length} merged`
              : "";
            const supply = isSolarSource(row?.slice?.source) ? "solar" : "grid";
            return `${ctx.label}: ${Number.isFinite(v) ? v.toFixed(3) : "—"} kWh · ${supply}${extra}`;
          },
        },
      },
    },
  },
});

const charts = [socChart, pvChart, outChart];

function fmt(value, digits = 1) {
  if (value == null || Number.isNaN(Number(value))) return "—";
  return Number(value).toFixed(digits);
}

function flash(el) {
  el.classList.remove("flash");
  void el.offsetWidth;
  el.classList.add("flash");
}

function formatWatts(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return "—";
  return `${n >= 100 ? n.toFixed(0) : n.toFixed(1)} W`;
}

function updateLoadsPie(loads) {
  if (loads) state.loadsDailyKwh = loads;
  const src = state.loadsDailyKwh || {};
  const rows = pieChartModel(currentLoadSlices(), src);
  state.pieRows = rows;
  const ds = loadsPieChart.data.datasets[0];
  loadsPieChart.data.labels = rows.map((r) => (r.slice.gap ? "" : r.slice.label));
  ds.backgroundColor = rows.map((r) => r.slice.color);
  ds.offset = rows.map(() => 0);
  ds.data = rows.map((r) => r.value);
  loadsPieChart.update("none");
  updateCurrentLoads();
}

function updateCurrentLoads(power) {
  if (power) state.loadsPowerW = power;
  if (!els.currentLoadsList) return;

  const src = state.loadsPowerW || {};
  const groups = [
    { title: "Solar supply", solar: true },
    { title: "Grid supply", solar: false },
  ]
    .map((group) => ({
      title: group.title,
      rows: currentLoadSlices()
        .filter((slice) => isSolarSource(slice.source) === group.solar)
        .map((slice) => ({
          label: slice.label,
          color: slice.color,
          watts: sumMapValues(src, sliceKeys(slice)),
        }))
        .filter((row) => row.watts > 0)
        .sort((a, b) => b.watts - a.watts),
    }))
    .filter((group) => group.rows.length);

  els.currentLoadsList.replaceChildren();
  if (!groups.length) {
    const empty = document.createElement("li");
    empty.className = "current-loads-empty";
    empty.textContent = "Nothing drawing power";
    els.currentLoadsList.appendChild(empty);
  } else {
    for (const group of groups) {
      const heading = document.createElement("li");
      heading.className = "current-load-group";
      heading.textContent = group.title;
      els.currentLoadsList.appendChild(heading);
      for (const row of group.rows) {
        const li = document.createElement("li");
        li.className = "current-load-row";

        const name = document.createElement("span");
        name.className = "current-load-name";
        const swatch = document.createElement("span");
        swatch.className = "current-load-swatch";
        swatch.style.background = row.color;
        const label = document.createElement("span");
        label.className = "current-load-label";
        label.textContent = row.label;
        name.append(swatch, label);

        const watts = document.createElement("span");
        watts.className = "current-load-watts";
        watts.textContent = formatWatts(row.watts);

        li.append(name, watts);
        els.currentLoadsList.appendChild(li);
      }
    }
  }

  if (els.currentLoadsTotal) {
    const total = groups.reduce(
      (sum, group) => sum + group.rows.reduce((inner, row) => inner + row.watts, 0),
      0,
    );
    els.currentLoadsTotal.textContent = groups.length ? formatWatts(total) : "—";
  }
}

function updateTiles(sample) {
  if (!sample) return;

  const pairs = [
    [els.batterySoc, fmt(sample.batterySoc, 0)],
    [els.batteryVoltage, fmt(sample.batteryVoltage, 1)],
    [els.chargeCurrent, fmt(sample.batteryChargeCurrent, 0)],
    [els.dischargeCurrent, fmt(sample.batteryDischargeCurrent, 0)],
    [els.pvVoltage, fmt(sample.pvVoltage, 1)],
    [els.pvPower, fmt(sample.pvPower, 0)],
    [els.outputPower, fmt(sample.outputPower, 0)],
    [els.loadPercent, fmt(sample.loadPercent, 1)],
  ];

  for (const [el, text] of pairs) {
    if (el.textContent !== text) {
      el.textContent = text;
      flash(el);
    }
  }

  const total = sample.energyKwhTotal ?? state.energyKwhTotal;
  els.energyTotal.textContent = fmt(total, 3);
  els.lastUpdate.textContent = sample.ts
    ? new Date(sample.ts).toLocaleString()
    : "—";

  if (sample.loadsDailyKwh) {
    updateLoadsPie(sample.loadsDailyKwh);
  }
  if (sample.loadsPowerW) {
    updateCurrentLoads(sample.loadsPowerW);
  }
}

function updateChrome() {
  const label = RANGE_LABELS[state.range] || state.range;
  els.footNote.textContent = `Window: ${label}`;
  paintChartPanels();
}

function syncCharts() {
  const battery = state.charts?.battery || {};
  const inverter = state.charts?.inverter || {};
  const soc = [];
  const pv = [];
  const out = [];
  const energy = [];
  const batteryLegacy = !!battery.legacy;
  const inverterLegacy = !!inverter.legacy;

  if (batteryLegacy) {
    for (const s of state.samples) soc.push({ x: s.ts, y: s.batterySoc });
  } else {
    soc.push(...(battery.points || []));
  }
  if (inverterLegacy) {
    for (const s of state.samples) {
      out.push({ x: s.ts, y: s.outputPower });
      energy.push({ x: s.ts, y: s.energyKwhCumulative });
    }
  } else {
    out.push(...(inverter.points || []));
  }
  for (const s of state.samples) {
    pv.push({ x: s.ts, y: s.pvPower });
  }

  const batteryPercent = batteryLegacy || battery.unit === "%";
  socChart.options.scales.y.suggestedMin = 0;
  socChart.options.scales.y.suggestedMax = batteryPercent ? 100 : undefined;
  socChart.data.datasets[0].label = batteryLegacy
    ? "Battery SoC %"
    : battery.label || battery.entityId;

  outChart.data.datasets[1].hidden = !inverterLegacy;
  if (outChart.options.scales.y1) outChart.options.scales.y1.display = inverterLegacy;
  outChart.options.scales.y.title = {
    display: true,
    text: inverterLegacy ? "W" : inverter.unit || "",
    color: "#8b9aab",
  };
  outChart.data.datasets[0].label = inverterLegacy
    ? "Output Power W"
    : inverter.label || inverter.entityId;

  socChart.data.datasets[0].data = smoothSeries(soc, 0.28);
  pvChart.data.datasets[0].data = smoothSeries(pv, 0.2);
  outChart.data.datasets[0].data = smoothSeries(out, 0.2);
  outChart.data.datasets[1].data = smoothSeries(energy, 0.45);

  for (const chart of charts) {
    chart.update("none");
  }
}

function resetAllZoom() {
  for (const chart of charts) {
    chart.resetZoom();
  }
}

function applyHistory(payload) {
  state.samples = payload.samples || [];
  state.energyKwhTotal = payload.energyKwhTotal || 0;
  state.rangeStartMs = Date.now() - rangeToMs(state.range);
  applyBoard(payload);
  syncCharts();
  resetAllZoom();
  updateTiles(
    payload.latest
      ? { ...payload.latest, energyKwhTotal: state.energyKwhTotal }
      : null,
  );
  applyLoadConfig(payload.loadConfig);
  updateLoadsPie(payload.loadsDailyKwh || payload.latest?.loadsDailyKwh || null);
  updateCurrentLoads(payload.loadsPowerW || payload.latest?.loadsPowerW || null);
  updateChrome();
}

function sampleInRange(sample) {
  const t = Date.parse(sample.ts);
  return Number.isFinite(t) && t >= state.rangeStartMs;
}

function applySample(sample) {
  if (!sample?.ts) return;

  if (sample.energyKwhTotal != null) {
    state.energyKwhTotal = sample.energyKwhTotal;
  }
  updateTiles(sample);

  if (!sampleInRange(sample)) return;

  const last = state.samples[state.samples.length - 1];
  if (last && last.ts === sample.ts) {
    state.samples[state.samples.length - 1] = sample;
  } else {
    state.samples.push(sample);
  }
  syncCharts();
}

async function loadHistory() {
  const res = await fetch(withSite(`/api/history?range=${encodeURIComponent(state.range)}`));
  if (res.status === 401) {
    location.href = "/login";
    return;
  }
  if (!res.ok) throw new Error(`history HTTP ${res.status}`);
  applyHistory(await res.json());
}

function setLive(live) {
  els.connection.textContent = live ? "Live" : "Reconnecting…";
  document.getElementById("connDot").classList.toggle("live", live);
}

function renderDevices(devices, { confirmPending = true } = {}) {
  if (Array.isArray(devices)) {
    if (confirmPending) confirmPendingFrom(devices);
    state.devices = devices.map(overlayPendingDevice);
  }
  paintDevices();
}

function paintDevices() {
  const sensors = state.devices.filter((d) => isSensorDomain(d.domain));
  const clickable = state.devices.filter((d) => !isSensorDomain(d.domain));
  renderSensors(sensors);
  renderClickableDevices(clickable);
}

function renderSensors(sensors) {
  if (!els.sensorSection || !els.sensorList) return;
  els.sensorList.replaceChildren();
  if (!sensors.length) {
    els.sensorSection.hidden = true;
    return;
  }
  els.sensorSection.hidden = false;
  for (const d of sensors) {
    const row = document.createElement("div");
    row.className = "sensor-row";
    row.dataset.entityId = d.entityId;

    const name = document.createElement("span");
    name.className = "sensor-name";
    name.textContent = d.name || d.entityId;

    const value = document.createElement("span");
    value.className = "sensor-state";
    const tone = stateTone(d);
    if (tone) value.classList.add(`is-${tone}`);
    value.textContent = formatHaState(d);

    row.append(name, value);
    els.sensorList.appendChild(row);
  }
}

function renderClickableDevices(devices) {
  if (!els.deviceGrid) return;
  els.deviceGrid.replaceChildren();

  if (!devices.length) {
    const empty = document.createElement("p");
    empty.className = "hint";
    empty.textContent = "No switches or lights available yet.";
    els.deviceGrid.appendChild(empty);
    return;
  }

  for (const d of devices) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "device-btn";
    btn.dataset.entityId = d.entityId;
    btn.textContent = d.name || d.entityId;
    const pending = state.pendingToggles.get(d.entityId);
    const on = pending
      ? pending.to === "on"
      : d.on === true || String(d.state || "").toLowerCase() === "on";
    const known = pending
      ? true
      : ["on", "off"].includes(String(d.state || "").toLowerCase());
    btn.classList.add(known ? (on ? "is-on" : "is-off") : "is-unknown");
    if (pending) btn.classList.add("is-pending");
    btn.setAttribute("aria-pressed", on ? "true" : "false");
    if (pending) {
      btn.disabled = true;
      btn.setAttribute("aria-busy", "true");
    }
    btn.addEventListener("click", () => toggleDevice(d.entityId));
    els.deviceGrid.appendChild(btn);
  }
}

async function loadDevices() {
  const res = await fetch(withSite("/api/devices"));
  if (!res.ok) {
    renderDevices([]);
    console.warn("devices API", res.status);
    return;
  }
  const data = await res.json();
  renderDevices(data.devices || []);
}

async function toggleDevice(entityId) {
  if (!entityId || state.pendingToggles.has(entityId)) return;
  const current = state.devices.find((d) => d.entityId === entityId);
  const from = switchState(current) === "on" ? "on" : "off";
  startPendingToggle(entityId, from);
  renderDevices(state.devices, { confirmPending: false });
  try {
    const res = await fetch(withSite(`/api/devices/${encodeURIComponent(entityId)}/toggle`), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
    });
    if (!res.ok) throw new Error(`toggle ${res.status}`);
    const data = await res.json();
    if (data.devices) renderDevices(data.devices, { confirmPending: false });
  } catch (err) {
    console.error(err);
    expirePendingToggle(entityId);
    await loadDevices().catch(() => {});
  }
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const qs = SITE && SITE !== "home" ? `?site=${encodeURIComponent(SITE)}` : "";
  const ws = new WebSocket(`${proto}://${location.host}/ws${qs}`);

  ws.addEventListener("open", () => setLive(true));
  ws.addEventListener("close", () => {
    setLive(false);
    setTimeout(connectWs, 3000);
  });
  ws.addEventListener("message", (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === "hello") {
        if (msg.energyKwhTotal != null) state.energyKwhTotal = msg.energyKwhTotal;
        if (msg.latest) {
          updateTiles({ ...msg.latest, energyKwhTotal: state.energyKwhTotal });
        }
        applyLoadConfig(msg.loadConfig);
        applyBoard(msg);
        if (msg.loadsDailyKwh) updateLoadsPie(msg.loadsDailyKwh);
        if (msg.loadsPowerW) updateCurrentLoads(msg.loadsPowerW);
        if (msg.devices) renderDevices(msg.devices);
      } else if (msg.type === "chartPoint") {
        appendChartPoint("battery", msg.battery);
        appendChartPoint("inverter", msg.inverter);
        syncCharts();
      } else if (msg.type === "board") {
        applyBoard(msg);
        syncCharts();
        updateLoadsPie();
      } else if (msg.type === "loadConfig") {
        applyLoadConfig(msg.loadConfig);
        updateLoadsPie();
        updateCurrentLoads();
      } else if (msg.type === "loadsPower") {
        updateCurrentLoads(msg.loadsPowerW);
      } else if (msg.type === "sample") {
        applySample(msg.sample);
      } else if (msg.type === "history") {
        applyHistory(msg);
      } else if (msg.type === "devices") {
        renderDevices(msg.devices);
        if (msg.tiles || msg.tilesMode || msg.showPie != null) applyBoard(msg);
      }
    } catch (err) {
      console.error("ws message error", err);
    }
  });
}

async function loadMe() {
  const res = await fetch(withSite("/api/me"));
  if (!res.ok) return;
  const me = await res.json();
  if (els.adminLink) {
    els.adminLink.href = SITE === "home" ? "/admin" : `/admin?site=${encodeURIComponent(SITE)}`;
    els.adminLink.hidden = !me.isAdmin;
  }
  const notice = document.getElementById("accountNotice");
  if (!notice || me.isAdmin || SITE === "home") return;
  notice.hidden = false;
  notice.textContent = "";
  notice.append(`Signed in as ${me.email}. `);
  if (me.isHomeAdmin) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "toolbar-btn";
    button.textContent = "Make this account the admin";
    button.addEventListener("click", async () => {
      const save = await fetch(`/api/admin/sites/${encodeURIComponent(SITE)}/admin`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ email: me.email }),
      });
      if (!save.ok) {
        button.textContent = "Could not save";
        return;
      }
      location.reload();
    });
    notice.append(button);
    return;
  }
  const link = document.createElement("a");
  link.href = "#";
  link.textContent = "Sign in with a different account";
  link.addEventListener("click", (event) => {
    event.preventDefault();
    const form = document.createElement("form");
    form.method = "post";
    form.action = `/logout?next=${encodeURIComponent(`/${SITE}`)}`;
    document.body.append(form);
    form.submit();
  });
  notice.append(link);
}

els.rangeSelect.addEventListener("change", () => {
  state.range = els.rangeSelect.value;
  loadHistory().catch((err) => console.error(err));
});

els.resetZoom.addEventListener("click", resetAllZoom);

paintTiles();
paintPie();
updateChrome();
loadMe().catch(() => {});
loadDevices().catch(() => {});
loadHistory()
  .catch((err) => console.error(err))
  .finally(connectWs);

setInterval(() => {
  if (els.connection.textContent !== "Live") {
    loadHistory().catch(() => {});
  }
}, 15000);
