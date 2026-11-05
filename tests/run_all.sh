#!/usr/bin/env bash
# Runs every automated (no-hardware) test: host C unit tests + Python tool tests.
# Firmware compilation is covered separately by tools/build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
echo "== host C unit tests (main/diag_logic.c) =="
make -s -C tests/host run
echo "== Python tool tests =="
python3 -m unittest discover -s tests/python -v
