"""ChromaStudio app icon: a calligraphic "C" stroke in a chromatic gradient on a dark squircle.

    python tools/make_icon.py            (needs numpy and Pillow)

Writes resources/app.ico (16–256 px), resources/icon-256.png and docs/images/logo.png (512 px).
Everything is computed per pixel at 2048 px and filtered down, so the edges stay clean at 16 px.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
S = 2048  # working size


def lerp(a, b, t):
    return a + (b - a) * t


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def hex_rgb(h):
    h = h.lstrip('#')
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * c ** (1 / 2.4) - 0.055)


def palette(t, stops):
    """Colour stops interpolated in linear light; t is an array in [0, 1]."""
    pos = np.array([p for p, _ in stops])
    cols = np.array([to_linear(hex_rgb(c)) for _, c in stops])
    out = np.empty(t.shape + (3,))
    for ch in range(3):
        out[..., ch] = np.interp(t, pos, cols[:, ch])
    return out


y, x = np.mgrid[0:S, 0:S].astype(np.float64) + 0.5
u = (x - S / 2) / (S / 2)  # [-1, 1], y down
v = (y - S / 2) / (S / 2)
px = 2.0 / S  # one working pixel in u units

# ── Tile: a superellipse (n = 5) with a 1-px soft edge ──
n = 5.0
margin = 0.06
q = (np.abs(u) / (1 - margin)) ** n + (np.abs(v) / (1 - margin)) ** n
tile = smoothstep(1.0 + 3 * px * n, 1.0 - 3 * px * n, q)

# Background: deep indigo to near-black, a violet glow behind the stroke.
bg_t = np.clip((u + v) * 0.35 + 0.5, 0, 1)
bg = lerp(to_linear(hex_rgb('#2B1760'))[None, None, :], to_linear(hex_rgb('#07060F'))[None, None, :], bg_t[..., None])
glow_r = np.sqrt((u + 0.08) ** 2 + (v + 0.02) ** 2)
bg += to_linear(hex_rgb('#5B21B6'))[None, None, :] * (0.55 * np.exp(-(glow_r / 0.55) ** 2))[..., None]

# ── The stroke: an open "C" from the upper right, round the left, to the lower right ──
r = np.sqrt(u ** 2 + v ** 2)
ang = np.degrees(np.arctan2(-v, u))  # counter-clockwise, 0° = right, y up
a0, a1 = 38.0, 318.0  # start (upper right) and end (lower right), counter-clockwise
sweep = (ang - a0) % 360.0
t = sweep / (a1 - a0)
inside_arc = sweep <= (a1 - a0)
t = np.clip(t, 0, 1)
R = 0.50 * (1.0 - 0.10 * t)  # a slight spiral inwards
taper = np.clip(np.sin(np.pi * np.clip(t * 0.92 + 0.04, 0, 1)), 0, 1) ** 0.65
width = 0.30 * taper * (0.55 + 0.45 * t)  # pressure grows towards the end
sd = width / 2 - np.abs(r - R)  # signed distance, positive inside
stroke = np.where(inside_arc, smoothstep(-1.5 * px, 1.5 * px, sd), 0.0)

stops = [(0.00, '#22D3EE'), (0.28, '#6366F1'), (0.52, '#C026D3'), (0.76, '#FB7185'), (1.00, '#FDBA74')]
col = palette(t, stops)
# A tube: lighter on the outer edge, a thin specular line along it.
across = np.clip((r - R) / np.maximum(width / 2, 1e-6), -1, 1)
shade = 0.78 + 0.32 * (across + 1) / 2
col = col * shade[..., None]
spec = np.exp(-((across - 0.45) / 0.18) ** 2) * 0.35 * taper
col = col + spec[..., None]

# ── The tip: a glowing drop at the end of the stroke ──
end = np.radians(a1)
Rend = 0.50 * 0.90
cx, cy = Rend * np.cos(end) + 0.03, -Rend * np.sin(end) + 0.02
d_tip = np.sqrt((u - cx) ** 2 + (v - cy) ** 2)
tip_r = 0.085
tip = smoothstep(tip_r + 1.5 * px, tip_r - 1.5 * px, d_tip)
tip_col = lerp(to_linear(hex_rgb('#FFF7ED')), to_linear(hex_rgb('#FDBA74')), 0.25)
tip_spec = np.exp(-(((u - cx + 0.03) ** 2 + (v - cy + 0.03) ** 2) / 0.0009))

# ── Compose in linear light ──
img = bg.copy()
# Glow under the stroke: the stroke colour blurred.
glow_src = Image.fromarray((to_srgb(col * stroke[..., None]) * 255).astype(np.uint8))
glow = np.asarray(glow_src.filter(ImageFilter.GaussianBlur(S * 0.035)), dtype=np.float64) / 255.0
img += to_linear(glow) * 0.9
tip_glow = np.exp(-(d_tip / 0.16) ** 2) * 0.55
img += to_linear(hex_rgb('#FDBA74'))[None, None, :] * tip_glow[..., None]
img = img * (1 - stroke[..., None]) + col * stroke[..., None]
img = img * (1 - tip[..., None]) + (tip_col[None, None, :] + tip_spec[..., None] * 0.6) * tip[..., None]

# Glass: a soft sheen at the top and a faint inner rim.
sheen = smoothstep(0.1, -0.95, v) * 0.022
img += sheen[..., None]
rim = smoothstep(1.0 - 14 * px * n, 1.0 - 4 * px * n, q) * smoothstep(1.0 + 3 * px * n, 1.0 - 3 * px * n, q)
img += (rim * 0.05)[..., None]

rgb = to_srgb(img)
rgba = np.dstack([rgb, tile]) * 255.0
big = Image.fromarray(np.clip(rgba + 0.5, 0, 255).astype(np.uint8), 'RGBA')

(ROOT / 'resources').mkdir(exist_ok=True)
(ROOT / 'docs' / 'images').mkdir(parents=True, exist_ok=True)


def scaled(size):
    return big.resize((size, size), Image.LANCZOS)


scaled(512).save(ROOT / 'docs' / 'images' / 'logo.png', optimize=True)
scaled(256).save(ROOT / 'resources' / 'icon-256.png', optimize=True)
sizes = [256, 128, 64, 48, 40, 32, 24, 20, 16]
scaled(256).save(ROOT / 'resources' / 'app.ico', sizes=[(s, s) for s in sizes])
# A strip of all sizes, for checking small-size legibility.
strip = Image.new('RGBA', (sum(sizes) + 10 * len(sizes), 256), (40, 40, 48, 255))
x0 = 0
for s in sizes:
    strip.alpha_composite(scaled(s), (x0, 0))
    x0 += s + 10
strip.save(ROOT / 'docs' / 'images' / 'icon-sizes.png')
print('ok')
