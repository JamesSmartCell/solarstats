# IPC loopback harness

Python encode/decode and a fake C6 for [protocol/ipc.md](../../protocol/ipc.md).

```bash
cd halite/tools/ipc-loopback
python frame.py selftest
python frame.py ping-demo
python frame.py fake-c6 > canned.bin
```

No PySerial dependency for `selftest` / in-memory demos. UART bring-up can pipe `fake-c6` later.
