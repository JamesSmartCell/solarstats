# ADR-0001: Zigbee stack on C6 (coordinator-on-C6)

- **Status:** Accepted
- **Date:** 2026-08-17

## Context

HALite uses an ESP32-P4 + ESP32-C6 combo. Zigbee can run as:

1. **Coordinator-on-C6** — full esp-zigbee-lib stack on C6; P4 speaks a host IPC.
2. **RCP** — radio co-processor on C6; stack on P4 (Espressif dual-SoC pattern).

We already ship a working coordinator in [`zigbee-gateway`](../../zigbee-gateway) (`zigbee_coordinator.c`, device registry, cluster interview).

## Decision

**Keep the Zigbee 3.0 coordinator stack on the C6.** P4 never speaks ZCL in Part 1. C6 reports simplified attributes/events and executes high-level commands over IPC.

## Consequences

- Maximizes reuse of zigbee-gateway Zigbee core.
- C6 firmware stays Zigbee-aware (HALite “radio subsystem”).
- P4 stays smaller and transport-agnostic (Zigbee vs ESPHome look the same at the entity layer).
- Not an Espressif RCP image; do not expect ZHA-style serial protocol compatibility.
- Future RCP migration would be a breaking IPC redesign — out of scope.
