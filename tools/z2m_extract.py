#!/usr/bin/env python3
"""
z2m_extract.py - Build a fingerprint from a Zigbee2MQTT database.db entry.

Zigbee2MQTT stores one JSON object per line in data/database.db, including the
interview result (endpoints, clusters, Basic attributes). Use this when a device
(e.g. the WA104) was interviewed by a SEPARATE Zigbee2MQTT coordinator on a
bench network - never by pairing it away from the live Wiser system.

Usage
  py tools\\z2m_extract.py path\\to\\database.db --ieee 0x000d6f00XXXXXXXX --label wa104 --out fingerprints\\wa104.captured.json
  py tools\\z2m_extract.py path\\to\\database.db --list

The IEEE address is only used to select the record; it is not written out.
Field names follow Zigbee2MQTT's database format as observed in practice; if a
field is missing the fingerprint keeps null and stays a template.
"""
import argparse
import datetime as dt
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_fingerprint as wf  # noqa: E402

BASIC_ATTR = {
    "zclVersion": "zcl_version", "appVersion": "application_version", "stackVersion": "stack_version",
    "hwVersion": "hw_version", "manufacturerName": "manufacturer_name", "modelId": "model_identifier",
    "dateCode": "date_code", "powerSource": "power_source", "swBuildId": "sw_build_id",
}


def hx(v: int) -> str:
    return f"0x{int(v):04x}"


def read_db(path: str) -> list[dict]:
    recs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                recs.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return recs


def convert(rec: dict, label: str, source: str) -> dict:
    fp = wf.empty_fingerprint(label)
    fp["node"]["manufacturer_code"] = hx(rec["manufId"]) if rec.get("manufId") is not None else None
    t = rec.get("type")
    fp["node"]["logical_type"] = {"Coordinator": "ZC", "Router": "ZR", "EndDevice": "ZED"}.get(t)
    if rec.get("manufName") is not None:
        fp["basic"]["manufacturer_name"] = rec["manufName"]
    if rec.get("modelId") is not None:
        fp["basic"]["model_identifier"] = rec["modelId"]
    for k in ("zclVersion", "appVersion", "stackVersion", "hwVersion", "dateCode", "swBuildId"):
        if rec.get(k) is not None:
            fp["basic"][BASIC_ATTR[k]] = rec[k]
    ps = rec.get("powerSource")
    if isinstance(ps, int):
        fp["basic"]["power_source"] = ps
    for ep_id, ep in sorted((rec.get("endpoints") or {}).items(), key=lambda kv: int(kv[0])):
        fp["endpoints"].append({
            "endpoint": int(ep.get("epId", ep_id)),
            "profile_id": hx(ep["profId"]) if ep.get("profId") is not None else None,
            "device_id": hx(ep["devId"]) if ep.get("devId") is not None else None,
            "device_version": None,
            "input_clusters": [hx(c) for c in ep.get("inClusterList", [])],
            "output_clusters": [hx(c) for c in ep.get("outClusterList", [])],
        })
    fp["proprietary_clusters"] = wf.proprietary_clusters(fp)
    fp["evidence"].append({"method": "zigbee2mqtt-database", "reference": source,
                           "note": f"interviewCompleted={rec.get('interviewCompleted')}"})
    complete = all(fp[s].get(f) not in (None, "") for s, f in wf.REQUIRED_FIELDS) and fp["endpoints"]
    fp["status"] = "captured" if complete else "template"
    return fp


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db")
    ap.add_argument("--ieee", help="IEEE address of the device record to extract")
    ap.add_argument("--label", default="device")
    ap.add_argument("--out")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    recs = read_db(args.db)
    if args.list or not args.ieee:
        for r in recs:
            print(f"{r.get('ieeeAddr')}  {r.get('type')}  {r.get('manufName')!r}  {r.get('modelId')!r}")
        return 0
    want = args.ieee.lower().replace(":", "")
    if not want.startswith("0x"):
        want = "0x" + want
    rec = next((r for r in recs if str(r.get("ieeeAddr", "")).lower() == want), None)
    if rec is None:
        print(f"No record with ieeeAddr {want}", file=sys.stderr)
        return 1
    fp = convert(rec, args.label, f"{os.path.basename(args.db)} @ {dt.date.today().isoformat()}")
    if args.out:
        wf.dump(fp, args.out)
        print(f"Wrote {args.out} (status: {fp['status']})")
    else:
        print(json.dumps(fp, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
