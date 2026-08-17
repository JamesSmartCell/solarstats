# C6 radio extract map (zigbee-gateway → halite/firmware/c6-radio)

Goal: keep Zigbee coordinator behaviour; **keep Wi‑Fi** for ESPHome / SmartHub / cloud; **replace “MQTT as host link to HA”** with **UART IPC to P4**; adapt MQTT as an **off-board** client only.

Do **not** delete or break [`zigbee-gateway`](../../zigbee-gateway) until c6-radio is proven. Prefer copy-then-adapt, then optionally share a common submodule later.

## File disposition

| Source (`zigbee-gateway/`) | Disposition in `halite/firmware/c6-radio/` | Notes |
|----------------------------|---------------------------------------------|--------|
| `main/zigbee_coordinator.c` | **KEEP → adapt** | Core stack; also emit IPC (not only MQTT) |
| `main/zigbee_coordinator.h` | **KEEP → adapt** | Public API stays; add IPC glue hooks |
| `main/device_registry.c` | **KEEP** | NVS device table (max 32); may grow for ESPHome-side cache |
| `main/device_registry.h` | **KEEP** | Capability bits stay aligned with entity model |
| `main/config.h` | **KEEP → adapt** | Keep Zigbee/caps/channel; retarget MQTT macros for ESPHome/cloud, not HA host |
| `main/board_io.c` / `.h` | **KEEP** | LED / BOOT → permit join still useful |
| `main/app_main.c` | **REWRITE** | NVS → registry → board → Wi‑Fi → Zigbee → **IPC UART** → MQTT (ESPHome/cloud) |
| `main/mqtt_bridge.c` / `.h` | **KEEP → rewrite** | No longer “host = HA”. Subscribe/publish ESPHome; optional cloud shim topics; mirror state to P4 via IPC |
| `main/ha_discovery.c` / `.h` | **DROP or gate** | P4 owns HALite entity API; do not publish HA discovery as the primary UI path |
| `main/wifi_net.c` / `.h` | **KEEP → adapt** | ADR-0002: C6 owns Wi‑Fi (coexistence with Zigbee remains) |
| `main/dev_pair_test.c` / `.h` | **OPTIONAL** | Bench helper; link only in debug builds |
| `main/Kconfig.projbuild` | **REWRITE** | UART pins, baud, Zigbee channel, Wi‑Fi, MQTT broker (ESPHome), cloud endpoint |
| `main/idf_component.yml` | **KEEP** | `esp-zigbee-lib`; keep `mdns` if SmartHub discovery needs it |
| `main/CMakeLists.txt` | **REWRITE** | Keep wifi + mqtt; drop/gate ha_discovery; add `ipc_*.c` |
| `CMakeLists.txt` | **KEEP** | New project name `halite-c6-radio` |
| `partitions.csv` | **KEEP** | Retain `zb_storage` |
| `sdkconfig.defaults*` | **REWRITE** | Wi‑Fi + Zigbee coexistence defaults; UART IPC |
| `README.md` / `docs/pairing.md` | **REWRITE** | IPC to P4; Wi‑Fi for ESPHome/cloud/SmartHub |
| `_restore/*` | **IGNORE** | Historical only |

## New files (c6-radio)

| File | Role |
|------|------|
| `main/ipc_host.c` / `.h` | UART framing, TX/RX task, dispatch to coordinator / MQTT |
| `main/ipc_codec.c` / `.h` | Pack/unpack structs from [protocol/ipc.md](../protocol/ipc.md) |
| `main/halite_crc.c` / `.h` | CRC8/CRC16 shared algorithm |
| `main/esphome_mqtt.c` / `.h` | Optional split from mqtt_bridge for clarity |

## Data paths

```text
Zigbee device ──802.15.4──► zigbee_coordinator ──► ipc_host ──UART──► P4
ESPHome node  ──Wi‑Fi/MQTT─► mqtt_bridge      ──► ipc_host ──UART──► P4
Cloud / SmartHub clients ──Wi‑Fi──► C6 (API proxy or hosted netif for P4)
P4 commands ──UART──► ipc_host ──► zigbee_coordinator / mqtt_bridge
```

**Not** a path: `P4 ──Wi‑Fi──► C6`.

## Callback rewiring

Today (gateway): Zigbee events → MQTT/HA discovery (host = HA).

HALite:

```text
zigbee_coordinator ──report/join──► ipc_host_send(ATTR_REPORT / DEVICE_JOINED)
mqtt_bridge (ESPHome) ──state──► ipc_host_send(ESPHOME_STATE / ATTR_REPORT)
ipc_host_recv(CMD_*) ──► zigbee_coordinator_*  or  mqtt publish
```

## Build / flash (future)

```text
cd halite/firmware/c6-radio
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

## Migration checklist

1. Copy tree; rename project; gate/remove HA discovery as primary path.
2. Add `ipc_host`; prove `PING`/`PONG` against [protocol/ipc.md](../protocol/ipc.md) harness.
3. Keep Wi‑Fi; retarget MQTT toward ESPHome + IPC mirror.
4. Wire Zigbee permit join + on/off + attr reports over IPC.
5. Leave zigbee-gateway MQTT→HA build green for existing installs.
