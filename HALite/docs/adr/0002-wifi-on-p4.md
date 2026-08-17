# ADR-0002: Wi‑Fi + Zigbee on C6; UART-only link to P4

- **Status:** Accepted (revised)
- **Date:** 2026-08-17
- **Supersedes:** Earlier draft that put application Wi‑Fi only on P4

## Context

Today’s [`zigbee-gateway`](../../zigbee-gateway) is a **standalone C6** that speaks Zigbee and **Wi‑Fi MQTT to a remote host** (HA). That Wi‑Fi hop is the host link.

HALite adds a **P4** on the same board (P4‑M3). The win is:

> **C6 does not need Wi‑Fi to talk to P4** — that path is UART IPC (ADR-0003).

C6 still needs Wi‑Fi for everything off-board:

- **SmartHub / control plane** (LAN UI and hub services clients reach over IP)
- **ESPHome** (and other Wi‑Fi smart devices)
- **Cloud exposing service** (tunnel / veneer relay)

C6 already has a single 2.4 GHz radio shared by Wi‑Fi and 802.15.4; coexistence remains a real constraint (channel planning, pair-window behaviour) — same class of problem as the current gateway, not a reason to move Wi‑Fi off C6.

## Decision

1. **C6 runs Zigbee + Wi‑Fi** (STA for normal use; optional AP for setup).
2. **P4 ↔ C6 is UART IPC only** — never a Wi‑Fi association between the two chips.
3. **C6 Wi‑Fi** is the hub’s RF path for:
   - ESPHome / Wi‑Fi device traffic (Part 1: MQTT client on C6)
   - Cloud expose / shim
   - SmartHub / LAN clients (control UI, REST/WS consumers)
4. **P4** remains the **logic authority** (registry, state store, Part 2 automations, auth). It consumes Zigbee and ESPHome facts over IPC; it does not peer with C6 over Wi‑Fi.
5. **IP how P4 serves API (Part 1 target):** prefer **esp_hosted / netif via C6** so P4 binds HTTP/WS on a LAN address while C6 owns the radio. Fallback: C6 terminates HTTP/WS and proxies to P4 over IPC if hosted bring-up lags.

## Consequences

- c6-radio **keeps** `wifi_net` (adapted); does **not** use Wi‑Fi as the P4 host protocol.
- Replace “MQTT to Home Assistant” with: MQTT/ESPHome + cloud client on C6, **plus** IPC up to P4 for canonical state.
- Coexistence docs (Zigbee channel vs AP channel) stay mandatory in builder/board profiles.
- P4 firmware may have little or no native Wi‑Fi of its own on combo modules — board profile must state hosted vs proxy API mode.
- ADR-0004: ESPHome MQTT client lives on **C6**, not on a P4-only radio.
