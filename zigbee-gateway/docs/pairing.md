# Pairing Zigbee sensors

## Before you start

1. Gateway is online: `zigbee-gw/bridge/status` = `online`.
2. Mosquitto + MQTT discovery are enabled in Home Assistant.
3. Sensor is factory-reset or in pairing mode (check the vendor manual).

## Open the network

Either:

- Press the FireBeetle **BOOT** button (GPIO9), or
- In HA, turn **Permit join** ON on the *ESP32-C6 Zigbee Gateway* device, or
- Publish MQTT: topic `zigbee-gw/bridge/permit_join`, payload `ON`

Default window: **180 seconds**. LED blinks when join is opened / a device announces.

## Pair

1. Trigger the sensor’s pairing / reset sequence near the coordinator.
2. Serial monitor should show something like:

```text
Device announce short=0x…. ieee=0x….
Matched kind=0 …   # temperature
Matched kind=1 …   # humidity
```

3. HA should create a MQTT device named from the Zigbee model string (or `ZigbeeSensor`).

## Verify

| Check | Where |
|-------|--------|
| Bridge online | MQTT explorer / HA device availability |
| Sensor state updating | Entity history for temperature / contact / etc. |
| Survive reboot | Power-cycle the C6; sensors should stay paired |

## Bring HA entities back (no re-pair)

Removing cards/devices in Home Assistant does **not** unpair Zigbee.
If the plug is still on the gateway network, republish discovery:

- MQTT topic: `zigbee-gw/bridge/rediscover`
- Payload: `ON`

## Remove devices (do this — not only delete in HA)

Deleting a card/device in Home Assistant is temporary: the gateway still has the
device in NVS and will republish MQTT discovery on the next reconnect.

Remove via MQTT so Zigbee leave + NVS + retained discovery are all cleared:

- One device: topic `zigbee-gw/bridge/remove`, payload `9285000024032588`
- All switches: topic `zigbee-gw/bridge/remove`, payload `switches`

Then **factory-reset the physical plug** (hold its button until it blinks) so it
forgets the old network. Without that, it will not join again.

Re-pair:

1. Turn **Permit join** ON (BOOT button or HA switch).
2. Put the plug in pairing mode near the FireBeetle.
3. Serial should show `Device announce` then `Cluster 0x0006`.

## Full wipe (nuclear)

```powershell
idf.py -p COMx erase-flash
idf.py -p COMx flash
```

Then pair every device again. This clears Zigbee NVS and the device registry.

## Troubleshooting

| Symptom | Likely cause |
|---------|----------------|
| No announce | Permit join closed; device not factory-reset; still joined to old network; RF distance |
| Announce but no HA entity | MQTT discovery off; broker auth wrong; unsupported clusters |
| Entity exists, APS NO_ACK (`0xa7`) | Stale route — power-cycle the plug, then toggle / rediscover |
| Deleted HA card, device "gone" | Publish `zigbee-gw/bridge/rediscover` — Zigbee pairing was never cleared |
| Wi-Fi drops often | Heavy 2.4 GHz congestion; move Zigbee channel; reduce Wi-Fi traffic |
