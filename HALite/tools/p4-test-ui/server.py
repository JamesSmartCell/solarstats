#!/usr/bin/env python3
"""Local HALite P4 bench UI — USB console JSON → localhost web page."""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Install pyserial:  pip install -r requirements.txt", file=sys.stderr)
    raise

HERE = Path(__file__).resolve().parent
JSON_PREFIX = "HALITE_JSON:"
SKIP_VIDS = {
    (0x0403, 0x6001),  # FTDI — typical C6 USB-UART programmer
    (0x1A86, 0x7523),  # CH340 — same class of adapter
}


class HubLink:
    def __init__(self) -> None:
        self._mu = threading.Lock()
        self._ser: serial.Serial | None = None
        self.port = ""
        self.last_error = ""

    def ports(self) -> list[dict]:
        out = []
        for p in list_ports.comports():
            vid, pid = p.vid or 0, p.pid or 0
            likely = (vid == 0x303A) or ("JTAG" in (p.description or "").upper())
            skip = (vid, pid) in SKIP_VIDS or "Bluetooth" in (p.description or "")
            out.append(
                {
                    "device": p.device,
                    "description": p.description or "",
                    "hwid": p.hwid or "",
                    "likely_p4": bool(likely and not skip),
                    "skip_hint": skip,
                }
            )
        return out

    def connect(self, port: str, baud: int = 115200) -> None:
        with self._mu:
            if self._ser:
                try:
                    self._ser.close()
                except Exception:
                    pass
                self._ser = None
            self.port = port
            self._ser = serial.Serial(port, baudrate=baud, timeout=0.05)
            time.sleep(0.2)
            self._ser.reset_input_buffer()
            self._ser.write(b"\r\n")
            self._drain_unlocked(0.3)
            self.last_error = ""

    def disconnect(self) -> None:
        with self._mu:
            if self._ser:
                try:
                    self._ser.close()
                except Exception:
                    pass
            self._ser = None
            self.port = ""

    def connected(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def _drain_unlocked(self, seconds: float) -> None:
        if not self._ser:
            return
        deadline = time.time() + seconds
        while time.time() < deadline:
            n = self._ser.in_waiting
            if n:
                self._ser.read(n)
            else:
                time.sleep(0.02)

    def cmd(self, line: str, wait: float = 2.5) -> dict:
        with self._mu:
            if not self._ser or not self._ser.is_open:
                raise RuntimeError("P4 serial not connected")
            self._drain_unlocked(0.05)
            self._ser.write((line.strip() + "\r\n").encode("utf-8"))
            buf = ""
            deadline = time.time() + wait
            while time.time() < deadline:
                chunk = self._ser.read(1024)
                if chunk:
                    buf += chunk.decode("utf-8", errors="replace")
                    for raw in buf.splitlines():
                        raw = raw.strip()
                        if raw.startswith(JSON_PREFIX):
                            payload = raw[len(JSON_PREFIX) :]
                            return json.loads(payload)
                else:
                    time.sleep(0.03)
            self.last_error = f"timeout waiting for {line!r}"
            raise TimeoutError(self.last_error)


HUB = HubLink()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _bytes(self, code: int, content_type: str, data: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        return json.loads(self.rfile.read(n).decode("utf-8") or "{}")

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/":
            html = (HERE / "index.html").read_bytes()
            self._bytes(200, "text/html; charset=utf-8", html)
            return
        if path == "/api/ports":
            self._json(200, {"ports": HUB.ports(), "connected": HUB.port})
            return
        if path == "/api/status":
            if not HUB.connected():
                self._json(200, {"ok": False, "connected": False, "error": "not connected", "entities": []})
                return
            try:
                data = HUB.cmd("json status")
                data["connected"] = True
                data["port"] = HUB.port
                self._json(200, data)
            except Exception as exc:
                self._json(503, {"ok": False, "connected": True, "error": str(exc), "entities": []})
            return
        self._json(404, {"error": "not found"})

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        body = self._read_json()
        try:
            if path == "/api/connect":
                port = str(body.get("port") or "")
                if not port:
                    self._json(400, {"ok": False, "error": "port required"})
                    return
                HUB.connect(port)
                self._json(200, {"ok": True, "port": port})
                return
            if path == "/api/disconnect":
                HUB.disconnect()
                self._json(200, {"ok": True})
                return
            if path == "/api/permit":
                enabled = bool(body.get("enabled", True))
                data = HUB.cmd("json permit on" if enabled else "json permit off")
                self._json(200, data)
                return
            if path == "/api/command":
                entity_id = str(body.get("entity_id") or "")
                if not entity_id:
                    self._json(400, {"ok": False, "error": "entity_id required"})
                    return
                data = HUB.cmd(f"json toggle {entity_id}")
                self._json(200, data)
                return
        except Exception as exc:
            self._json(500, {"ok": False, "error": str(exc)})
            return
        self._json(404, {"error": "not found"})


def main() -> int:
    parser = argparse.ArgumentParser(description="HALite P4 local test UI")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--port", help="P4 USB COM port (optional; pick in the UI)")
    args = parser.parse_args()

    if args.port:
        try:
            HUB.connect(args.port)
            print(f"Connected {args.port}")
        except Exception as exc:
            print(f"Could not open {args.port}: {exc}", file=sys.stderr)

    httpd = ThreadingHTTPServer((args.host, args.http_port), Handler)
    url = f"http://{args.host}:{args.http_port}/"
    print(f"HALite P4 test UI: {url}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped")
    finally:
        HUB.disconnect()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
