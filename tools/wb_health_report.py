#!/usr/bin/env python3
"""
wb_health_report.py - Summarise a long-run log captured with wb_monitor.py.

Usage
  py tools\\wb_health_report.py logs\\soak.log [--min-hours 24]

Reports boots (and reset reasons), time covered, how often the repeater was
joined, router-neighbour gaps, children seen, heap trend and counters, then
gives a PASS/FAIL style summary for milestone M6. It only reports what is in
the log - missing data is reported as missing.
"""
from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from dataclasses import dataclass, field

TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?)\s(.*)$")
HEALTH_RE = re.compile(r"HEALTH (.*)$")
KV_RE = re.compile(r"(\w+)=(\S+)")
RESET_RE = re.compile(r"Reset reason\s*:\s*(.+)$")


@dataclass
class Report:
    first: dt.datetime | None = None
    last: dt.datetime | None = None
    boots: list[tuple[str, str]] = field(default_factory=list)
    samples: int = 0
    joined_samples: int = 0
    isolated_samples: int = 0
    states: dict[str, int] = field(default_factory=dict)
    verdicts: dict[str, int] = field(default_factory=dict)
    max_children: int = 0
    max_routers: int = 0
    min_heap: int | None = None
    first_minheap: int | None = None
    last_minheap: int | None = None
    last_kv: dict = field(default_factory=dict)
    removed_events: int = 0
    leave_events: int = 0
    nwk_status_events: int = 0


def parse(lines) -> Report:
    r = Report()
    last_kv: dict = {}
    for raw in lines:
        m = TS_RE.match(raw.rstrip("\n"))
        if not m:
            continue
        t = dt.datetime.fromisoformat(m.group(1))
        msg = m.group(2)
        r.first = r.first or t
        r.last = t
        rm = RESET_RE.search(msg)
        if rm:
            r.boots.append((t.isoformat(timespec="seconds"), rm.group(1).strip()))
        if "REMOVED from network" in msg:
            r.removed_events += 1
        if "NLME-NWK-STATUS" in msg:
            r.nwk_status_events += 1
        hm = HEALTH_RE.search(msg)
        if not hm:
            continue
        kv = dict(KV_RE.findall(hm.group(1)))
        r.samples += 1
        st = kv.get("state", "?")
        r.states[st] = r.states.get(st, 0) + 1
        v = kv.get("verdict", "?")
        r.verdicts[v] = r.verdicts.get(v, 0) + 1
        if st == "JOINED":
            r.joined_samples += 1
            if kv.get("rtr", "0") == "0":
                r.isolated_samples += 1
        r.max_children = max(r.max_children, int(kv.get("child", 0)))
        r.max_routers = max(r.max_routers, int(kv.get("rtr", 0)))
        mh = int(kv.get("minheap", 0))
        r.min_heap = mh if r.min_heap is None else min(r.min_heap, mh)
        if r.first_minheap is None:
            r.first_minheap = mh
        r.last_minheap = mh
        last_kv = kv
    r.last_kv = last_kv
    return r


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--min-hours", type=float, default=24.0)
    args = ap.parse_args()
    with open(args.log, encoding="utf-8", errors="replace") as f:
        r = parse(f)
    if not r.first:
        print("No timestamped lines found (capture with tools/wb_monitor.py --log).")
        return 1
    hours = (r.last - r.first).total_seconds() / 3600
    print(f"Log span          : {r.first} -> {r.last} ({hours:.1f} h)")
    print(f"HEALTH samples    : {r.samples}")
    print(f"Boots seen        : {len(r.boots)}")
    for t, why in r.boots:
        print(f"   {t}  reset reason: {why}")
    if r.samples:
        print(f"Joined samples    : {r.joined_samples}/{r.samples} ({100 * r.joined_samples / r.samples:.1f}%)")
        print(f"Isolated samples  : {r.isolated_samples} (joined but 0 router neighbours)")
        print(f"States            : {r.states}")
        print(f"Verdicts          : {r.verdicts}")
        print(f"Max children      : {r.max_children}   max router neighbours: {r.max_routers}")
        print(f"Min free heap     : {r.min_heap}  (first minheap {r.first_minheap}, last {r.last_minheap})")
        lk = r.last_kv
        print(f"Last counters     : rejoins={lk.get('rejoins')} leaves={lk.get('leaves')} nlme={lk.get('nlme')} "
              f"noLinks={lk.get('noLinks')}")
    print(f"Removed-by-network: {r.removed_events}   NWK status lines: {r.nwk_status_events}")

    bad_boots = [b for b in r.boots if any(k in b[1] for k in ("PANIC", "WATCHDOG", "BROWNOUT"))]
    checks = [
        ("duration >= %.0f h" % args.min_hours, hours >= args.min_hours),
        ("no crash/watchdog/brownout resets", not bad_boots),
        ("joined in every sample", r.samples > 0 and r.joined_samples == r.samples),
        ("never removed by the network", r.removed_events == 0),
        ("min heap not trending to zero",
         r.first_minheap is not None and r.last_minheap is not None and r.last_minheap > 0.5 * r.first_minheap),
    ]
    print("\nM6 checks (log-based; physical checks such as TRV responsiveness are separate):")
    for name, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    return 0 if all(ok for _, ok in checks) else 2


if __name__ == "__main__":
    sys.exit(main())
