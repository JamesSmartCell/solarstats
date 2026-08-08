# solarstats

Receives PowMr snapshots from `solarstatsapi`, stores history in SQLite, integrates inverter output into **kWh**, and serves a live dashboard at **`/solarstats`**.

## Setup (remote server)

```bash
cd solarstats
cp .env.example .env
# edit .env — set INGEST_SECRET to the same value as solarstatsapi
npm install
npm start
```

Open: `http://<server>:8787/solarstats`

> `better-sqlite3` needs build tools on the server (`build-essential` / Python on Linux).

## Environment

| Variable | Default | Notes |
|----------|---------|--------|
| `PORT` | `8787` | HTTP + WebSocket listen port |
| `INGEST_SECRET` | _(empty = open)_ | Bearer token required on `/api/ingest` |
| `DB_PATH` | `./data/solarstats.db` | SQLite file |
| `HISTORY_RETENTION_DAYS` | `30` | Old samples pruned periodically |

## API

- `POST /api/ingest` — accept snapshot from Pi (`Authorization: Bearer <INGEST_SECRET>`)
- `GET /api/history?range=24h` — chart series + totals (`24h`, `7d`, …)
- `GET /solarstats` — dashboard
- `WS /ws` — live sample push

## Energy

On each ingest, cumulative kWh is updated with trapezoidal integration of `outputPower` over the time delta (gaps &gt; 5 minutes are skipped).
