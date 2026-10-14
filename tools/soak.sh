#!/usr/bin/env bash
# Soak test: build (build-soak/), flash over USB, run the ~35 min realistic workload, report.
# Needs the phone hotspot on (real downloads are part of the workload) and hands off the screen.
#   tools/soak.sh            then re-flash normal firmware: idf.py -p /dev/ttyACM0 flash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./env.sh
rm -f build-soak/sdkconfig   # regenerate: must match the product's sdkconfig.defaults
idf.py -B build-soak -DSDKCONFIG=build-soak/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.soak" build > /dev/null
idf.py -B build-soak -p "${PORT:-/dev/ttyACM0}" flash > /dev/null
log="build-soak/soak.log"
uvx --with pyserial --with pillow python tools/devcap.py --dumps 0 --seconds "${SECONDS_MAX:-2700}" > "$log" 2>&1 || true
grep -E "soak:|Guru|assert failed|stack overflow|abort\(\)|rst:0x" "$log" | grep -v "ui cycle" | cut -c1-200 || true
boots=$(grep -c "rst:0x" "$log" || true)
[ "$boots" -le 1 ] || echo "board restarted $boots times during the soak"
grep -q "soak: RESULT: PASS" "$log" && [ "$boots" -le 1 ] && { echo "SOAK PASSED (log: $log)"; exit 0; }
echo "SOAK FAILED (log: $log)"; exit 1
