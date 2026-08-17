# ADR-0004: ESPHome device ingest via MQTT on C6 (Part 1)

- **Status:** Accepted (revised)
- **Date:** 2026-08-17

## Context

Part 1 must expose ESPHome Wi‑Fi devices alongside Zigbee. Per ADR-0002, **C6 owns Wi‑Fi**; P4 is reached only over UART IPC.

Options:

1. **ESPHome Native API** client on C6 — efficient, heavier to implement.
2. **MQTT client on C6** — matches current gateway skills and ESPHome’s common MQTT mode.
3. MQTT client on P4 via hosted netif — possible later; adds little for Part 1 if C6 already runs Wi‑Fi.

## Decision

**Part 1: C6 is the MQTT client** on the LAN broker.

- Subscribe to configured ESPHome state topics (and/or HA MQTT discovery if present).
- Publish commands to ESPHome command topics when P4 requests an action over IPC.
- Normalize to the shared entity model and forward **state / join / leave** to P4 via IPC (same spirit as Zigbee `ATTR_REPORT` / device events — may reuse or extend IPC types for `transport=esphome`).
- P4 remains the **canonical registry** and issues commands with `entity_id`; C6 resolves topics / IEEE.

**Native API** remains a later candidate (still on C6 if Wi‑Fi stays there).

## Consequences

- Users still need a LAN MQTT broker for the ESPHome path (Pi / HA OS / etc.).
- Builder YAML’s `wifi` + `mqtt` blocks are **C6** configuration.
- zigbee-gateway’s MQTT bridge is a starting point, but HA discovery topics are replaced by IPC-to-P4 (and optional HALite cloud shim), not “MQTT is the P4 link.”
- IPC must carry enough ESPHome identity (topics / node name) for P4 UI — see [entity-model.md](../entity-model.md) and [ipc.md](../../protocol/ipc.md) extensions.
