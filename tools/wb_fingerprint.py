#!/usr/bin/env python3
"""
wb_fingerprint.py - Zigbee device fingerprint helpers (shared by the CLI tools).

A fingerprint records what a real device reported about itself (descriptors,
clusters, Basic attributes) together with WHERE that evidence came from. The
firmware compatibility profile may only be generated from a fingerprint whose
status is "captured" and whose required fields are all populated - template
placeholders (null) are rejected, so guessed values cannot slip through.

Unique identity and secrets (IEEE address, install code, link/network keys) are
never part of a fingerprint and are rejected if present.
"""
from __future__ import annotations

import json
import re
from typing import Any

SCHEMA_VERSION = 1
HEX16 = re.compile(r"^0x[0-9a-fA-F]{4}$")
HEX8 = re.compile(r"^0x[0-9a-fA-F]{2}$")
FORBIDDEN_KEYS = {
    "ieee", "ieee_addr", "ieee_address", "eui64", "install_code", "installcode",
    "network_key", "link_key", "tc_link_key", "nwk_key", "key",
}
REQUIRED_FIELDS = [
    ("basic", "manufacturer_name"),
    ("basic", "model_identifier"),
    ("node", "manufacturer_code"),
    ("node", "logical_type"),
]


def empty_fingerprint(name: str = "wa104") -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "status": "template",
        "device_label": name,
        "evidence": [],
        "node": {
            "logical_type": None,
            "manufacturer_code": None,
            "mac_capability_flags": None,
            "server_mask": None,
            "stack_compliance_revision": None,
            "max_buffer_size": None,
            "max_incoming_transfer_size": None,
            "max_outgoing_transfer_size": None,
        },
        "basic": {
            "zcl_version": None,
            "application_version": None,
            "stack_version": None,
            "hw_version": None,
            "manufacturer_name": None,
            "model_identifier": None,
            "date_code": None,
            "power_source": None,
            "sw_build_id": None,
        },
        "endpoints": [],
        "proprietary_clusters": [],
        "notes": "",
    }


def _walk_keys(obj: Any, path: str = ""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield f"{path}.{k}" if path else k, k
            yield from _walk_keys(v, f"{path}.{k}" if path else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from _walk_keys(v, f"{path}[{i}]")


def validate(fp: dict[str, Any], require_captured: bool = False) -> list[str]:
    """Return a list of problems (empty list = valid)."""
    errs: list[str] = []
    if fp.get("schema_version") != SCHEMA_VERSION:
        errs.append(f"schema_version must be {SCHEMA_VERSION}")
    if fp.get("status") not in ("template", "captured"):
        errs.append("status must be 'template' or 'captured'")
    for path, key in _walk_keys(fp):
        if key.lower() in FORBIDDEN_KEYS:
            errs.append(f"forbidden identity/secret field present: {path} (never store IEEE addresses or keys)")
    for ep in fp.get("endpoints", []):
        if not isinstance(ep.get("endpoint"), int) or not (1 <= ep["endpoint"] <= 240 or ep["endpoint"] == 242):  # 242 = Green Power
            errs.append(f"endpoint number invalid: {ep.get('endpoint')!r}")
        for f in ("profile_id", "device_id"):
            if ep.get(f) is not None and not HEX16.match(str(ep[f])):
                errs.append(f"endpoint {ep.get('endpoint')}: {f} must look like 0x0104")
        for f in ("input_clusters", "output_clusters"):
            for c in ep.get(f, []):
                if not HEX16.match(str(c)):
                    errs.append(f"endpoint {ep.get('endpoint')}: cluster {c!r} must look like 0x0000")
    mc = fp.get("node", {}).get("manufacturer_code")
    if mc is not None and not HEX16.match(str(mc)):
        errs.append("node.manufacturer_code must look like 0x1234")
    if fp.get("status") == "captured" or require_captured:
        if fp.get("status") != "captured":
            errs.append("status is not 'captured' - a template cannot be used to build a profile")
        if not fp.get("evidence"):
            errs.append("captured fingerprint must list at least one evidence entry")
        for sect, field in REQUIRED_FIELDS:
            if fp.get(sect, {}).get(field) in (None, ""):
                errs.append(f"required field missing (null): {sect}.{field}")
        if not fp.get("endpoints"):
            errs.append("captured fingerprint must list endpoints")
    return errs


def from_interview(record: dict[str, Any], method: str = "wb-interview", reference: str = "") -> dict[str, Any]:
    """Convert a FINGERPRINT-JSON record printed by the firmware 'interview' command."""
    fp = empty_fingerprint(record.get("device_label", "device"))
    nd = record.get("node_descriptor") or {}
    for k in fp["node"]:
        if k in nd:
            fp["node"][k] = nd[k]
    attr_map = {
        "0x0000": "zcl_version", "0x0001": "application_version", "0x0002": "stack_version",
        "0x0003": "hw_version", "0x0004": "manufacturer_name", "0x0005": "model_identifier",
        "0x0006": "date_code", "0x0007": "power_source", "0x4000": "sw_build_id",
    }
    ba = record.get("basic_attributes")
    if isinstance(ba, dict):
        for aid, val in ba.items():
            key = attr_map.get(aid.lower())
            if key:
                fp["basic"][key] = val
    for ep in record.get("endpoints", []):
        fp["endpoints"].append({
            "endpoint": ep["endpoint"],
            "profile_id": ep.get("profile_id"),
            "device_id": ep.get("device_id"),
            "device_version": ep.get("device_version"),
            "input_clusters": ep.get("input_clusters", []),
            "output_clusters": ep.get("output_clusters", []),
        })
    fp["proprietary_clusters"] = proprietary_clusters(fp)
    fp["evidence"].append({"method": method, "reference": reference,
                           "note": record.get("note", "")})
    complete = all(fp[s].get(f) not in (None, "") for s, f in REQUIRED_FIELDS) and fp["endpoints"]
    fp["status"] = "captured" if complete else "template"
    return fp


def proprietary_clusters(fp: dict[str, Any]) -> list[str]:
    """Manufacturer-specific cluster IDs (0xFC00-0xFFFF) seen on any endpoint."""
    out = set()
    for ep in fp.get("endpoints", []):
        for c in ep.get("input_clusters", []) + ep.get("output_clusters", []):
            if int(str(c), 16) >= 0xFC00:
                out.add(str(c).lower())
    return sorted(out)


def to_cli_commands(fp: dict[str, Any], endpoint: int | None = None) -> list[str]:
    """Build firmware 'profile set' commands. Raises ValueError for templates/invalid data."""
    errs = validate(fp, require_captured=True)
    if errs:
        raise ValueError("fingerprint not usable:\n  - " + "\n  - ".join(errs))
    eps = fp["endpoints"]
    if endpoint is None:
        chosen = next((e for e in eps if "0x0000" in [c.lower() for c in e.get("input_clusters", [])]), eps[0])
    else:
        chosen = next((e for e in eps if e["endpoint"] == endpoint), None)
        if chosen is None:
            raise ValueError(f"endpoint {endpoint} not in fingerprint")
    b, n = fp["basic"], fp["node"]
    ins = [c.lower() for c in chosen.get("input_clusters", []) if c.lower() not in ("0x0000", "0x0003")]
    outs = [c.lower() for c in chosen.get("output_clusters", [])]
    if len(ins) > 8 or len(outs) > 8:
        raise ValueError("firmware supports at most 8 extra input and 8 output clusters")
    cmds = [
        f"profile set manufacturer {b['manufacturer_name']}",
        f"profile set model {b['model_identifier']}",
        f"profile set endpoint {chosen['endpoint']}",
    ]
    if chosen.get("profile_id"):
        cmds.append(f"profile set profile_id {chosen['profile_id']}")
    if chosen.get("device_id"):
        cmds.append(f"profile set device_id {chosen['device_id']}")
    if chosen.get("device_version") is not None:
        cmds.append(f"profile set device_version {chosen['device_version']}")
    cmds.append(f"profile set manufacturer_code {n['manufacturer_code']}")
    has_identify = "0x0003" in [c.lower() for c in chosen.get("input_clusters", [])]
    cmds.append(f"profile set identify {'on' if has_identify else 'off'}")
    if b.get("power_source") is not None:
        cmds.append(f"profile set power_source {int(b['power_source'])}")
    if b.get("date_code"):
        cmds.append(f"profile set date_code {str(b['date_code'])[:16]}")
    cmds.append(f"profile set in_clusters {','.join(ins) if ins else 'none'}")
    cmds.append(f"profile set out_clusters {','.join(outs) if outs else 'none'}")
    label = fp.get("device_label", "fingerprint")[:20]
    cmds.append(f"profile set source fp:{label}")
    cmds.append("profile pending")
    return cmds


def load(path: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def dump(fp: dict[str, Any], path: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(fp, f, indent=2)
        f.write("\n")
