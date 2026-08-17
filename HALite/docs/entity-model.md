# HALite entity model (Part 1)

Shared by P4 hub, C6 radio (via IPC), builder manifest, and control UI.

## Goals

- HA-like domains without full HA compatibility.
- One registry on P4; transports are `zigbee` or `esphome`.
- Align Zigbee capability bits with [`zigbee-gateway/main/config.h`](../../zigbee-gateway/main/config.h).

## Identity

| Field | Type | Notes |
|-------|------|--------|
| `device_id` | string | Stable id. Zigbee: `zb_<ieee_hex>`. ESPHome: `esphome_<node_name>`. |
| `entity_id` | string | `domain.object_id` (e.g. `switch.office_plug`). Unique on the hub. |
| `name` | string | Friendly name for UI. |
| `transport` | enum | `zigbee` \| `esphome` |
| `capabilities` | uint32 | Bitmask (below). |
| `online` | bool | Recent activity / broker session. |
| `last_seen` | int64 | Unix ms. |

### Zigbee extras (stored on P4, filled from IPC)

| Field | Notes |
|-------|--------|
| `ieee` | uint64 |
| `short_addr` | uint16 |
| `endpoint` | primary On/Off EP |
| `manufacturer` / `model` | from interview |

### ESPHome extras

| Field | Notes |
|-------|--------|
| `mqtt_state_topic` | subscribe |
| `mqtt_command_topic` | publish commands |
| `node_name` | ESPHome `esphome.name` |

## Domains (Part 1)

| Domain | Readable state | Commands |
|--------|----------------|----------|
| `switch` | `on` \| `off` | `turn_on`, `turn_off`, `toggle` |
| `binary_sensor` | `on` \| `off` | none |
| `sensor` | numeric (+ optional unit) | none |

**Stretch / later:** `light` (on/off first; Level/Color when C6 gains clusters).

## Capability bits

Same numeric values as zigbee-gateway for Zigbee devices:

| Bit | Name | Typical entity |
|-----|------|----------------|
| 0 | `CAP_TEMPERATURE` | `sensor.*_temperature` |
| 1 | `CAP_HUMIDITY` | `sensor.*_humidity` |
| 2 | `CAP_CONTACT` | `binary_sensor.*_contact` |
| 3 | `CAP_OCCUPANCY` | `binary_sensor.*_occupancy` |
| 4 | `CAP_ON_OFF` | `switch.*` |
| 5 | `CAP_POWER` | `sensor.*_power` (W) |
| 6 | `CAP_ENERGY` | `sensor.*_energy` (kWh) |
| 7 | `CAP_SMOKE` | `binary_sensor.*_smoke` |
| 8 | `CAP_BATTERY` | `sensor.*_battery` (%) |
| 9 | `CAP_TAMPER` | `binary_sensor.*_tamper` |
| 10 | `CAP_SMOKE_TEST` | optional |
| 11 | `CAP_BATTERY_LOW` | `binary_sensor.*_battery_low` |

ESPHome devices set only the bits that match configured entities (often just `CAP_ON_OFF`).

## State values

```json
{
  "entity_id": "switch.office_plug",
  "state": "on",
  "attributes": {
    "device_id": "zb_00124b0012345678",
    "transport": "zigbee",
    "friendly_name": "Office plug"
  },
  "last_changed": 1720000000000
}
```

Sensors use stringified or numeric `state` plus `attributes.unit_of_measurement` (`W`, `kWh`, `%`, `°C`, …).

Derived metrics (e.g. watts from V×A) are computed on **C6** before IPC (per product goals); P4 stores the simplified value.

## Commands (Part 1)

```json
{
  "entity_id": "switch.office_plug",
  "action": "toggle"
}
```

Allowed `action`: `turn_on` | `turn_off` | `toggle`.  
Optional `value` for future `set` (brightness, etc.) — ignored in Part 1 for switches.

P4 routes:

- `transport=zigbee` → IPC `CMD_SET_ON_OFF` (C6 speaks ZCL)
- `transport=esphome` → IPC `CMD_ESPHOME_SET` (C6 publishes MQTT over its Wi‑Fi)

## Machine-readable schema

See [entity-model.schema.json](entity-model.schema.json) for registry export / builder manifests.
