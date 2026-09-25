# solarstats

Receives snapshots from **`solarstatsapi`** (home) and **`solarshim`** (other sites), stores history in SQLite, integrates inverter output into **kWh**, and serves dashboards at **`/`** (home) and **`/:slug`** (e.g. `/rivermill`).

The dashboard is **private**: Sign in with Microsoft (Authenticator number matching), admin-approved accounts, optional site passkeys, plus an **All devices today (kWh)** doughnut.

## Setup (remote server)

```bash
cd solarstats
cp .env.example .env
# edit .env — INGEST_SECRET, Azure app, SESSION_SECRET, ORIGIN/RP_ID
npm install
npm start
```

Local check: `http://127.0.0.1:8787/` (set `COOKIE_SECURE` unset / not `1` for HTTP)  
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

## Sign-in providers (Microsoft / Google / Apple)

Configure at least one IdP in `.env` (see `.env.example`).

**Microsoft:** Entra app → redirect `https://your-host/auth/callback` → `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET` / `AZURE_TENANT=consumers`.

**Google:** [Cloud Console credentials](https://console.cloud.google.com/apis/credentials) → OAuth Web client → redirect `https://your-host/auth/callback/google` → `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET`.

**Apple (optional):** Developer Services ID + `.p8` key → redirect `https://your-host/auth/callback/apple`.

**Passkeys** are stored by the **device** (Apple Passwords / Google Password Manager / Windows Hello), not Microsoft Authenticator. iPhone: Settings → General → Autofill & Passwords → enable **Passwords**.

Admin email is set via **`ADMIN_EMAIL`** in `.env` (seeded as approved admin on first DB open).

### Account management (`/admin`)

| Control | Behavior |
|---------|----------|
| **Allow new accounts** | On: unknown emails become `pending`. Off: rejected immediately. |
| **Allow passkey enrollment** | On: approved users can **Add passkey** on the dashboard. Off: registration API returns 403. Existing passkeys still work for login. |
| **Approve / Deny / Revoke** | Controls who can open `/` and `/api/history` / `/ws`. |

## Multiple sites

One solarstats process. Home keeps `INGEST_SECRET` + `data/solarstats.db`. Extra slugs get their own secret and `data/sites/<slug>.db`.

```env
SITES=rivermill
SITE_RIVERMILL_SECRET=long-random
SITE_RIVERMILL_NAME=Rivermill
```

| URL | Role |
|-----|------|
| `/` | Home dashboard (existing) |
| `/rivermill` | Rivermill dashboard |
| `POST /api/ingest` | Home agent (`Authorization: Bearer INGEST_SECRET`) |
| `POST /api/ingest/rivermill` | solarshim (`Bearer SITE_RIVERMILL_SECRET`) |

Login is still the existing solarstats accounts. Site data is isolated.

On the Rivermill LAN run [`../solarshim`](../solarshim) with `SITE_INGEST_URL=https://homesolar.percolate.one/api/ingest/rivermill`.

### Pair a home from Home Assistant

Copy [`../custom_components/homesolar`](../custom_components/homesolar) into the HA `custom_components` folder and restart. Add the **Home Solar** integration, enter a display name and admin email, then type the 5-character code at `/connect`. The code uses `A–Z` and `2–9` and skips `0`, `O`, `1`, and `I`. It expires in 10 minutes.

That creates `data/sites/<slug>.db`, approves the email, and returns an ingest secret to Home Assistant once. The integration then posts snapshots to `POST /api/ingest/<slug>` the same way `solarshim` does. Sign in as that email and open `/<slug>`.

`POST /api/pair/start` and `GET /api/pair/poll` are public and rate-limited. The secret is not shown in the browser.

## Caddy (DNS → TLS → Node on 8787)

```caddy
solar.example.com {
	tls /etc/letsencrypt/live/solar.example.com/fullchain.pem /etc/letsencrypt/live/solar.example.com/privkey.pem

	reverse_proxy 127.0.0.1:8787
}
```

TLS is required for secure cookies (`COOKIE_SECURE=1`) and WebAuthn in production.

### Port 8787 vs SSH tunnel — not a conflict

| Where | What listens on 8787 |
|-------|----------------------|
| **EC2** | `solarstats` (Node) on `127.0.0.1:8787` |
| **Pi** | SSH `-L 8787:127.0.0.1:8787` — Pi’s localhost:8787 forwards **to** EC2’s 8787 |

Public clients use **443** only; leave **8787 closed** in the security group.

Pi `.env`: `SITE_INGEST_URL=http://127.0.0.1:8787/api/ingest` (through the tunnel).

## Environment

| Variable | Default | Notes |
|----------|---------|--------|
| `PORT` | `8787` | HTTP + WebSocket listen port |
| `INGEST_SECRET` | _(empty = open)_ | Bearer token required on `/api/ingest` (home) |
| `SITES` | _(empty)_ | Extra slugs, e.g. `rivermill` |
| `SITE_<SLUG>_SECRET` | — | Ingest bearer for that slug |
| `SITE_<SLUG>_NAME` | slug | Dashboard title |
| `DB_PATH` | `./data/solarstats.db` | Home SQLite file |
| `HISTORY_RETENTION_DAYS` | `30` | Old samples pruned periodically |
| `AZURE_CLIENT_ID` | — | Entra application ID |
| `AZURE_CLIENT_SECRET` | — | Client secret |
| `AZURE_TENANT` | `consumers` | `consumers` or `common` |
| `AUTH_REDIRECT_URI` | — | Exact callback URL registered in Entra |
| `SESSION_SECRET` | — | Cookie signing key |
| `ORIGIN` | — | Public origin, e.g. `https://solar.example.com` |
| `RP_ID` | hostname of `ORIGIN` | WebAuthn RP ID (apex host, no path) |
| `COOKIE_SECURE` | off unless `production` | Set `1` behind HTTPS |
| `ZBGW_DIAG_TOKEN` | `zbgw-anon-v1` | Header `X-ZBGW-Diag` from opted-in C6 gateways |
| `ZBGW_DIAG_DB_PATH` | `./data/zbgw_diag.db` | Gateway health / restart queue |

## API

- `POST /api/ingest` — home snapshot (`Authorization: Bearer <INGEST_SECRET>`)
- `POST /api/ingest/:slug` — site snapshot (`Bearer` = `SITE_<SLUG>_SECRET`)
- `GET /api/history?range=24h` — chart series + totals (**session required**)
- `GET /api/devices` — switches/lights visible to the viewer (ACL-filtered)
- `POST /api/devices/:entityId/toggle` — queue a HA toggle (Pi agent executes)
- `GET /api/agent/commands` — Pi claims pending toggles; returns `track` entity IDs
- `POST /api/agent/commands/:id/complete` — Pi reports command result
- `GET /` — dashboard (**approved session**)
- `WS /ws` — live sample + `devices` push (**approved session**)
- `POST /api/diag/zbgw` — Zigbee gateway diagnostics (`X-ZBGW-Diag` product token)
- `GET /fw/zigbee-gateway.bin` — public OTA image (admin-uploaded)
- `GET /api/admin/zbgw` — gateway list + events (**admin**)
- `POST /api/admin/zbgw/:deviceId/restart` — queue remote restart (**admin**)
- `GET /api/admin/fw` / `PUT /api/admin/fw/zigbee-gateway` — OTA image metadata / upload (**admin**)
- `GET /api/health` — public liveness
- `GET /login`, `/auth/microsoft`, `/auth/callback`, `POST /logout`
- Passkey + `/admin` routes as above

## Switches & lights

Dashboard buttons at the bottom show live on/off (yellow fill = on, outline = off). Toggle clicks enqueue a command on solarstats; **solarstatsapi** on the Pi claims and calls Home Assistant `switch`/`light` `toggle`, then the next ingest refreshes state.

Seeded entities ship with useful defaults; **Admin → Switches & lights** lists every HA `switch`/`light` the Pi discovers. Check **User** (everyone on the board) or **Admin** (admin only); leave both unchecked to hide from the board.

## HA field catalog

Ingest no longer depends on a single hard-coded inverter entity id. Each snapshot upserts every reported HA sensor into `ha_catalog` (state + last seen + HA `last_updated`). Logical fields (`pvPower`, `batterySoc`, …) bind to an entity and auto-heal when a rename leaves exactly one strong match. Found values still write; missing ones stay empty and show as stale/missing.

Admin → **HA inverter fields** lists bindings, age, and candidates. Pick a candidate to pin a manual bind.

## Zigbee gateway diagnostics

Opted-in ESP32-C6 gateways POST to `https://homesolar.percolate.one/api/diag/zbgw` (header `X-ZBGW-Diag`). Failures are stored immediately; a clean unit sends **Device working correctly** about once an hour. Polls every two minutes only refresh last-seen and pick up a remote **Restart**. Admin → **Zigbee gateways** shows the list and accepts a `zigbee-gateway.bin` upload, served at `https://homesolar.percolate.one/fw/zigbee-gateway.bin`. Nothing in the payload is a Wi-Fi or MQTT password — the device id is the STA MAC.

## Energy

On each ingest, cumulative kWh is updated with trapezoidal integration of `outputPower` over the time delta (gaps &gt; 5 minutes are skipped).

Unavailable / `unknown` HA states are **not** stored as zero. The server keeps the last live inverter snapshot (pack voltage ≥ 8 V), skips energy integration for that tick, and holds daily-load kWh unless every meter resets together (midnight). Restarting `solarstats` (or `npm run repair`) deletes zeroed samples so the dashboard comes back.

Daily device kWh from HA (`loadsDailyKwh`) drives the **All devices today** doughnut; those meters reset at midnight in Home Assistant. Admin marks each load as inverter or grid so the pie groups them with a thick black gap between the two.
