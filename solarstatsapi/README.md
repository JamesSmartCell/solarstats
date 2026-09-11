# solarstatsapi

Polls Home Assistant every **15s** (`GET /api/states`) and POSTs every switch, light, sensor, and binary sensor as-is (id, name, state, unit, class, `last_updated`) to `solarstats`. Binding and healing happen on the site admin page.

## Setup (Raspberry Pi)

```bash
cd solarstatsapi
cp .env.example .env
# edit .env — set HA_TOKEN, SITE_INGEST_URL, INGEST_SECRET
npm install   # better-sqlite3 needs build-essential / Python on Linux
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
| `CATALOG_PATH` | `./data/catalog.db` | SQLite catalog of HA entities + field bindings |

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
  "devices": [
    {
      "entity_id": "sensor.garden_powmr_inverter_pv_power",
      "state": "197.0",
      "name": "PV Power",
      "device_class": "power",
      "unit": "W",
      "last_updated": "2026-09-10T22:49:40.853256+00:00"
    }
  ]
}
```

`solarstats` stores the dump, binds inverter fields, and builds the load pie. Admin → **HA inverter fields** heals a rename; Admin → **Devices** decides which switches/lights appear on the board.

The agent keeps a local SQLite cache (`CATALOG_PATH`, default `./data/catalog.db`) so a failed HA dump can still forward the last seen names and values. Field bindings and healing live on the solarstats site. The ingest server keeps the last live inverter reading instead of writing zeros.
