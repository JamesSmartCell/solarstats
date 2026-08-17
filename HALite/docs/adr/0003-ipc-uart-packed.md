# ADR-0003: P4↔C6 IPC over UART with packed frames

- **Status:** Accepted
- **Date:** 2026-08-17

## Context

C6 must push joins/reports and accept commands from P4. Candidates:

| Option | Pros | Cons |
|--------|------|------|
| JSON lines over UART | Easy to debug | CPU/RAM, framing ambiguity |
| CBOR | Compact, schema-friendly | Extra library on both sides |
| Packed LE structs | Tiny, matches C firmware | Versioning discipline required |
| SPI | Higher bandwidth | More pins, harder early bring-up |

Part 1 traffic is low rate (state + occasional commands).

## Decision

1. **Physical:** UART (default 921600 8N1; configurable in builder). SPI reserved as a later optional transport with the same message types.
2. **Encoding:** **Versioned packed little-endian structs** with a fixed header (`magic`, `version`, `type`, `flags`, `seq`, `len`, `crc16`).
3. **Reliability:** Stop-and-wait ACK for commands that mutate devices; attribute reports are fire-and-forget with optional P4 NACK/re-request later.

Full message catalog: [`../../protocol/ipc.md`](../../protocol/ipc.md).

## Consequences

- Shared `protocol/` headers can be included from both IDF apps (or duplicated carefully).
- Breaking changes bump `version`; unknown types are ignored with a `MSG_UNSUPPORTED` reply when solicited.
- Host PC loopback harness can speak the same framing over a USB-UART.
- C6 may also run Wi‑Fi (ADR-0002) for ESPHome / SmartHub / cloud; that traffic **never** replaces this UART link to P4.
