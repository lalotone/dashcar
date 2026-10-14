#!/usr/bin/env bash
# One-time setup of the build environment: ESP-IDF (pinned version) + its toolchains and Python venv.
#   tools/setup.sh            then: . ./env.sh && idf.py build
# The LVGL/display/touch components are not installed here: idf.py downloads them on the first build,
# at the exact versions pinned in dependencies.lock.
#
# Env overrides: IDF_PATH (default ~/esp/esp-idf), IDF_VERSION (default v5.5.5), PYTHON (interpreter to use).
set -euo pipefail

IDF_VERSION="${IDF_VERSION:-v5.5.5}"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

need() { command -v "$1" > /dev/null || { echo "Missing '$1'. $2" >&2; exit 1; }; }
need git    "Install git."
need cmake  "Install cmake (Fedora: dnf install cmake; Debian/Ubuntu: apt install cmake)."
need ninja  "Install ninja (Fedora: dnf install ninja-build; Debian/Ubuntu: apt install ninja-build)."

# ESP-IDF 5.5 supports Python 3.9-3.13; 3.14 (e.g. Fedora 43's default) is too new.
if [ -z "${PYTHON:-}" ]; then
    for p in python3.13 python3.12 python3.11 python3.10 python3.9; do
        if command -v "$p" > /dev/null; then PYTHON="$(command -v "$p")"; break; fi
    done
fi
[ -n "${PYTHON:-}" ] || { echo "Need Python 3.9-3.13 (set PYTHON=/path/to/python3.x)." >&2; exit 1; }
echo "Using $PYTHON ($("$PYTHON" --version))"

if [ -d "$IDF_PATH/.git" ]; then
    have="$(git -C "$IDF_PATH" describe --tags 2> /dev/null || echo unknown)"
    if [ "$have" != "$IDF_VERSION" ]; then
        echo "$IDF_PATH has ESP-IDF $have, expected $IDF_VERSION. Move it away or set IDF_PATH." >&2
        exit 1
    fi
    echo "ESP-IDF $IDF_VERSION already at $IDF_PATH"
else
    mkdir -p "$(dirname "$IDF_PATH")"
    git clone --depth 1 --branch "$IDF_VERSION" --recursive --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi

# install.sh/export.sh pick "python3" from PATH, so put the chosen interpreter first.
shim="$(mktemp -d)"
trap 'rm -rf "$shim"' EXIT
ln -s "$PYTHON" "$shim/python3"
ln -s "$PYTHON" "$shim/python"
PATH="$shim:$PATH" "$IDF_PATH/install.sh" esp32s3

cat << EOF

Done. Build and flash:
  . ./env.sh
  idf.py build
  idf.py -p /dev/ttyACM0 flash      # your user needs access to the serial port (group dialout/uucp)

The helper scripts in tools/ (devcap, selftest, ota) run their Python through uv: https://docs.astral.sh/uv/
EOF
