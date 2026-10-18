#!/usr/bin/env python3
"""Render the DashCar artwork: LVGL A8 (alpha-only) images and the splash emblem outline.

A8 is 1 byte/pixel and is tinted at runtime (image_recolor), so one bitmap serves any colour;
partial alpha gives the dimmer track and ticks. Images:
  app_logo_small    speedometer-gauge emblem (original artwork), small (home header)
Splash emblem, as flattened outlines rather than a bitmap (the splash rasterises it at boot into PSRAM:
no flash/PSRAM cost for a big A8 image). Same symbols in either file; main/CMakeLists.txt builds one:
  main/ui/emblem.c          the DashCar "DC" monogram: italic, the D's stem breaking into three speed
                            streaks (original artwork, in the repo)
  main/ui/emblem_private.c  with --dacia: the Dacia "Link" emblem (2021+, the "DC" on 2023+ cars), for
                            the owner's own build. Trademark of Dacia / Renault Group: git-ignored, never
                            committed. Used automatically when present (run `idf.py reconfigure` once).
Units are arbitrary (the splash scales to a pixel width); the Dacia one is split into halves at x = w/2.

Usage:  uvx --with cairosvg --with pillow --with shapely python tools/gen_logo.py [--dacia]
Writes: main/ui/app_logo.c, main/ui/emblem.c [, main/ui/emblem_private.c]
"""
import io
import json
import math
import pathlib
import sys

import cairosvg
from PIL import Image

# Geometry on a 200x200 canvas, angles in degrees clockwise from 3 o'clock (same as lv_arc).
R, W = 78, 15                 # dial radius (stroke centre) and stroke width
START, END, NEEDLE = 135, 405, 300
SMALL_PX = 30                  # home-header icon size


def pt(a, r, c=100):
    t = math.radians(a)
    return c + r * math.cos(t), c + r * math.sin(t)


def track(opacity):
    (x0, y0), (x1, y1) = pt(START, R), pt(END, R)
    return (f'<path d="M {x0:.2f} {y0:.2f} A {R} {R} 0 1 1 {x1:.2f} {y1:.2f}" fill="none" stroke="#fff" '
            f'stroke-opacity="{opacity}" stroke-width="{W}" stroke-linecap="round"/>')


def value_arc():
    (x0, y0), (xv, yv) = pt(START, R), pt(NEEDLE, R)
    return (f'<path d="M {x0:.2f} {y0:.2f} A {R} {R} 0 0 1 {xv:.2f} {yv:.2f}" fill="none" stroke="#fff" '
            f'stroke-width="{W}" stroke-linecap="round"/>')


def ticks():
    out = ""
    for a in range(START, END + 1, 45):
        (ax, ay), (bx, by) = pt(a, R - 22), pt(a, R - 15)
        out += (f'<line x1="{ax:.2f}" y1="{ay:.2f}" x2="{bx:.2f}" y2="{by:.2f}" stroke="#fff" '
                f'stroke-opacity="0.45" stroke-width="5" stroke-linecap="round"/>')
    return out


def needle(hub=True):
    tip, bl, br = pt(NEEDLE, 60), pt(NEEDLE + 90, 9), pt(NEEDLE - 90, 9)
    return (f'<polygon points="{bl[0]:.2f},{bl[1]:.2f} {tip[0]:.2f},{tip[1]:.2f} {br[0]:.2f},{br[1]:.2f}" '
            f'fill="#fff"/>' + ('<circle cx="100" cy="100" r="13" fill="#fff"/>' if hub else ''))


def a8(body, size, crop=None):
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">{body}</svg>'
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size, output_height=size)
    img = Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    if crop:
        o = (size - crop) // 2
        img = img.crop((o, o, o + crop, o + crop))
    return img.tobytes()


def c_image(name, data, w, h):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    body = ",\n    ".join(rows)
    return f"""
static const uint8_t {name}_map[] = {{
    {body}
}};

const lv_image_dsc_t {name} = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_A8,
        .w = {w},
        .h = {h},
        .stride = {w},
    }},
    .data_size = sizeof({name}_map),
    .data = {name}_map,
}};
"""


# Private emblem (--dacia): outline data in tools/emblem_private.json (git-ignored, never committed):
# {"w": .., "h": .., "path": [[x, y] | ["c", [c1x, c1y], [c2x, c2y], [x, y]], ...]}, clockwise; "c" = cubic Bezier.
PRIVATE_JSON = pathlib.Path(__file__).resolve().parent / "emblem_private.json"


def private_points(path, steps=4):
    pts = []
    for seg in path:
        if seg[0] == "c":
            (x0, y0), (c1x, c1y), (c2x, c2y), (x3, y3) = pts[-1], seg[1], seg[2], seg[3]
            for i in range(1, steps + 1):
                t = i / steps
                u = 1 - t
                pts.append((u ** 3 * x0 + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t ** 3 * x3,
                            u ** 3 * y0 + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t ** 3 * y3))
        else:
            pts.append(seg)
    return pts


def dc_monogram():
    """DashCar "DC": italic D and C; the D's stem is cut into three bars that run out to the left
    as speed streaks (longest in the middle). Returns the outlines (exteriors and holes)."""
    from shapely.affinity import skew, translate
    from shapely.geometry import Polygon, box
    from shapely.ops import unary_union

    def arc(cx, cy, r, a0, a1, n=32):
        return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                 cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]

    h, t, a = 40.0, 10.0, 30.0          # height, stroke, where the D's bowl starts
    r = h / 2
    d = Polygon([(0, 0)] + arc(a, r, r, -90, 90) + [(0, h)]).difference(
        Polygon([(t, t)] + arc(a, r, r - t, -90, 90) + [(t, h - t)]))
    gap = 3.2
    bar = (h - 2 * gap) / 3
    streaks = [Polygon([(-length, i * (bar + gap) + bar), (-length + bar * 0.9, i * (bar + gap)),
                        (t, i * (bar + gap)), (t, i * (bar + gap) + bar)])
               for i, length in enumerate((26, 40, 18))]
    cuts = [box(-50, bar + k * (bar + gap), t, bar + k * (bar + gap) + gap) for k in range(2)]
    d = unary_union([d] + streaks).difference(unary_union(cuts))
    cb = a + 2 * r + 6.0                # C's bowl centre, a small gap after the D
    c = Polygon([(cb + 28, 0)] + arc(cb, r, r, -90, -270) + [(cb + 28, h)]).difference(
        Polygon([(cb + 28, t)] + arc(cb, r, r - t, -90, -270) + [(cb + 28, h - t)]))
    mono = skew(unary_union([d, c]), xs=-14, origin=(0, h / 2))   # lean forward
    minx, miny, maxx, maxy = mono.bounds
    mono = translate(mono, -minx, -miny)
    out = []
    for p in ([mono] if mono.geom_type == "Polygon" else list(mono.geoms)):
        out.append(list(p.exterior.coords)[:-1])
        out += [list(i.coords)[:-1] for i in p.interiors]
    return out, maxx - minx, maxy - miny


def c_emblem(path, contours, what, w, h):
    pts = [p for c in contours for p in c]
    ends, n = [], 0
    for c in contours:
        n += len(c)
        ends.append(n)
    body = ",\n    ".join(", ".join(f"{{ {x:.3f}f, {y:.3f}f }}" for x, y in pts[i:i + 4])
                           for i in range(0, len(pts), 4))
    path.write_text(f"""// Generated by tools/gen_logo.py - do not edit. Splash emblem: {what}.
// Closed outlines (even-odd fill) in {w:.3f} x {h:.3f} units; contour k ends before index contour_end[k].
const float app_emblem_w = {w:.3f}f, app_emblem_h = {h:.3f}f;
const int app_emblem_contours = {len(contours)};
const int app_emblem_contour_end[] = {{ {", ".join(map(str, ends))} }};
const float app_emblem_pts[][2] = {{
    {body}
}};
""")
    print(f"wrote {path} ({len(pts)} points)")


images = [
    ("app_logo_small", a8(track(0.28) + value_arc() + ticks() + needle(), SMALL_PX), SMALL_PX, SMALL_PX),
]
out = pathlib.Path(__file__).resolve().parent.parent / "main" / "ui" / "app_logo.c"
text = "// Generated by tools/gen_logo.py - do not edit. DashCar header icon (A8).\n#include \"lvgl.h\"\n"
for name, data, w, h in images:
    text += c_image(name, data, w, h)
out.write_text(text)
print(f"wrote {out} ({', '.join(f'{n} {w}x{h}' for n, _, w, h in images)})")
mono, mono_w, mono_h = dc_monogram()
c_emblem(out.parent / "emblem.c", mono, "DashCar \"DC\" monogram (original artwork)", mono_w, mono_h)
if "--dacia" in sys.argv:
    priv = json.loads(PRIVATE_JSON.read_text())
    c_emblem(out.parent / "emblem_private.c", [private_points(priv["path"])],
             "Dacia Link emblem, trademark of Dacia / Renault Group; owner's build only, git-ignored",
             priv["w"], priv["h"])
