# HALite P4 hub

ESP-IDF firmware for **ESP32-P4**: entity registry, UART IPC client to the C6, REST/WS API, USB console.

The C6 owns Zigbee + Wi‑Fi. This image never talks to the C6 over Wi‑Fi.

## What this image does

| Path | Role |
|------|------|
| UART IPC | Client to C6 — PING/PONG, joins, attr reports, permit join, on/off |
| Registry | Canonical entities (`switch.zb_<ieee>`, sensors, ESPHome) |
| USB console | `entities` / `permit` / `toggle` / `ping` (works without Ethernet) |
| HTTP + WS | `/` UI, `/api/health`, `/api/entities`, `/api/zigbee/permit_join` |

The HTTP API is served on the P4. It is reachable over **USB-NCM** (default): plug the **P4 USB-OTG** port (not the CH340 UART used for COM22). Windows gets a virtual NIC; the hub is `192.168.4.1`.

```
curl http://192.168.4.1/api/health
curl http://192.168.4.1/api/entities
```

CORS is open (`*`) so an external website can poll those URLs. Do **not** hit the C6 STA address (`192.168.50.10`) — that is not this API.

Ethernet is **off by default**. Enable `HALITE_P4_ETHERNET` in menuconfig if the board has a PHY (Function EV-style pins).

**ESP-Hosted LAN** (`HALITE_ESP_HOSTED`) is **off by default**. When both images enable it, the P4 associates over the PCB SDIO link and `httpd` binds a `192.168.50.x` address. That pulses C6 reset (GPIO54) — do not turn it on until the C6 is also a hosted+Zigbee image. UART jumpers stay until IPC moves to hosted custom-data.

## Wire C6 ↔ P4 (IPC)

On the P4-M3 header, use the onboard C6 flash UART (already labelled). Cross TX/RX:

| P4 | Header |
|----|--------|
| GPIO4 TX | `C6_RXD` (C6 GPIO17) |
| GPIO5 RX | `C6_TXD` (C6 GPIO16) |

GND is already common. Unplug any USB-UART from `C6_TXD`/`C6_RXD` first. C6 log lines are echoed on the P4 console as `C6>…`.

Do **not** use these pins for flashing. Confirm the real P4-M3 pinout in menuconfig if the combo board already routes a different pair.

## Build / flash

This image is built for **ESP32-P4 rev v1.x** (this board is v1.3). IDF 5.5 defaults to rev ≥ v3.1 — do not `--force` a v3 bootloader onto this chip.

P4 boards often flash over USB-UART (CH340/FTDI) or USB Serial/JTAG.

```powershell
cd halite/firmware/p4-hub
. .\env.ps1
idf.py set-target esp32p4
idf.py menuconfig   # IPC pins; optional Ethernet
idf.py build
idf.py -p COMx flash monitor   # COM22 is the CH340 on this P4
```

Replace `COMx` with the P4 USB port. Look for `HALite P4 hub starting`, then `PONG from C6` once the C6 is powered and the IPC wires are crossed.

## Local test UI (this PC)

The P4 has no LAN yet. Use the USB console from a page on the PC:

```powershell
cd HALite/tools/p4-test-ui
pip install -r requirements.txt
python server.py
```

Open http://127.0.0.1:8765/ — permit join, device list, toggle. Close `idf.py monitor` on that port first.

## Bring-up (no Ethernet)

USB monitor REPL:

```
entities
ping
permit on
toggle switch.zb_<16 hex ieee>
```

If Ethernet is on, open `http://<eth-ip>/`.

## Config notes

- Empty `HALITE_API_TOKEN` = open LAN API (Part 1).
- After the first PONG, the hub asks the C6 to rediscover already-joined Zigbee devices.
- Do not flash this onto a board that is not the HALite P4.
