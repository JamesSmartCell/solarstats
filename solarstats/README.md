# solarstats

Receives PowMr snapshots from `solarstatsapi`, stores history in SQLite, integrates inverter output into **kWh**, and serves a live dashboard at **`/`**.

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

## Microsoft Entra app registration

1. Open [Entra app registrations](https://entra.microsoft.com/#view/Microsoft_AAD_RegisteredApps) → **New registration**.
2. Supported account types: **Personal Microsoft accounts only** (or personal + work if you prefer).
3. Redirect URI (Web): `https://your-host/auth/callback` — must match `AUTH_REDIRECT_URI`.
4. Certificates & secrets → create a **client secret**.
5. Copy **Application (client) ID** → `AZURE_CLIENT_ID`; secret → `AZURE_CLIENT_SECRET`.
6. Set `AZURE_TENANT=consumers` for personal accounts (use `common` if you also allow work/school).

Admin account **`manticorenettle@gmail.com`** is seeded as approved admin on first DB open. Sign in with that Microsoft identity first.

Enable **Microsoft Authenticator MFA** on each user’s Microsoft account so number matching appears at sign-in ([account security](https://account.microsoft.com/security)).

### Account management (`/admin`)

| Control | Behavior |
|---------|----------|
| **Allow new accounts** | On: unknown emails become `pending`. Off: rejected immediately. |
| **Allow passkey enrollment** | On: approved users can **Add passkey** on the dashboard. Off: registration API returns 403. Existing passkeys still work for login. |
| **Approve / Deny / Revoke** | Controls who can open `/` and `/api/history` / `/ws`. |

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
| `INGEST_SECRET` | _(empty = open)_ | Bearer token required on `/api/ingest` |
| `DB_PATH` | `./data/solarstats.db` | SQLite file |
| `HISTORY_RETENTION_DAYS` | `30` | Old samples pruned periodically |
| `AZURE_CLIENT_ID` | — | Entra application ID |
| `AZURE_CLIENT_SECRET` | — | Client secret |
| `AZURE_TENANT` | `consumers` | `consumers` or `common` |
| `AUTH_REDIRECT_URI` | — | Exact callback URL registered in Entra |
| `SESSION_SECRET` | — | Cookie signing key |
| `ORIGIN` | — | Public origin, e.g. `https://solar.example.com` |
| `RP_ID` | hostname of `ORIGIN` | WebAuthn RP ID (apex host, no path) |
| `COOKIE_SECURE` | off unless `production` | Set `1` behind HTTPS |

## API

- `POST /api/ingest` — Pi snapshot (`Authorization: Bearer <INGEST_SECRET>`); includes optional `loadsDailyKwh`
- `GET /api/history?range=24h` — chart series + totals (**session required**)
- `GET /` — dashboard (**approved session**)
- `WS /ws` — live sample push (**approved session**)
- `GET /api/health` — public liveness
- `GET /login`, `/auth/microsoft`, `/auth/callback`, `POST /logout`
- Passkey + `/admin` routes as above

## Energy

On each ingest, cumulative kWh is updated with trapezoidal integration of `outputPower` over the time delta (gaps &gt; 5 minutes are skipped).

Daily device kWh from HA (`loadsDailyKwh`) drives the **All devices today** doughnut; those meters reset at midnight in Home Assistant.
