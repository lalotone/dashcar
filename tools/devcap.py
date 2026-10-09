#!/usr/bin/env python3
"""Reset the board, stream its console, and turn DUMP blocks (from dbg_screenshot()) into PNGs.

    uvx --with pyserial --with pillow python tools/devcap.py [--dumps N] [--seconds S]
    uvx --with pyserial --with pillow --with opencv-python-headless --with numpy \
        python tools/devcap.py --qr          # also decode QR codes in the screenshots

Log lines are printed as they arrive; screenshots go to --out (default: /tmp/devcap/<tag>.png),
scaled back to full size when they were dumped at half resolution.
"""
import argparse
import pathlib
import re
import sys
import time

import serial
from PIL import Image

HEX = re.compile(r"^[0-9a-f]+$")


def rgb565_rows_to_image(rows, w, step):
    im = Image.new("RGB", (w, len(rows)))
    px = im.load()
    for y, r in enumerate(rows):
        for x in range(w):
            v = int(r[x * 4:x * 4 + 4], 16)
            px[x, y] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    return im.resize((w * step, len(rows) * step), Image.NEAREST) if step > 1 else im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=90, help="give up after this long")
    ap.add_argument("--dumps", type=int, default=1, help="stop after this many screenshots (0 = just stream logs)")
    ap.add_argument("--out", default="/tmp/devcap")
    ap.add_argument("--no-reset", action="store_true", help="don't reset the chip first")
    ap.add_argument("--qr", action="store_true", help="decode QR codes (needs opencv + numpy)")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    s = serial.Serial(args.port, 115200, timeout=0.2)
    if not args.no_reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False

    end = time.time() + args.seconds
    pending = b""
    dump = None  # [tag, w, h, step, rows]
    done = 0
    while time.time() < end and (args.dumps == 0 or done < args.dumps):
        pending += s.read(65536)
        *lines, pending = pending.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if dump is None:
                if line.startswith("DUMP "):
                    tag, w, h, step = line.split()[1:5]
                    dump = [tag, int(w), int(h), int(step), []]
                elif line:
                    print(line, flush=True)
                continue
            if line == "ENDDUMP":
                tag, w, h, step, rows = dump
                path = out / f"{tag}.png"
                img = rgb565_rows_to_image(rows, w, step)
                img.save(path)
                print(f"[devcap] {path}  ({w}x{len(rows)} of {h} rows, step {step})", flush=True)
                if args.qr:
                    import cv2
                    import numpy as np
                    text, _, _ = cv2.QRCodeDetector().detectAndDecode(cv2.cvtColor(np.array(img), cv2.COLOR_RGB2BGR))
                    print(f"[devcap] QR in {tag}: {text!r}", flush=True)
                dump = None
                done += 1
            elif len(line) == dump[1] * 4 and HEX.match(line):
                dump[4].append(line)
            # anything else inside a dump (interleaved log lines) is dropped
    if args.dumps and done < args.dumps:
        print(f"[devcap] timed out with {done}/{args.dumps} screenshots", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
