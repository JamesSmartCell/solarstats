# HALite C6 radio

ESP-IDF firmware for **ESP32-C6**: Zigbee coordinator + Wi‑Fi, talking to the P4 over **UART IPC**.

Extracted from [`zigbee-gateway`](../../../zigbee-gateway) (that project is unchanged and still MQTT→HA).

## What this image does

| Path | Role |
|------|------|
| Zigbee 802.15.4 | Same stack as zigbee-gateway (`zigbee_coordinator.c`) |
| UART IPC | Host link to P4 — joins, attr reports, permit join, on/off commands |
| Wi‑Fi STA | ESPHome MQTT, later SmartHub/cloud — **not** used to reach P4 |

BOOT button still opens permit-join (pauses Wi‑Fi during pair, same as the gateway).

## Build / flash

This C6 is programmed with an **external USB-UART**, not native USB. Put the chip in **download mode** first, then flash.

### Download mode (ESP32-C6)

Typical strapping:

1. Wire the programmer: **GND**, **3V3** (if the board is not otherwise powered), **TX→C6 RX (UART0)**, **RX→C6 TX (UART0)**.
2. Hold **BOOT** (GPIO9) low.
3. Pulse **EN / RESET** (or power-cycle).
4. Release BOOT. The ROM bootloader should sit on UART0 waiting for esptool.

If the adapter has DTR/RTS auto-reset, `idf.py flash` can enter download mode itself — still confirm BOOT/EN wiring.

**Do not use the IPC UART (UART1 GPIO4/5) for flashing.** That link is for P4 after boot. Flash is UART0.

### Commands

```powershell
cd halite/firmware/c6-radio
. .\env.ps1
idf.py set-target esp32c6
idf.py menuconfig   # Wi-Fi, MQTT (optional), IPC UART pins
idf.py build
# After the C6 is in download mode:
idf.py -p COMx flash
# Optional: reset out of download mode, then
idf.py -p COMx monitor
```

Replace `COMx` with the USB-UART adapter port. After flash, release BOOT and reset so it runs the app — look for `HALite C6 radio starting`.

## Bring-up without a P4

Loop TX–RX on the C6 (or use a USB-UART to a PC) and run:

```powershell
python ..\..\tools\ipc-loopback\frame.py ping-demo
```

A PC can send a `PING` frame; C6 replies `ACK` + `PONG`.

## Config notes

- Empty MQTT host = Zigbee+IPC only (ESPHome ingest off).
- ESPHome entities: set `HALITE_ESPHOME_1_*` in menuconfig (entity id + state/command topics).
- Do not flash this onto a box that should keep talking to Home Assistant via zigbee-gateway.
