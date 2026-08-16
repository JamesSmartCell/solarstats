# solarstatsapi

Polls Home Assistant PowMr sensors (and daily load kWh meters) every **15s** and POSTs a JSON snapshot to the remote `solarstats` ingest endpoint.

## Setup (Raspberry Pi)

```bash
cd solarstatsapi
cp .env.example .env
# edit .env — set HA_TOKEN, SITE_INGEST_URL, INGEST_SECRET
npm install
npm start
```

### systemd

Edit paths/user in [`solarstatsapi.service`](solarstatsapi.service), then:

```bash
sudo cp solarstatsapi.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now solarstatsapi
sudo systemctl status solarstatsapi
```

## Environment

| Variable | Example | Notes |
|----------|---------|--------|
| `HA_BASE_URL` | `http://192.168.50.41` | Home Assistant base URL (no trailing slash) |
| `HA_TOKEN` | long-lived token | HA profile → Long-Lived Access Tokens |
| `SITE_INGEST_URL` | `http://127.0.0.1:8787/api/ingest` | Remote site ingest (often via SSH tunnel) |
| `POLL_INTERVAL_MS` | `15000` | Match ESPHome / HA update interval |
| `INGEST_SECRET` | shared secret | Must match `solarstats` `INGEST_SECRET` |

## SSH tunnel example (push to server)

This app **POSTs** to the server. Forward Pi `localhost:8787` to the server’s solarstats port:

```bash
ssh -N -L 8787:127.0.0.1:8787 user@your-server
```

Then keep `SITE_INGEST_URL=http://127.0.0.1:8787/api/ingest` in `.env`.

## Payload

Each tick POSTs JSON like:

```json
{
  "ts": "2026-08-08T01:22:30.000Z",
  "batterySoc": 85,
  "pvPower": 148,
  "outputPower": 36,
  "batteryVoltage": 26.1,
  "loadsDailyKwh": {
    "officePc": 0.42,
    "frontRoomPc": 0.31,
    "pi5": 0.18,
    "motorbike": 0,
    "fridge": 0.55,
    "washingMachine": 0.12,
    "otherInverter": 0.9
  }
}
```

Daily load entities (edit IDs in [`src/index.js`](src/index.js) if HA renames them):

| Key | Entity |
|-----|--------|
| `officePc` | `sensor.office_pc_synth_energy_daily` |
| `frontRoomPc` | `sensor.front_room_pc_synth_energy_daily` |
| `pi5` | `sensor.pi5_server_energy_daily_2` |
| `motorbike` | `sensor.motorbike_charger_energy_daily_2` |
| `fridge` | `sensor.fridge_energy_daily_2` |
| `washingMachine` | `sensor.inverter_loads_energy_daily` |
| `otherInverter` | `sensor.inverter_unmetered_energy_daily` |

Missing sensors are sent as `null` (logged as warnings) so inverter ingest still succeeds.
