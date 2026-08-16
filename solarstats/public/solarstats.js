import {
  startRegistration,
} from "https://unpkg.com/@simplewebauthn/browser@13.1.0/esm/index.js";

const RANGE_LABELS = {
  "1h": "1 hour",
  "6h": "6 hours",
  "12h": "12 hours",
  "24h": "24 hours",
  "3d": "3 days",
  "7d": "7 days",
  "30d": "30 days",
};

const LOAD_SLICES = [
  { key: "officePc", label: "Office PC", color: "#42a5f5" },
  { key: "frontRoomPc", label: "Front Room PC", color: "#5c6bc0" },
  { key: "pi5", label: "Pi5 Server", color: "#7e57c2" },
  { key: "motorbike", label: "Motorbike", color: "#26a69a" },
  { key: "fridge", label: "Fridge", color: "#66bb6a" },
  { key: "washingMachine", label: "Washing machine", color: "#8bc34a" },
  { key: "otherInverter", label: "Other inverter", color: "#cddc39" },
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
  adminLink: document.getElementById("adminLink"),
  passkeyRegisterBtn: document.getElementById("passkeyRegisterBtn"),
};

const state = {
  range: els.rangeSelect.value || "24h",
  samples: [],
  energyKwhTotal: 0,
  rangeStartMs: 0,
  loadsDailyKwh: null,
};

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

const loadsPieChart = new Chart(document.getElementById("loadsPieChart"), {
  type: "doughnut",
  data: {
    labels: LOAD_SLICES.map((s) => s.label),
    datasets: [
      {
        data: LOAD_SLICES.map(() => 0),
        backgroundColor: LOAD_SLICES.map((s) => s.color),
        borderColor: "#12181e",
        borderWidth: 2,
      },
    ],
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    plugins: {
      legend: {
        position: "bottom",
        labels: { color: "#8b9aab", boxWidth: 12, font: { size: 11 } },
      },
      tooltip: {
        backgroundColor: "#12181e",
        borderColor: "#2a3540",
        borderWidth: 1,
        callbacks: {
          label(ctx) {
            const v = Number(ctx.raw);
            return `${ctx.label}: ${Number.isFinite(v) ? v.toFixed(3) : "—"} kWh`;
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

function updateLoadsPie(loads) {
  if (loads) state.loadsDailyKwh = loads;
  const src = state.loadsDailyKwh || {};
  loadsPieChart.data.datasets[0].data = LOAD_SLICES.map((s) => {
    const n = Number(src[s.key]);
    return Number.isFinite(n) && n > 0 ? n : 0;
  });
  loadsPieChart.update("none");
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
}

function updateChrome() {
  const label = RANGE_LABELS[state.range] || state.range;
  els.socHint.textContent = `State of charge · ${label}`;
  els.footNote.textContent = `Window: ${label} · pinch/wheel zoom · smoothed series · trapezoid kWh`;
}

function syncCharts() {
  const soc = [];
  const pv = [];
  const out = [];
  const energy = [];

  for (const s of state.samples) {
    const x = s.ts;
    soc.push({ x, y: s.batterySoc });
    pv.push({ x, y: s.pvPower });
    out.push({ x, y: s.outputPower });
    energy.push({ x, y: s.energyKwhCumulative });
  }

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
  syncCharts();
  resetAllZoom();
  updateTiles(
    payload.latest
      ? { ...payload.latest, energyKwhTotal: state.energyKwhTotal }
      : null,
  );
  updateLoadsPie(payload.loadsDailyKwh || payload.latest?.loadsDailyKwh || null);
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
  const res = await fetch(`/api/history?range=${encodeURIComponent(state.range)}`);
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

function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws`);

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
        if (msg.loadsDailyKwh) updateLoadsPie(msg.loadsDailyKwh);
      } else if (msg.type === "sample") {
        applySample(msg.sample);
      } else if (msg.type === "history") {
        applyHistory(msg);
      }
    } catch (err) {
      console.error("ws message error", err);
    }
  });
}

async function loadMe() {
  const res = await fetch("/api/me");
  if (!res.ok) return;
  const me = await res.json();
  if (me.role === "admin") {
    els.adminLink.hidden = false;
  }
  if (me.settings?.allowPasskeyEnrollment && window.PublicKeyCredential) {
    els.passkeyRegisterBtn.hidden = false;
  }
}

els.passkeyRegisterBtn?.addEventListener("click", async () => {
  els.passkeyRegisterBtn.disabled = true;
  try {
    const optRes = await fetch("/auth/passkey/register/options", { method: "POST" });
    if (!optRes.ok) {
      throw new Error((await optRes.json().catch(() => ({}))).error || "options failed");
    }
    const options = await optRes.json();
    const credential = await startRegistration({ optionsJSON: options });
    const verifyRes = await fetch("/auth/passkey/register/verify", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(credential),
    });
    if (!verifyRes.ok) {
      throw new Error((await verifyRes.json().catch(() => ({}))).error || "verify failed");
    }
    alert("Passkey registered. You can use it on the sign-in page next time.");
  } catch (err) {
    alert(err.message || "Passkey registration failed");
  } finally {
    els.passkeyRegisterBtn.disabled = false;
  }
});

els.rangeSelect.addEventListener("change", () => {
  state.range = els.rangeSelect.value;
  loadHistory().catch((err) => console.error(err));
});

els.resetZoom.addEventListener("click", resetAllZoom);

updateChrome();
loadMe().catch(() => {});
loadHistory()
  .catch((err) => console.error(err))
  .finally(connectWs);

setInterval(() => {
  if (els.connection.textContent !== "Live") {
    loadHistory().catch(() => {});
  }
}, 15000);
