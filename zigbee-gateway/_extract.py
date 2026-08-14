import json
path = r"C:\Users\soyle\.cursor\projects\d-Users-soyle-Dev-HomeAssistant\agent-transcripts\93ba28bc-9c0e-417f-86f3-ead16f92addb\93ba28bc-9c0e-417f-86f3-ead16f92addb.jsonl"
out = r"d:\Users\soyle\Dev\HomeAssistant\zigbee-gateway\_extract_coord.txt"
n = 0
writes = []
with open(path, encoding="utf-8") as f:
    for i, line in enumerate(f):
        n += 1
        try:
            o = json.loads(line)
        except Exception:
            continue
        content = o.get("message", {}).get("content")
        if not isinstance(content, list):
            continue
        for c in content:
            if not isinstance(c, dict) or c.get("type") != "tool_use":
                continue
            if c.get("name") != "Write":
                continue
            inp = c.get("input", {})
            pth = str(inp.get("path", ""))
            if pth.endswith("zigbee_coordinator.c"):
                text = inp.get("contents", "")
                writes.append((i, len(text)))
                with open(rf"d:\Users\soyle\Dev\HomeAssistant\zigbee-gateway\_coord_line_{i}.c", "w", encoding="utf-8") as wf:
                    wf.write(text)
with open(out, "w", encoding="utf-8") as wf:
    wf.write(f"lines={n}\nwrites={writes}\n")
