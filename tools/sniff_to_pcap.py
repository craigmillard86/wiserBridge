#!/usr/bin/env python3
"""
sniff_to_pcap.py - Convert the WiserBridge sniffer stream into a Wireshark pcap.

Requires the separate sniffer firmware (sniffer/) flashed to the C6.

Usage (Windows, use cmd.exe - PowerShell 5 pipes corrupt binary data)
  py tools\\sniff_to_pcap.py --port COM8 --channel 15 --out captures\\wiser_ch15.pcap
  py tools\\sniff_to_pcap.py --port COM8 --channel 15 --stdout | "C:\\Program Files\\Wireshark\\Wireshark.exe" -k -i -
  py tools\\sniff_to_pcap.py --from-text saved_stream.txt --out replay.pcap

Link types
  nofcs (default): LINKTYPE_IEEE802_15_4_NOFCS (230). The ESP32-C6 checks the FCS in
                   hardware and replaces it with RSSI/LQI, so frames carry no FCS.
  tap:             LINKTYPE_IEEE802_15_4_TAP (283) with channel, RSSI and LQI TLVs,
                   shown by Wireshark in the frame details.
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from typing import BinaryIO, Iterable, Optional

LINKTYPE_NOFCS = 230
LINKTYPE_TAP = 283


def pcap_global_header(linktype: int) -> bytes:
    return struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, linktype)


def tap_header(channel: int, rssi: int, lqi: int) -> bytes:
    tlvs = b""
    tlvs += struct.pack("<HHB3x", 0, 1, 0)                  # FCS type: 0 = none
    tlvs += struct.pack("<HHf", 1, 4, float(rssi))          # RSS (dBm, float32)
    tlvs += struct.pack("<HHHBx", 3, 3, channel, 0)         # channel assignment: channel, page 0
    tlvs += struct.pack("<HHB3x", 10, 1, lqi)               # LQI
    return struct.pack("<BBH", 0, 0, 4 + len(tlvs)) + tlvs


def pcap_record(payload: bytes, t: float) -> bytes:
    sec = int(t)
    usec = int(round((t - sec) * 1_000_000))
    if usec >= 1_000_000:
        sec, usec = sec + 1, usec - 1_000_000
    return struct.pack("<IIII", sec, usec, len(payload), len(payload)) + payload


def parse_line(line: str) -> Optional[tuple[int, int, int, int, bytes]]:
    """Parse 'F <ch> <rssi> <lqi> <ts_us> <hex>' -> (ch, rssi, lqi, ts, frame) or None."""
    parts = line.strip().split()
    if len(parts) != 6 or parts[0] != "F":
        return None
    try:
        ch, rssi, lqi, ts = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
        frame = bytes.fromhex(parts[5])
    except ValueError:
        return None
    if not frame or len(frame) > 127:
        return None
    return ch, rssi, lqi, ts, frame


class PcapWriter:
    def __init__(self, out: BinaryIO, linktype: int):
        self.out = out
        self.linktype = linktype
        self.count = 0
        out.write(pcap_global_header(linktype))
        out.flush()

    def write(self, ch: int, rssi: int, lqi: int, frame: bytes, t: float) -> None:
        payload = tap_header(ch, rssi, lqi) + frame if self.linktype == LINKTYPE_TAP else frame
        self.out.write(pcap_record(payload, t))
        self.out.flush()
        self.count += 1


def convert_lines(lines: Iterable[str], writer: PcapWriter, t0: Optional[float] = None) -> int:
    """Offline conversion: timestamps derived from the radio timestamp (us)."""
    base_ts = None
    t0 = time.time() if t0 is None else t0
    for line in lines:
        rec = parse_line(line)
        if rec is None:
            continue
        ch, rssi, lqi, ts, frame = rec
        if base_ts is None:
            base_ts = ts
        writer.write(ch, rssi, lqi, frame, t0 + (ts - base_ts) / 1e6)
    return writer.count


def live(args, writer: PcapWriter) -> int:
    import serial  # pyserial

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, 115200, 0.5
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.write(f"c {args.channel}\n".encode())
    ser.flush()
    stop_at = time.time() + args.duration if args.duration else None
    buf = b""
    try:
        while stop_at is None or time.time() < stop_at:
            buf += ser.read(ser.in_waiting or 1)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", errors="replace").strip()
                if line.startswith("#"):
                    print(line, file=sys.stderr)
                    continue
                rec = parse_line(line)
                if rec:
                    ch, rssi, lqi, _ts, frame = rec
                    writer.write(ch, rssi, lqi, frame, time.time())
                    if args.out and writer.count % 50 == 0:
                        print(f"{writer.count} frames", file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return writer.count


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--channel", type=int, default=11, choices=range(11, 27), metavar="11..26")
    ap.add_argument("--out", help="pcap file to write")
    ap.add_argument("--stdout", action="store_true", help="stream pcap to stdout (pipe into Wireshark -k -i -)")
    ap.add_argument("--linktype", choices=["nofcs", "tap"], default="nofcs")
    ap.add_argument("--duration", type=float, default=0, help="seconds (0 = until Ctrl+C)")
    ap.add_argument("--from-text", help="convert a saved text stream instead of reading a port")
    args = ap.parse_args()

    if not args.out and not args.stdout:
        ap.error("choose --out FILE or --stdout")
    lt = LINKTYPE_TAP if args.linktype == "tap" else LINKTYPE_NOFCS
    out = sys.stdout.buffer if args.stdout else open(args.out, "wb")
    writer = PcapWriter(out, lt)
    if args.from_text:
        with open(args.from_text, encoding="ascii", errors="replace") as f:
            n = convert_lines(f, writer)
    else:
        if not args.port:
            ap.error("--port is required unless --from-text is used")
        n = live(args, writer)
    if not args.stdout:
        out.close()
        print(f"wrote {n} frames to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
