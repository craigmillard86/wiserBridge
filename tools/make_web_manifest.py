#!/usr/bin/env python3
"""Copy an ESP-IDF build's flash parts into a folder and write an ESP Web Tools manifest.

    tools/make_web_manifest.py <build_dir> <out_dir> "<name>" <version>

Reads <build_dir>/flash_args (written by idf.py build), so the offsets always match the
partition table. Flashes the parts separately instead of one merged image: a merged image
pads the gap before the app with 0xFF and would wipe NVS (saved profile, EPID) on updates.
"""
import json
import os
import shutil
import sys


def main(build_dir, out_dir, name, version):
    os.makedirs(out_dir, exist_ok=True)
    parts = []
    with open(os.path.join(build_dir, "flash_args")) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("-"):
                continue
            offset, rel = line.split(None, 1)
            fname = os.path.basename(rel)
            shutil.copy(os.path.join(build_dir, rel), os.path.join(out_dir, fname))
            parts.append({"path": fname, "offset": int(offset, 16)})
    if not parts:
        sys.exit(f"no flash parts found in {build_dir}/flash_args")
    parts.sort(key=lambda p: p["offset"])
    manifest = {
        "name": name,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-C6", "parts": parts}],
    }
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"{out_dir}: " + ", ".join(f"{p['path']}@0x{p['offset']:x}" for p in parts))


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
