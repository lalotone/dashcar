#!/usr/bin/env python3
"""Render the DashCar artwork to LVGL A8 (alpha-only) images.

A8 is 1 byte/pixel and is tinted at runtime (image_recolor), so one bitmap serves any colour;
partial alpha gives the dimmer track and ticks. Images:
  app_logo_small    speedometer-gauge emblem (original artwork), small (home header)
  app_logo_text     "DashCar" in Black Ops One, a military stencil face (SIL OFL,
                    tools/fonts/OFL-BlackOpsOne.txt), for the splash. Only this bitmap ships, not the font.
  app_compass_n/_s  compass needle halves (north/south), tinted differently, rotated together
Plus the splash landscape (dawn sky, stars, three mountain ranges with snow, pine forest), original
artwork drawn procedurally with a fixed seed, as a baseline JPEG (decoded into PSRAM at boot).

Usage:  uvx --with cairosvg --with pillow python tools/gen_logo.py
Writes: main/ui/app_logo.c, main/ui/splash_bg.jpg
"""
import io
import math
import pathlib
import random

import cairosvg
from PIL import Image, ImageDraw, ImageFont

# Geometry on a 200x200 canvas, angles in degrees clockwise from 3 o'clock (same as lv_arc).
R, W = 78, 15                 # dial radius (stroke centre) and stroke width
START, END, NEEDLE = 135, 405, 300
SMALL_PX = 30                  # home-header icon size
TEXT_FONT = pathlib.Path(__file__).resolve().parent / "fonts" / "BlackOpsOne-Regular.ttf"
TEXT_PX = 100                  # font size of the splash word mark


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


def text_a8(text):
    font = ImageFont.truetype(str(TEXT_FONT), TEXT_PX)
    x0, y0, x1, y1 = font.getbbox(text)
    img = Image.new("L", (x1 - x0 + 4, y1 - y0 + 4), 0)
    ImageDraw.Draw(img).text((2 - x0, 2 - y0), text, font=font, fill=255)
    return img.tobytes(), img.width, img.height


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


NEEDLE_W, NEEDLE_HALF = 22, 46   # compass needle: width, length of each half (px); clears the N/S letters


def needle_half(north):
    """Diamond needle half, tip at the top; pivot = bottom centre (north) / top centre (south)."""
    s = 4  # supersample
    w, h = NEEDLE_W * s, NEEDLE_HALF * s
    pts = f"{w / 2},0 {w},{h} 0,{h}" if north else f"0,0 {w},0 {w / 2},{h}"
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}">'
           f'<polygon points="{pts}" fill="#fff"/></svg>')
    png = cairosvg.svg2png(bytestring=svg.encode())
    img = Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    img = img.resize((NEEDLE_W, NEEDLE_HALF), Image.LANCZOS)
    return img.tobytes(), NEEDLE_W, NEEDLE_HALF


def ridge(rng, x0, x1, base, amp, rough, n=7, peaks=()):
    """Mountain ridge by midpoint displacement: list of (x, y). `peaks` = (x, height) seeds."""
    pts = [(x0, base - rng.uniform(0, amp * 0.3))] + [(x, base - hgt) for x, hgt in peaks] + \
          [(x1, base - rng.uniform(0, amp * 0.3))]
    disp = amp
    for _ in range(n):
        out = []
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            out += [(ax, ay), ((ax + bx) / 2, (ay + by) / 2 + rng.uniform(-disp, disp))]
        pts = out + [pts[-1]]
        disp *= rough
    return pts


def poly(pts, bottom=600):
    d = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
    return f"{pts[0][0]:.1f},{bottom} {d} {pts[-1][0]:.1f},{bottom}"


def scene_jpeg(path, w=1024, h=600):
    rng = random.Random(7)
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}">',
           '<defs><linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#03060D"/><stop offset="0.45" stop-color="#0E1A30"/>'
           '<stop offset="0.68" stop-color="#22324E"/><stop offset="0.80" stop-color="#4A4A5A"/></linearGradient>'
           '<radialGradient id="glow" cx="0.62" cy="0.70" r="0.45">'
           '<stop offset="0" stop-color="#F59E0B" stop-opacity="0.35"/>'
           '<stop offset="1" stop-color="#F59E0B" stop-opacity="0"/></radialGradient>'
           '<linearGradient id="mist" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#9FB3C8" stop-opacity="0"/>'
           '<stop offset="0.6" stop-color="#9FB3C8" stop-opacity="0.18"/>'
           '<stop offset="1" stop-color="#9FB3C8" stop-opacity="0"/></linearGradient></defs>',
           f'<rect width="{w}" height="{h}" fill="url(#sky)"/>',
           f'<rect width="{w}" height="{h}" fill="url(#glow)"/>']
    for _ in range(90):  # stars, denser and brighter towards the top
        y = rng.uniform(0, 300) ** 1.15 / 300 ** 0.15
        svg.append(f'<circle cx="{rng.uniform(0, w):.1f}" cy="{y:.1f}" r="{rng.uniform(0.5, 1.5):.2f}" '
                   f'fill="#fff" fill-opacity="{rng.uniform(0.25, 0.9) * (1 - y / 360):.2f}"/>')
    # Far range with snow caps (clipped to the range), then mist, mid and near ranges.
    far = ridge(rng, -20, w + 20, 420, 90, 0.5, peaks=((150, 120), (330, 175), (470, 95), (700, 190), (880, 130)))
    svg.append(f'<polygon points="{poly(far)}" fill="#34465F"/>')
    # Snow caps: below the ridge, deeper the higher the peak rises above the snow line.
    snowline = 290
    lower = [(x, y + max(0.0, snowline - y) * 0.45 * rng.uniform(0.6, 1.0)) for x, y in far]
    cap = " ".join(f"{x:.1f},{y:.1f}" for x, y in far + lower[::-1])
    svg.append(f'<polygon points="{cap}" fill="#D7E0EA" fill-opacity="0.85"/>')
    svg.append(f'<rect y="330" width="{w}" height="150" fill="url(#mist)"/>')
    mid = ridge(rng, -20, w + 20, 475, 60, 0.5, peaks=((90, 90), (560, 70), (960, 100)))
    svg.append(f'<polygon points="{poly(mid)}" fill="#1F2D44"/>')
    svg.append(f'<rect y="420" width="{w}" height="110" fill="url(#mist)"/>')
    near = ridge(rng, -20, w + 20, 525, 40, 0.5, peaks=((260, 55), (780, 45)))
    svg.append(f'<polygon points="{poly(near)}" fill="#131D2E"/>')
    # Foreground: a low ridge covered in pines.
    fg = ridge(rng, -20, w + 20, 560, 30, 0.5, n=6)
    svg.append(f'<polygon points="{poly(fg)}" fill="#090F1A"/>')
    for i in range(140):
        x = rng.uniform(-10, w + 10)
        gy = min(y for px, y in fg if abs(px - x) < 12) + 6  # sit on the ridge
        th = rng.uniform(22, 58) * (1.25 if i % 9 == 0 else 1)
        tw = th * rng.uniform(0.32, 0.42)
        tiers = "".join(f'<polygon points="{x - tw * k:.1f},{gy - th * (0.55 - 0.3 * k) + 0:.1f} '
                        f'{x:.1f},{gy - th * (1.0 - 0.32 * (1 - k)):.1f} {x + tw * k:.1f},'
                        f'{gy - th * (0.55 - 0.3 * k):.1f}"/>' for k in (0.6, 0.8, 1.0))
        svg.append(f'<g fill="#070C15">{tiers}<rect x="{x - 1.2:.1f}" y="{gy - th * 0.3:.1f}" '
                   f'width="2.4" height="{th * 0.3:.1f}"/></g>')
    svg.append("</svg>")
    png = cairosvg.svg2png(bytestring="".join(svg).encode())
    img = Image.open(io.BytesIO(png)).convert("RGB")
    img.save(path, "JPEG", quality=88, optimize=True, progressive=False)
    return path.stat().st_size


images = [
    ("app_logo_small", a8(track(0.28) + value_arc() + ticks() + needle(), SMALL_PX), SMALL_PX, SMALL_PX),
    ("app_logo_text", *text_a8("DashCar")),
    ("app_compass_n", *needle_half(True)),
    ("app_compass_s", *needle_half(False)),
]
out = pathlib.Path(__file__).resolve().parent.parent / "main" / "ui" / "app_logo.c"
text = "// Generated by tools/gen_logo.py - do not edit. DashCar emblem, word mark, compass needle; A8.\n#include \"lvgl.h\"\n"
for name, data, w, h in images:
    text += c_image(name, data, w, h)
out.write_text(text)
print(f"wrote {out} ({', '.join(f'{n} {w}x{h}' for n, _, w, h in images)})")
jpg = out.parent / "splash_bg.jpg"
print(f"wrote {jpg} ({scene_jpeg(jpg) // 1024} KB)")
