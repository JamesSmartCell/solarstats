# solarstats

Receives PowMr snapshots from `solarstatsapi`, stores history in SQLite, integrates inverter output into **kWh**, and serves a live dashboard at **`/`**.

## Setup (remote server)

```bash
cd solarstats
cp .env.example .env
# edit .env — set INGEST_SECRET to the same value as solarstatsapi
npm install
npm start
```

Local check: `http://127.0.0.1:8787/`  
Public (via Caddy): `https://solar.example.com/`

> `better-sqlite3` needs build tools on the server (`build-essential` / Python on Linux).

### systemd

Edit paths/user in [`solarstats.service`](solarstats.service), then:

```bash
sudo cp solarstats.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now solarstats
sudo systemctl status solarstats
```

## Caddy (DNS → TLS → Node on 8787)

Proxy the whole host to Node:

```caddy
solar.example.com {
	tls /etc/letsencrypt/live/solar.example.com/fullchain.pem /etc/letsencrypt/live/solar.example.com/privkey.pem

	reverse_proxy 127.0.0.1:8787
}
```

That covers:

- `GET /` (dashboard; `/solarstats` redirects here)
- `GET /solarstats.css`, `/solarstats.js`, …
- `POST /api/ingest`, `GET /api/history`, `GET /api/health`
- `WS /ws` (live updates)

### Port 8787 vs SSH tunnel — not a conflict

| Where | What listens on 8787 |
|-------|----------------------|
| **EC2** | `solarstats` (Node) on `127.0.0.1:8787` |
| **Pi** | SSH `-L 8787:127.0.0.1:8787` — Pi’s localhost:8787 forwards **to** EC2’s 8787 |

Caddy on EC2 also talks to the **same** Node process (`127.0.0.1:8787`).  
Public clients use **443** only; leave **8787 closed** in the security group.

Pi `.env`: `SITE_INGEST_URL=http://127.0.0.1:8787/api/ingest` (through the tunnel).

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
- `GET /` — dashboard (`/solarstats` → `/`)
- `WS /ws` — live sample push

## Energy

On each ingest, cumulative kWh is updated with trapezoidal integration of `outputPower` over the time delta (gaps &gt; 5 minutes are skipped).
