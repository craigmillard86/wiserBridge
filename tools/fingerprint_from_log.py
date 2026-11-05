#!/usr/bin/env python3
"""
fingerprint_from_log.py - Turn the firmware's 'interview' output into a fingerprint file.

Usage
  py tools\\fingerprint_from_log.py logs\\interview.log --label wa104 --out fingerprints\\wa104.captured.json

The log must contain a line beginning with 'FINGERPRINT-JSON ' (as printed by the
'interview <addr>' command; wb_monitor.py timestamps are tolerated). The IEEE
address in that line is deliberately dropped.
"""
import argparse
import datetime as dt
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_fingerprint as wf  # noqa: E402


def extract_records(text: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        i = line.find("FINGERPRINT-JSON ")
        if i < 0:
            continue
        payload = line[i + len("FINGERPRINT-JSON "):].strip()
        try:
            out.append(json.loads(payload))
        except json.JSONDecodeError as e:
            print(f"warning: could not parse a FINGERPRINT-JSON line: {e}", file=sys.stderr)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--label", default="device")
    ap.add_argument("--out", required=True)
    ap.add_argument("--index", type=int, default=-1, help="which record to use if several (default: last)")
    args = ap.parse_args()

    with open(args.log, encoding="utf-8", errors="replace") as f:
        recs = extract_records(f.read())
    if not recs:
        print("No FINGERPRINT-JSON line found. Run 'interview <addr>' on the router first.", file=sys.stderr)
        return 1
    rec = recs[args.index]
    rec.pop("ieee_addr_do_not_copy", None)
    rec["device_label"] = args.label
    fp = wf.from_interview(rec, method="wb-interview",
                           reference=f"{os.path.basename(args.log)} @ {dt.date.today().isoformat()}")
    errs = wf.validate(fp)
    wf.dump(fp, args.out)
    print(f"Wrote {args.out} (status: {fp['status']})")
    if fp["status"] != "captured":
        print("Some required fields are missing; status left as 'template'. Fill them only from real evidence.")
    for e in errs:
        print(f"  problem: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
