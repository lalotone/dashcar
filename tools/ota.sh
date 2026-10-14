#!/usr/bin/env bash
# Upload build/dashcar.bin to the board over Wi-Fi (Settings > Wireless updates must be on,
# and this computer must be on the same network as the board).
#   tools/ota.sh [host]        host defaults to dashcar.local (mDNS); an IP works too
set -euo pipefail
host="${1:-dashcar.local}"
bin="$(dirname "$0")/../build/dashcar.bin"
[[ -f "$bin" ]] || { echo "no $bin: run idf.py build first" >&2; exit 1; }

echo "Board says: $(curl -fsS --max-time 5 "http://$host/" | head -1)" || {
    echo "Can't reach http://$host/ (same network? wireless updates on?)" >&2; exit 1; }
echo "Uploading $(du -h "$bin" | cut -f1) to $host ..."
curl -fS --progress-bar --max-time 300 -H "Content-Type: application/octet-stream" \
     --data-binary @"$bin" "http://$host/update"
echo "Done. The board restarts into the new firmware; it rolls back by itself if that crashes within 20 s."
