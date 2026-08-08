# Home Assistant — PowMr inverter monitoring

ESPHome firmware for a **PowMr** hybrid inverter, plus a small Node.js pipeline that publishes live solar stats to a web dashboard.

```text
PowMr inverter ──RS232/TTL──► ESP32 (ESPHome) ──Wi‑Fi──► Home Assistant
                                                          │
                                              solarstatsapi (Pi)
                                                          │ POST /api/ingest
                                                          ▼
                                              solarstats (/solarstats)
```

| Path | Role |
|------|------|
| [`powMr.yaml`](powMr.yaml) | ESPHome config — Modbus sensors on the inverter |
| [`solarstatsapi/`](solarstatsapi/) | Polls HA every 15s, forwards JSON snapshots |
| [`solarstats/`](solarstats/) | Ingest + SQLite history + live dashboard |

Register decoding is based on [leodesigner/powmr_comm](https://github.com/leodesigner/powmr_comm).

---

## Hardware

- PowMr inverter with RS232 / Modbus RTU (slave address **0x05**, **2400** baud)
- Level shifter (e.g. SP3232) between inverter RS232 and ESP32 TTL UART
- ESP32 (`esp32dev` in the YAML — change board if needed)
- Home Assistant (this setup uses HA OS on a Raspberry Pi)

Default UART pins in [`powMr.yaml`](powMr.yaml):

| Signal | ESP32 pin |
|--------|-----------|
| TX → inverter (via SP3232 RXD) | `GPIO17` |
| RX ← inverter (via SP3232 TXD) | `GPIO16` |

---

## `powMr.yaml` (ESPHome)

### What it does

- Connects to Wi‑Fi and exposes the native ESPHome **API** (Home Assistant discovers the device; MQTT is not used)
- Speaks Modbus RTU over UART to the inverter
- Polls holding registers every **15s** (`update_interval`)
- Applies a **16-bit byte swap** then scaling so values match the working `powmr_comm` firmware

PowMr register words are little-endian on the wire. ESPHome’s `U_WORD` reads them as big-endian, so each sensor does:

```yaml
filters:
  - lambda: |-
      uint16_t raw = (uint16_t) x;
      return (float) (uint16_t) ((raw >> 8) | (raw << 8));
  # then multiply where needed
```

The cast back to `uint16_t` is required — without it, the left shift widens and produces huge bogus values.

### Register map

Holding registers start at **4501** (function 3). Mapping used here:

| Address | Sensor | Scale | Notes |
|---------|--------|-------|--------|
| 4502 | Grid Voltage | ×0.1 | AC input; noise if no grid connected |
| 4503 | AC Frequency | ×0.1 | Hz |
| 4504 | PV Voltage | ×0.1 | V |
| 4505 | PV Power | raw | W |
| 4506 | Battery Voltage | ×0.1 | V |
| 4507 | Battery SoC | raw | % |
| 4508 | Battery Charge Current | raw | whole amps only |
| 4509 | Battery Discharge Current | raw | A |
| 4512 | Output Power | raw | W (inverter AC output) |
| 4513 | Load Percent | ×0.05 | `/20` in reference firmware |

Modbus controller settings: address `0x05`, `command_throttle: 1s`, `send_wait_time: 250ms`.

### Flash with ESPHome

1. Copy [`powMr.yaml`](powMr.yaml) into the ESPHome dashboard (or this repo path if you compile from CLI).
2. **Replace Wi‑Fi `ssid` / `password`** with your network (do not commit real credentials).
3. Confirm `board`, UART pins, and `update_interval` if your wiring differs.
4. Install **wirelessly** (OTA) or via USB.
5. In Home Assistant: **Settings → Devices & services** — adopt the ESPHome device if prompted.

After a bad firmware reading, clear skewed graphs via **Developer tools → Statistics → Delete selected statistics** for the PowMr entities (that removes history data only, not the sensors).

### Home Assistant entity IDs

Entity IDs can vary with device/area naming. Examples from a live install:

- `sensor.powmr_inverter_battery_voltage`
- `sensor.powmr_inverter_battery_soc`
- `sensor.garden_powmr_inverter_pv_power`
- `sensor.garden_powmr_inverter_output_power`

Confirm exact IDs under **Developer tools → States** (search `powmr`). Update [`solarstatsapi/src/index.js`](solarstatsapi/src/index.js) if yours differ.

---

## Solar stats pipeline

### 1. Remote dashboard — `solarstats`

On the server that will host the site:

```bash
cd solarstats
cp .env.example .env
# set INGEST_SECRET (see below)
npm install
npm start
```

Open: `http://<server>:8787/solarstats`

Needs Node **18+**. `better-sqlite3` requires build tools on Linux (`build-essential`, Python).

### 2. Shared ingest secret

```bash
openssl rand -hex 32
```

Put the **same** value in:

- `solarstats/.env` → `INGEST_SECRET=...`
- `solarstatsapi/.env` → `INGEST_SECRET=...`

### 3. Poller — `solarstatsapi` (Pi / LAN host)

```bash
cd solarstatsapi
cp .env.example .env
# HA_BASE_URL, HA_TOKEN, SITE_INGEST_URL, INGEST_SECRET
npm install
npm start
```

Create a **Long-Lived Access Token** in Home Assistant (profile → Long-Lived Access Tokens) and set `HA_TOKEN`.

| Variable | Example |
|----------|---------|
| `HA_BASE_URL` | `http://192.168.50.41` (no trailing slash; use your HA host/port) |
| `HA_TOKEN` | HA long-lived token |
| `SITE_INGEST_URL` | `http://127.0.0.1:8787/api/ingest` |
| `POLL_INTERVAL_MS` | `15000` (match ESPHome) |
| `INGEST_SECRET` | same as solarstats |

### 4. Reach the server from the Pi

If the server is not directly reachable, forward local port 8787 to the server (push model):

```bash
ssh -N -L 8787:127.0.0.1:8787 user@your-server
```

Or set `SITE_INGEST_URL` to the server’s public/LAN URL and skip the tunnel.

### Dashboard features

- Live tiles: SoC, battery V, charge/discharge A, PV V/W, output W, load %, total kWh
- Charts (24h): battery SoC, PV power, inverter output + cumulative kWh
- WebSocket updates on each ingest (~15s)
- Server-side trapezoidal integration of `outputPower` → kWh (gaps &gt; 5 minutes skipped)

More detail: [`solarstatsapi/README.md`](solarstatsapi/README.md), [`solarstats/README.md`](solarstats/README.md).

---

## Security notes

- **Never commit** `.env` files or real Wi‑Fi passwords. Root [`.gitignore`](.gitignore) ignores `**/.env`.
- Rotate any credentials that were previously committed in YAML or chat logs.
- Keep `INGEST_SECRET` set in production so `/api/ingest` is not open.

---

## License / credits

- Modbus layout reference: [leodesigner/powmr_comm](https://github.com/leodesigner/powmr_comm)
- Related ecosystem notes also appear in that project (SmartESS / DESS monitor tooling)
