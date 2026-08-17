# Part 1 acceptance story

**Title:** One Zigbee plug + one ESPHome switch on the HALite UI

## Goal

Prove end-to-end **direct control and reporting** for both transports — no automations.

## Actors / hardware

| Piece | Role |
|-------|------|
| ESP32-P4 | Logic / registry / API authority |
| C6 (`c6-radio`) | Zigbee coordinator **+ Wi‑Fi** (ESPHome MQTT, SmartHub/LAN, cloud path) |
| UART | Only P4↔C6 link (not Wi‑Fi) |
| Zigbee smart plug | Zigbee device |
| ESPHome node with one `switch` | Wi‑Fi device |
| LAN MQTT broker | ESPHome ↔ C6 |
| Browser on LAN | Control UI (P4 API via hosted netif or C6 proxy) |

## Preconditions

- [ ] ADRs accepted (incl. revised ADR-0002: Wi‑Fi on C6)
- [ ] IPC v0 enough for Zigbee join/on-off + ESPHome state/set
- [ ] **C6** Wi‑Fi associated; MQTT connected (`NET_STATUS` / health)
- [ ] Hub flashed via builder or `idf.py`
- [ ] ESPHome endpoint MQTT topics match hub YAML ([builder.md](builder.md))

## Steps

1. Open hub UI — entity list loads (may be empty of Zigbee).
2. Enable **Permit join** → P4 → IPC → C6 join window.
3. Pair Zigbee plug → UI shows `switch.*` (and power sensor if capable).
4. Toggle Zigbee switch → plug changes; confirm via `ATTR_REPORT`.
5. ESPHome `switch.bench_lamp` appears after C6 MQTT → `ESPHOME_STATE` → P4.
6. Toggle ESPHome switch in UI → P4 `CMD_ESPHOME_SET` → C6 MQTT publish → device moves.
7. Refresh / reconnect WS → both entities still listed.
8. Disable permit join → new Zigbee devices do not appear.

## Pass criteria

| # | Criterion |
|---|-----------|
| P1 | Zigbee join reaches P4 over **IPC** (not Wi‑Fi to P4) |
| P2 | Zigbee toggle works via IPC (&lt; ~2 s UX) |
| P3 | ESPHome toggle: P4 → IPC → **C6 MQTT** → device |
| P4 | WS pushes state for both transports |
| P5 | Health shows C6 link + Wi‑Fi/MQTT up on C6 |
| P6 | No automations/schedules required |
| P7 | No Wi‑Fi association between P4 and C6 |

## Explicit non-goals for this story

- Lights Level/Color, Tuya `0xEF00`
- Passkeys / full cloud product
- History charts
- Admin ACL checkboxes

## Traceability

| Layer | Doc |
|-------|-----|
| Entities | [entity-model.md](entity-model.md) |
| Zigbee / ESPHome path | [../protocol/ipc.md](../protocol/ipc.md), [c6-extract-map.md](c6-extract-map.md) |
| HTTP/WS | [../protocol/p4-api.openapi.yaml](../protocol/p4-api.openapi.yaml) |
| Builder | [builder.md](builder.md) |
| Wi‑Fi ownership | [adr/0002-wifi-on-p4.md](adr/0002-wifi-on-p4.md) |
