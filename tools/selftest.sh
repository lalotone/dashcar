#!/usr/bin/env bash
# Build the self-test firmware (separate build dir), flash it over USB, run it, report.
#   tools/selftest.sh            then re-flash normal firmware: idf.py -p /dev/ttyACM0 flash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./env.sh
# Regenerate the config every time: a stale build-selftest/sdkconfig ignores options added to
# sdkconfig.defaults later, and the test would run a different memory layout than the product.
rm -f build-selftest/sdkconfig
idf.py -B build-selftest -DSDKCONFIG=build-selftest/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.selftest" build > /dev/null
idf.py -B build-selftest -p "${PORT:-/dev/ttyACM0}" flash > /dev/null
log="build-selftest/selftest.log"
uvx --with pyserial --with pillow python tools/devcap.py --dumps 0 --seconds 330 > "$log" 2>&1 || true
grep -E "selftest:|Guru|assert failed|stack overflow|abort\(\)" "$log" | cut -c1-160 || true   # no matches must not abort the script (pipefail)
grep -q "selftest: RESULT: 0 failure" "$log" && { echo "SELFTEST PASSED (log: $log)"; exit 0; }
echo "SELFTEST FAILED (log: $log)"; exit 1
