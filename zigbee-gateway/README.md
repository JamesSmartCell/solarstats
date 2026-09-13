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

Wi-Fi SSID/password and MQTT host/user/password are stored in **NVS** (flash), not baked into OTA images. Port `1883`, topic prefix, and Zigbee channel stay compile-time.

**First boot** (empty NVS): the C6 broadcasts open AP **`ZIGBEE_SETUP`**. Connect and open `http://192.168.4.1` (or the captive page). Enter Wi-Fi, MQTT host, username, and password. **Allow anonymous diagnostics** is on by default (failures plus an hourly health ping to `homesolar.percolate.one`; no Wi-Fi/MQTT passwords). It saves and reboots onto STA. Boards that already have credentials keep diagnostics on until you re-enter setup and uncheck it.

**Re-enter setup:** hold **BOOT** for 3 seconds at power-on (clears only those credentials, not the Zigbee mesh).

Kconfig Wi-Fi/MQTT fields are seeds only. If you flash a board that already has a real SSID in Kconfig, that seed is copied into NVS once so existing units keep working.

## OTA

The partition table is dual-slot (`ota_0` / `ota_1`). The **first** OTA-capable image must be USB-flashed (and `erase-flash` if you are moving off the old factory layout). After that, upload `zigbee-gateway.bin` in Solarstats **Admin → Zigbee gateways**. It is served at `https://homesolar.percolate.one/fw/zigbee-gateway.bin`.

`CONFIG_ZBGW_OTA_URL` defaults to that path. Auto-check on boot is off; publish ON to `zigbee-gw/bridge/ota` to pull a newer version. Same version is skipped. Credentials stay in NVS across updates.

```powershell
cd zigbee-gateway
# After ESP-IDF export.ps1:
idf.py set-target esp32c6
idf.py menuconfig
```

Under **Zigbee Gateway Configuration** set the OTA URL and (optionally) seed credentials. Zigbee primary channel default is **15**.

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

Avoid parking Zigbee on top of the AP’s Wi-Fi channel. Keep MQTT traffic light (this firmware already uses Wi-Fi modem sleep). Power/energy polls run every 15s and are paused while MQTT is down or publishing discovery, so a brief broker blip does not republish every HA entity.

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
| `zigbee-gw/bridge/ota` | subscribe | any payload starts an HTTPS OTA check |
| `zigbee-gw/bridge/ota/state` | publish | `checking` / `up_to_date` / `updating` / `failed` |
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

If MQTT stays down after **4** failed reconnects, a watchdog restarts the C6 (Zigbee NVS is kept). Pairing pauses Wi‑Fi/MQTT and does not trip the watchdog.

## Anonymous diagnostics

When the setup checkbox is on (default), the gateway POSTs to `CONFIG_ZBGW_DIAG_URL` (default `https://homesolar.percolate.one/api/diag/zbgw`):

- boot after Wi-Fi is up
- operational errors (MQTT watchdog, MQTT client error, Zigbee not ready, OTA failure)
- hourly **Device working correctly** if the last hour was clean
- a quiet poll every 2 minutes so **Admin → Zigbee gateways → Restart** arrives without waiting for the hour

The report is the STA MAC, firmware version, uptime, reset reason, heap, Wi-Fi RSSI, MQTT/Zigbee flags, and joined-device count. Solarstats `ZBGW_DIAG_TOKEN` must match `CONFIG_ZBGW_DIAG_TOKEN`.

## Limitations

- Single-SoC Wi-Fi + Zigbee coexistence is workable for a small home sensor mesh, but weaker than a dedicated USB stick (SkyConnect / Sonoff ZBDongle) or Espressif’s dual-SoC RCP gateway.
- Not every Tuya/Aqara quirk is covered; reporting intervals may need per-device tweaks later.
- Factory-new formation uses the configured primary channel; erase NVS if you need a clean Zigbee network:

```powershell
idf.py -p COMx erase-flash
idf.py -p COMx flash
```
