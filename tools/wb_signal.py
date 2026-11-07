#!/usr/bin/env python3
"""Live placement helper: ping the Wiser hub from the WiserBridge router every few seconds
and show the signal strength (RSSI) of the hub's replies, so you can move/rotate the board
and watch the link improve or degrade.

    py tools\\wb_signal.py COM8            (Ctrl+C to stop)

Note: opening the USB serial port restarts the ESP32-C6 once (it keeps its network
membership). RSSI shown is for the last radio hop of frames from the hub; if the hub's
traffic is being relayed by another router, the value reflects that router instead.
"""
import re
import sys
import time
from collections import deque

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
EVERY = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 0  # seconds, 0 = until Ctrl+C

APS_HUB = re.compile(r"APS-IN 0x0000 ")
RSSI = re.compile(r"WB_APSIN:.*rssi=(-?\d+)")
PING = re.compile(r"PING 0x0000: (\d+)/(\d+) replies(?:, rtt min/avg/max (\d+)/(\d+)/(\d+) ms)?")


def verdict(avg):
    if avg is None:
        return "no frames"
    if avg >= -80:
        return "GOOD"
    if avg >= -90:
        return "OK"
    if avg >= -95:
        return "MARGINAL"
    return "POOR"


def bar(rssi):
    # -105 dBm -> 0 blocks, -60 dBm -> 30 blocks
    n = max(0, min(30, int((rssi + 105) * 30 / 45)))
    return "#" * n + "." * (30 - n)


def main():
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = PORT, 115200, 0.1
    ser.dtr = ser.rts = False
    ser.open()
    print(f"Opening {PORT} (the board restarts once)... waiting 10 s for it to rejoin.")
    t_end = time.time() + 10
    while time.time() < t_end:
        ser.read(ser.in_waiting or 1)
    ser.write(b"apslog on\r\n")

    window = deque()          # (timestamp, rssi) for the last 30 s
    buf = ""
    pending_hub = False
    next_ping = time.time()
    sent = ok = 0
    print("time      ping        now    avg30s  min30s  signal                          verdict")
    try:
        t_stop = time.time() + DURATION if DURATION else None
        while True:
            now = time.time()
            if t_stop and now > t_stop:
                break
            if now >= next_ping:
                ser.write(b"ping 0x0000 1\r\n")
                sent += 1
                next_ping = now + EVERY
            chunk = ser.read(ser.in_waiting or 1).decode("utf-8", errors="replace")
            if not chunk:
                continue
            buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if APS_HUB.search(line):
                    pending_hub = True
                    continue
                m = RSSI.search(line)
                if m and pending_hub:
                    window.append((time.time(), int(m.group(1))))
                    pending_hub = False
                    continue
                m = PING.search(line)
                if m:
                    got = int(m.group(1))
                    ok += got
                    while window and window[0][0] < time.time() - 30:
                        window.popleft()
                    vals = [r for _, r in window]
                    last = vals[-1] if vals else None
                    avg = sum(vals) / len(vals) if vals else None
                    mn = min(vals) if vals else None
                    p = f"OK {m.group(4)}ms" if got else "LOST"
                    if last is None:
                        print(f"{time.strftime('%H:%M:%S')}  {p:<10}  (no hub frames yet)")
                    else:
                        print(f"{time.strftime('%H:%M:%S')}  {p:<10}  {last:4d}  {avg:6.0f}  {mn:6d}   "
                              f"{bar(avg)}  {verdict(avg)}")
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print(f"\nstopped: {ok}/{sent} pings answered")


if __name__ == "__main__":
    main()
