#!/usr/bin/env python3
"""
wb_monitor.py - Serial monitor / command runner / long-run logger for WiserBridge.

Works on Windows (COMx) and Linux (/dev/ttyACMx). Requires pyserial
(installed alongside esptool: `py -m pip install pyserial`).

Examples
  # Reset the board, capture 15 s of boot log, then run two commands
  py tools\\wb_monitor.py --port COM8 --reset --duration 15 --cmd status --cmd network

  # Long-running stability log (Ctrl+C to stop); every line is timestamped
  py tools\\wb_monitor.py --port COM8 --log logs\\soak.log --duration 0 --every 600 --cmd health

  # Then summarise it
  py tools\\wb_health_report.py logs\\soak.log
"""
import argparse
import datetime as dt
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: py -m pip install pyserial")


def ts() -> str:
    return dt.datetime.now().isoformat(timespec="milliseconds")


def reset_board(ser: "serial.Serial") -> None:
    """Hard reset through RTS (works for the ESP32-C6 USB-Serial/JTAG port)."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    ser.dtr = False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="e.g. COM8 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by USB-Serial/JTAG but required by pyserial")
    ap.add_argument("--reset", action="store_true", help="hard-reset the board before capturing")
    ap.add_argument("--duration", type=float, default=20.0, help="seconds to capture (0 = until Ctrl+C)")
    ap.add_argument("--cmd", action="append", default=[], help="command to send (repeatable)")
    ap.add_argument("--cmd-delay", type=float, default=8.0, help="seconds to wait before sending the first command")
    ap.add_argument("--every", type=float, default=0.0, help="resend the commands every N seconds (long-run mode)")
    ap.add_argument("--log", help="append timestamped output to this file")
    args = ap.parse_args()

    logf = open(args.log, "a", encoding="utf-8") if args.log else None
    # Configure DTR/RTS *before* opening so that attaching the logger does not
    # reset a running repeater (important for soak tests).
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    if args.reset:
        reset_board(ser)

    start = time.time()
    next_cmd = start + args.cmd_delay if args.cmd else None
    buf = b""

    def emit(line: str) -> None:
        stamped = f"{ts()} {line}"
        print(stamped, flush=True)
        if logf:
            logf.write(stamped + "\n")
            logf.flush()

    try:
        while True:
            now = time.time()
            if args.duration and now - start > args.duration:
                break
            if next_cmd is not None and now >= next_cmd:
                for c in args.cmd:
                    emit(f">>> {c}")
                    ser.write((c + "\r\n").encode())
                    ser.flush()
                    time.sleep(1.5)
                    chunk = ser.read(ser.in_waiting or 1)
                    buf += chunk
                next_cmd = (now + args.every) if args.every else None
            chunk = ser.read(ser.in_waiting or 1)
            if chunk:
                buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                # strip ANSI colour codes from ESP_LOG
                while "\x1b[" in text:
                    i = text.index("\x1b[")
                    j = text.find("m", i)
                    if j < 0:
                        break
                    text = text[:i] + text[j + 1:]
                emit(text)
    except KeyboardInterrupt:
        pass
    finally:
        if buf:
            emit(buf.decode("utf-8", errors="replace"))
        ser.close()
        if logf:
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
