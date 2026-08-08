const RANGE = "24h";
const MAX_POINTS = 6000;

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
};

const state = {
  samples: [],
  energyKwhTotal: 0,
};

const chartDefaults = {
  responsive: true,
  maintainAspectRatio: false,
  animation: false,
  interaction: { mode: "index", intersect: false },
  scales: {
    x: {
      type: "time",
      time: { tooltipFormat: "HH:mm:ss", displayFormats: { minute: "HH:mm", hour: "HH:mm" } },
      ticks: { color: "#8b9aab", maxRotation: 0 },
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
          tension: 0.2,
          pointRadius: 0,
          borderWidth: 2,
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
        tension: 0.2,
        pointRadius: 0,
        borderWidth: 2,
        yAxisID: "y",
      },
      {
        label: "Cumulative kWh",
        data: [],
        borderColor: "#3ecf8e",
        backgroundColor: "transparent",
        fill: false,
        tension: 0.15,
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

function fmt(value, digits = 1) {
  if (value == null || Number.isNaN(Number(value))) return "—";
  return Number(value).toFixed(digits);
}

function flash(el) {
  el.classList.remove("flash");
  void el.offsetWidth;
  el.classList.add("flash");
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
}

function trimSamples() {
  if (state.samples.length > MAX_POINTS) {
    state.samples = state.samples.slice(-MAX_POINTS);
  }
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

  socChart.data.datasets[0].data = soc;
  pvChart.data.datasets[0].data = pv;
  outChart.data.datasets[0].data = out;
  outChart.data.datasets[1].data = energy;
  socChart.update("none");
  pvChart.update("none");
  outChart.update("none");
}

function applyHistory(payload) {
  state.samples = payload.samples || [];
  state.energyKwhTotal = payload.energyKwhTotal || 0;
  trimSamples();
  syncCharts();
  updateTiles(
    payload.latest
      ? { ...payload.latest, energyKwhTotal: state.energyKwhTotal }
      : null,
  );
}

function applySample(sample) {
  if (!sample?.ts) return;
  const last = state.samples[state.samples.length - 1];
  if (last && last.ts === sample.ts) {
    state.samples[state.samples.length - 1] = sample;
  } else {
    state.samples.push(sample);
  }
  if (sample.energyKwhTotal != null) {
    state.energyKwhTotal = sample.energyKwhTotal;
  }
  trimSamples();
  syncCharts();
  updateTiles(sample);
}

async function loadHistory() {
  const res = await fetch(`/api/history?range=${RANGE}`);
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
      if (msg.type === "history") {
        applyHistory(msg);
      } else if (msg.type === "sample") {
        applySample(msg.sample);
      }
    } catch (err) {
      console.error("ws message error", err);
    }
  });
}

loadHistory()
  .catch((err) => console.error(err))
  .finally(connectWs);

// Fallback refresh aligned with HA/API interval if WS drops
setInterval(() => {
  if (els.connection.textContent !== "Live") {
    loadHistory().catch(() => {});
  }
}, 15000);
