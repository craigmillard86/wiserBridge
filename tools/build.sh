#!/usr/bin/env bash
# Reproducible build of the router and sniffer firmware using the official
# Espressif Docker image, plus single-file release images (flash at 0x0).
#
#   tools/build.sh            # router + sniffer + release/ images
#   IDF_IMAGE=espressif/idf:v5.5.6 tools/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.6}"
VER="$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)"

run() { docker run --rm -v "$PWD":/project -w "/project/$1" "$IDF_IMAGE" bash -c "$2"; }

echo "== router firmware (${IDF_IMAGE}) =="
run . "idf.py set-target esp32c6 >/dev/null && idf.py build"
echo "== sniffer firmware =="
run sniffer "idf.py set-target esp32c6 >/dev/null && idf.py build"

mkdir -p release
echo "== merged release images =="
run build "esptool.py --chip esp32c6 merge_bin -o /project/release/wiserbridge-router-${VER}-xiao-esp32c6.bin @flash_args"
run sniffer/build "esptool.py --chip esp32c6 merge_bin -o /project/release/wiserbridge-sniffer-${VER}-xiao-esp32c6.bin @flash_args"
(cd release && sha256sum ./*.bin > SHA256SUMS.txt && cat SHA256SUMS.txt)
echo "Flash a release image with:  py -m esptool --chip esp32c6 -p COMx write-flash 0x0 release/<file>.bin"
