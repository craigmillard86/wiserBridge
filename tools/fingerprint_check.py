#!/usr/bin/env python3
"""
fingerprint_check.py - Validate a fingerprint file and optionally emit firmware commands.

Usage
  py tools\\fingerprint_check.py fingerprints\\wa104.template.json
  py tools\\fingerprint_check.py fingerprints\\wa104.captured.json --emit-cli

--emit-cli prints 'profile set ...' commands for the router's serial console.
It refuses templates and incomplete fingerprints, so no guessed value can be
turned into a profile. Paste the commands, check 'profile pending', then
'profile save' and 'reboot'.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_fingerprint as wf  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--emit-cli", action="store_true")
    ap.add_argument("--endpoint", type=int, help="endpoint to mirror (default: the one with Basic server)")
    args = ap.parse_args()

    fp = wf.load(args.file)
    errs = wf.validate(fp)
    print(f"{args.file}: status={fp.get('status')} endpoints={len(fp.get('endpoints', []))} "
          f"proprietary={wf.proprietary_clusters(fp) or 'none'}")
    if errs:
        print("Problems:")
        for e in errs:
            print(f"  - {e}")
    if not args.emit_cli:
        return 1 if errs else 0
    try:
        cmds = wf.to_cli_commands(fp, args.endpoint)
    except ValueError as e:
        print(f"REFUSED: {e}", file=sys.stderr)
        return 2
    print("\n# WARNING: these commands make the repeater present the captured device's identity strings and")
    print("# manufacturer code. Extra clusters are exposed EMPTY (no attributes/commands behind them).")
    print("# The unique IEEE address is never copied. Apply, then: profile save, reboot, reset + re-join.\n")
    for c in cmds:
        print(c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
