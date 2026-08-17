# HALite documentation index

Design-phase docs. Implementation of firmware starts after D1–D2 (entity model + IPC) are accepted.

| Doc | Purpose |
|-----|---------|
| [adr/0001-stack-on-c6.md](adr/0001-stack-on-c6.md) | Zigbee stack lives on C6 |
| [adr/0002-wifi-on-p4.md](adr/0002-wifi-on-p4.md) | Wi‑Fi + Zigbee on C6; UART-only to P4 |
| [adr/0003-ipc-uart-packed.md](adr/0003-ipc-uart-packed.md) | UART + packed structs |
| [adr/0004-esphome-mqtt-ingest.md](adr/0004-esphome-mqtt-ingest.md) | ESPHome MQTT on C6 → IPC to P4 |
| [adr/0005-builder-external-component.md](adr/0005-builder-external-component.md) | PC builder = ESPHome external component |
| [entity-model.md](entity-model.md) | Domains, capabilities, state, commands |
| [../protocol/ipc.md](../protocol/ipc.md) | IPC v0 + sequences |
| [../tools/ipc-loopback/](../tools/ipc-loopback/) | Fake C6 / frame selftest |
| [c6-extract-map.md](c6-extract-map.md) | File-level extract from zigbee-gateway |
| [../protocol/p4-api.openapi.yaml](../protocol/p4-api.openapi.yaml) | Part 1 HTTP/WS API |
| [builder.md](builder.md) | Creation YAML + compile/flash flow |
| [part1-acceptance.md](part1-acceptance.md) | One Zigbee plug + one ESPHome switch |
| [security-cloud.md](security-cloud.md) | Passkeys, capability tokens, cloud shim hooks |
