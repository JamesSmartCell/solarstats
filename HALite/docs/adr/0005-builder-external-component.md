# ADR-0005: Creation UX as ESPHome external component (not a full fork)

- **Status:** Accepted
- **Date:** 2026-08-17

## Context

We want a PC-local “creation” experience like ESPHome: edit config in a dashboard (browser), **compile and flash on the host** via PlatformIO/IDF. Options:

1. Full fork of ESPHome / Device Builder
2. ESPHome **external component** + scripts that invoke `halite/firmware` IDF builds
3. Standalone Electron/web app

ESPHome does **not** compile inside the browser; the dashboard delegates to a local `esphome` process.

## Decision

Ship HALite builder as an **ESPHome external component / package** under `halite/builder/` that:

- Accepts a small hub YAML (board, Zigbee channel, Wi‑Fi, MQTT, entity manifest)
- Invokes ESP-IDF builds for `firmware/c6-radio` and `firmware/p4-hub`
- Flashes over USB using existing IDF/`esptool` flows
- Remains callable from CLI without the dashboard (`halite build` / documented `idf.py` wrappers)

**Do not fork** ESPHome unless upstream hooks prove insufficient.

## Consequences

- Hub firmware stays pure C / ESP-IDF (not generated Arduino sketches).
- Endpoint ESPHome devices remain normal ESPHome YAML projects; they are not “built by” the hub component except as documented companion configs.
- Upstream ESPHome churn is isolated to the thin builder glue.
- Details: [builder.md](../builder.md).
