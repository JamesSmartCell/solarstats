# P4 local test UI

Bench page on this PC. It talks to the P4 over **USB serial** (`json …` console lines) and the P4 talks to the C6 over IPC.

```powershell
cd HALite/tools/p4-test-ui
pip install -r requirements.txt
python server.py
```

Open http://127.0.0.1:8765/

1. Flash `p4-hub` first (this UI needs the `json` console command).
2. Pick the P4 USB-C COM port (not the C6 FTDI/CH340 programmer).
3. **Permit join**, then put a Zigbee device in pairing mode.
4. Joined devices show name, transport, and on/off. Switches can be toggled.

Do not run `idf.py monitor` on the same COM port at the same time.
