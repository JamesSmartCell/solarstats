#!/usr/bin/env python3
"""HALite IPC v0 encode/decode + fake-C6 loopback (no hardware required).

Usage:
  python frame.py selftest
  python frame.py fake-c6          # stdin/stdout binary framing (for pipes)
  python frame.py ping-demo        # build PING, parse round-trip in memory

See ../../protocol/ipc.md
"""

from __future__ import annotations

import argparse
import struct
import sys

MAGIC = 0x484C
VERSION = 0x01

TYPE_PING = 0x01
TYPE_PONG = 0x02
TYPE_PERMIT_JOIN = 0x10
TYPE_DEVICE_JOINED = 0x20
TYPE_ATTR_REPORT = 0x22
TYPE_CMD_SET_ON_OFF = 0x30
TYPE_CMD_RESULT = 0x3F
TYPE_NET_STATUS = 0x40

FLAG_NEEDS_ACK = 1 << 0
FLAG_IS_ACK = 1 << 1
FLAG_IS_NACK = 1 << 2


def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def encode_frame(msg_type: int, payload: bytes = b"", *, seq: int = 1, flags: int = 0) -> bytes:
    if len(payload) > 512:
        raise ValueError("payload too large")
    # hdr without hdr_crc for crc16 input uses hdr_crc=0
    hdr_wo = struct.pack("<HBBBBHH", MAGIC, VERSION, msg_type, flags, 0, seq & 0xFFFF, len(payload))
    # insert hdr_crc over first 5 bytes (magic..flags)
    hcrc = crc8(hdr_wo[:5])
    hdr = struct.pack("<HBBBBHH", MAGIC, VERSION, msg_type, flags, hcrc, seq & 0xFFFF, len(payload))
    body = hdr + payload
    c16 = crc16_ccitt(hdr_wo + payload)
    return body + struct.pack("<H", c16)


def decode_frame(buf: bytes) -> tuple[int, int, int, bytes]:
    if len(buf) < 12:
        raise ValueError("short frame")
    magic, ver, msg_type, flags, hcrc, seq, length = struct.unpack_from("<HBBBBHH", buf, 0)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if ver != VERSION:
        raise ValueError("bad version")
    if len(buf) < 10 + length + 2:
        raise ValueError("truncated")
    payload = buf[10 : 10 + length]
    (c16,) = struct.unpack_from("<H", buf, 10 + length)
    hdr_wo = struct.pack("<HBBBBHH", magic, ver, msg_type, flags, 0, seq, length)
    if crc8(hdr_wo[:5]) != hcrc:
        raise ValueError("hdr crc")
    if crc16_ccitt(hdr_wo + payload) != c16:
        raise ValueError("frame crc")
    return msg_type, flags, seq, payload


def pack_device_joined(
    ieee: int,
    short_addr: int,
    endpoint: int,
    capabilities: int,
    manufacturer: str,
    model: str,
) -> bytes:
    return struct.pack(
        "<QHBBL32s32s",
        ieee,
        short_addr,
        endpoint,
        0,
        capabilities,
        manufacturer.encode()[:32].ljust(32, b"\0"),
        model.encode()[:32].ljust(32, b"\0"),
    )


def pack_attr_report(ieee: int, attr_id: int, ep: int, value_type: int, value_bits: int) -> bytes:
    return struct.pack("<QBBBBI", ieee, attr_id, ep, value_type, 0, value_bits & 0xFFFFFFFF)


def pack_cmd_result(req_seq: int, status: int) -> bytes:
    return struct.pack("<Hi", req_seq & 0xFFFF, status)


def selftest() -> None:
    frame = encode_frame(TYPE_PING, b"", seq=7, flags=FLAG_NEEDS_ACK)
    t, f, s, p = decode_frame(frame)
    assert t == TYPE_PING and s == 7 and f == FLAG_NEEDS_ACK and p == b""
    joined = pack_device_joined(0x00124B0012345678, 0xABCD, 1, 1 << 4, "Test", "Plug")
    frame2 = encode_frame(TYPE_DEVICE_JOINED, joined, seq=8)
    t2, _, _, p2 = decode_frame(frame2)
    assert t2 == TYPE_DEVICE_JOINED and len(p2) == len(joined)
    # 1000 round trips
    for i in range(1000):
        fr = encode_frame(TYPE_ATTR_REPORT, pack_attr_report(1, 1, 1, 0, i & 1), seq=i)
        decode_frame(fr)
    print("selftest ok")


def ping_demo() -> None:
    req = encode_frame(TYPE_PING, b"", seq=1, flags=FLAG_NEEDS_ACK)
    # fake C6: ACK + PONG
    ack = encode_frame(TYPE_PING, b"", seq=1, flags=FLAG_IS_ACK)
    pong = encode_frame(TYPE_PONG, struct.pack("<I", 12345), seq=2)
    for label, fr in (("req", req), ("ack", ack), ("pong", pong)):
        t, f, s, p = decode_frame(fr)
        print(f"{label}: type=0x{t:02x} flags=0x{f:02x} seq={s} payload={p.hex() or '-'}")


def fake_c6() -> None:
    """Read length-prefixed frames from stdin is awkward; read raw until we can parse.

    Simple mode: expect one complete frame per read chunk for demo; for real UART
    use a sliding buffer (left as firmware bring-up work).
    """
    data = sys.stdin.buffer.read()
    if not data:
        # emit canned DEVICE_JOINED + ATTR_REPORT for harness consumers
        ieee = 0x00124B0012345678
        sys.stdout.buffer.write(
            encode_frame(TYPE_DEVICE_JOINED, pack_device_joined(ieee, 0x1001, 1, 1 << 4, "Fake", "Plug"), seq=1)
        )
        sys.stdout.buffer.write(
            encode_frame(TYPE_ATTR_REPORT, pack_attr_report(ieee, 1, 1, 0, 1), seq=2)
        )
        sys.stdout.buffer.write(
            encode_frame(TYPE_NET_STATUS, struct.pack("<BBBBbBBB", 1, 1, 0, 0, -40, 0, 0, 0), seq=3)
        )
        return
    # respond to PING
    try:
        t, f, s, _ = decode_frame(data)
        if t == TYPE_PING:
            sys.stdout.buffer.write(encode_frame(TYPE_PING, b"", seq=s, flags=FLAG_IS_ACK))
            sys.stdout.buffer.write(encode_frame(TYPE_PONG, struct.pack("<I", 1), seq=s + 1))
        elif t == TYPE_CMD_SET_ON_OFF:
            sys.stdout.buffer.write(encode_frame(TYPE_CMD_SET_ON_OFF, b"", seq=s, flags=FLAG_IS_ACK))
            sys.stdout.buffer.write(encode_frame(TYPE_CMD_RESULT, pack_cmd_result(s, 0), seq=s + 1))
    except ValueError as e:
        print(f"decode error: {e}", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cmd", choices=["selftest", "ping-demo", "fake-c6"])
    args = ap.parse_args()
    if args.cmd == "selftest":
        selftest()
    elif args.cmd == "ping-demo":
        ping_demo()
    else:
        fake_c6()


if __name__ == "__main__":
    main()
