# ESP32-C6 Zigbee → Home Assistant Gateway

ESP-IDF firmware for the **DFRobot FireBeetle 2 ESP32-C6** that runs as a **Zigbee 3.0 coordinator** and bridges joined sensors into Home Assistant over **MQTT discovery**.

```text
Zigbee sensors ──802.15.4──► FireBeetle ESP32-C6 ──Wi-Fi MQTT──► Mosquitto ──► Home Assistant
```

This is separate from the PowMr ESPHome path in the repo root. HA does **not** use ZHA/Zigbee2MQTT for this gateway — entities appear via the MQTT integration.

## What v1 supports

| Capability | Zigbee cluster | HA entity |
|------------|----------------|-----------|
| Temperature | Temperature Measurement `0x0402` | `sensor` |
| Humidity | Relative Humidity `0x0405` | `sensor` |
| Contact / door | IAS Zone `0x0500` | `binary_sensor` |
| Occupancy / motion | Occupancy Sensing `0x0406` | `binary_sensor` |
| Permit join | BOOT button + MQTT switch | `switch` |

Mains sockets / power monitoring are deferred.

## Requirements

- FireBeetle 2 ESP32-C6 (or any ESP32-C6 with native 802.15.4)
- ESP-IDF **≥ 5.2** (tested with 5.5.4)
- Home Assistant OS with **Mosquitto broker** add-on
- MQTT integration enabled (discovery on)

## Configure credentials

```powershell
cd zigbee-gateway
# After ESP-IDF export.ps1:
idf.py set-target esp32c6
idf.py menuconfig
```

Under **Zigbee Gateway Configuration** set:

- WiFi SSID / password
- MQTT host (Pi hostname or IP), port `1883`, username / password
- Zigbee primary channel (default **15**)

Or edit `sdkconfig` after the first configure.

## Build & flash

```powershell
$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_TOOLS_PATH = "D:\Espressif"
. "$env:IDF_PATH\export.ps1"

cd zigbee-gateway
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the FireBeetle USB serial port.

## Home Assistant / Mosquitto

1. Install **Mosquitto broker** (Settings → Add-ons).
2. Create a user matching `CONFIG_ZBGW_MQTT_USERNAME` / password.
3. Settings → Devices & services → **MQTT** → enable discovery.
4. Flash the C6; within a minute you should see:
   - Topic `zigbee-gw/bridge/status` = `online`
   - Device **ESP32-C6 Zigbee Gateway** with a **Permit join** switch

## Channel planning (Wi-Fi coexistence)

ESP32-C6 shares one 2.4 GHz radio between Wi-Fi and Zigbee. Prefer:

| Your Wi-Fi channel | Prefer Zigbee channel |
|--------------------|------------------------|
| 1 | 15, 20, 25 |
| 6 | 11, 15, 25, 26 |
| 11 | 15, 20, 25 |

Avoid parking Zigbee on top of the AP’s Wi-Fi channel. Keep MQTT traffic light (this firmware already uses Wi-Fi modem sleep).

## Pairing a sensor

See [docs/pairing.md](docs/pairing.md).

Short version:

1. In HA, turn **Permit join** ON (or press **BOOT** on the FireBeetle).
2. Put the Zigbee sensor into pairing mode.
3. Watch serial log for `Device announce` / `Matched kind=…`.
4. Entities appear under MQTT devices using the sensor IEEE address.

## MQTT topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `zigbee-gw/bridge/status` | publish (LWT) | `online` / `offline` |
| `zigbee-gw/bridge/permit_join` | subscribe | `ON` / `OFF` |
| `zigbee-gw/bridge/permit_join/state` | publish | current permit-join state |
| `zigbee-gw/bridge/info` | publish | PAN / channel JSON |
| `zigbee-gw/<ieee>/temperature` | publish | °C |
| `zigbee-gw/<ieee>/humidity` | publish | % |
| `zigbee-gw/<ieee>/contact` | publish | `ON` / `OFF` |
| `zigbee-gw/<ieee>/occupancy` | publish | `ON` / `OFF` |
| `homeassistant/.../config` | publish | HA MQTT discovery |

## Hardware notes (FireBeetle 2)

| Function | GPIO |
|----------|------|
| Status LED | 15 |
| BOOT / permit join | 9 |

Network keys and the joined-device table live in NVS (`zb_storage` + default NVS). Sensors should survive a gateway reboot without re-pairing.

## Limitations

- Single-SoC Wi-Fi + Zigbee coexistence is workable for a small home sensor mesh, but weaker than a dedicated USB stick (SkyConnect / Sonoff ZBDongle) or Espressif’s dual-SoC RCP gateway.
- Not every Tuya/Aqara quirk is covered; reporting intervals may need per-device tweaks later.
- Factory-new formation uses the configured primary channel; erase NVS if you need a clean Zigbee network:

```powershell
idf.py -p COMx erase-flash
idf.py -p COMx flash
```
