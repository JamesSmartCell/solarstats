# P4 HTTP / WebSocket API (Part 1)

Machine-readable OpenAPI: [p4-api.openapi.yaml](p4-api.openapi.yaml).

## WebSocket contract

Connect to `ws://<hub>/ws` (optional `?token=` if hub token configured).

| type | Direction | Body |
|------|-----------|------|
| `hello` | S→C | Full `entities` snapshot |
| `state` | S→C | One entity state change |
| `device_joined` | S→C | New device + entities |
| `device_left` | S→C | `device_id` |
| `permit_join` | S→C | `{ enabled }` |
| `ping` / `pong` | either | keepalive |

REST auth (optional Part 1): `Authorization: Bearer <hub_token>`.
