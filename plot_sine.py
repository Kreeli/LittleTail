# -*- coding: utf-8 -*-
"""Draw sine_tab from main.c as waveform + TX halfword layout."""
import math
import re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(r"D:\mounriver-studio-projects\LittleTail")
SRC = (ROOT / "User" / "main.c").read_text(encoding="utf-8", errors="ignore")

m = re.search(r"sine_tab\[SINE_PERIOD\]\s*=\s*\{([^}]+)\}", SRC, re.S)
if not m:
    raise SystemExit("sine_tab not found in main.c")
vals = [int(x, 16) if x.strip().lower().startswith("0x") else int(x)
        for x in re.findall(r"0x[0-9A-Fa-f]+|-?\d+", m.group(1))]
# normalize to signed 32-bit
def to_i32(u):
    u &= 0xFFFFFFFF
    return u - 0x100000000 if u >= 0x80000000 else u
vals = [to_i32(v) for v in vals]
N = len(vals)

# ideal sine for comparison (same amp)
amp = 0x60000000
ideal = [math.sin(2 * math.pi * i / N) * amp for i in range(N)]

W, H = 1100, 720
img = Image.new("RGB", (W, H), (18, 18, 24))
d = ImageDraw.Draw(img)

def font(sz):
    try:
        return ImageFont.truetype("arial.ttf", sz)
    except Exception:
        return ImageFont.load_default()

f14, f12 = font(16), font(13)

d.text((24, 18), f"sine_tab[{N}]  from main.c   amp=0x60000000   1kHz @ 48kHz", fill=(220, 220, 230), font=f14)

# --- plot 1: samples ---
x0, y0, pw, ph = 70, 60, 980, 260
mid = y0 + ph // 2
d.rectangle([x0, y0, x0 + pw, y0 + ph], outline=(70, 70, 90), fill=(24, 24, 32))
# zero line
d.line([x0, mid, x0 + pw, mid], fill=(80, 80, 100), width=1)

def sx(i):
    return x0 + i * pw / (N - 1)

def sy(v):
    return mid - v * (ph // 2 - 12) / 0x80000000

# ideal dashed-ish
for i in range(N - 1):
    d.line([sx(i), sy(ideal[i]), sx(i + 1), sy(ideal[i + 1])], fill=(60, 90, 140), width=2)
# table
for i in range(N - 1):
    d.line([sx(i), sy(vals[i]), sx(i + 1), sy(vals[i + 1])], fill=(80, 220, 160), width=3)
for i in range(N):
    d.ellipse([sx(i) - 3, sy(vals[i]) - 3, sx(i) + 3, sy(vals[i]) + 3], fill=(120, 255, 190))

d.text((74, 42), "green = sine_tab[i]   blue = ideal sin(2*pi*i/48)*0x60000000", fill=(160, 160, 180), font=f12)
d.text((74, y0 + ph + 8), "sample index 0..47  (one period)", fill=(150, 150, 170), font=f12)

# --- plot 2: TX buffer as sent lo then hi ---
y1 = 400
d.text((24, y1 - 22), "TX halfword stream (this board: lo first, then hi)  first 24 slots", fill=(220, 220, 230), font=f14)

# build first 12 samples * 2 (L,R) = 24 halfword pairs = 48 halfwords
slots = []
for i in range(12):
    u = vals[i] & 0xFFFFFFFF
    lo, hi = u & 0xFFFF, (u >> 16) & 0xFFFF
    slots.append((lo, hi))

bx, by, cw, ch = 70, y1 + 10, 40, 220
# amplitude of 16-bit halves as bars
d.rectangle([bx, by, bx + cw * 24, by + ch], outline=(70, 70, 90), fill=(24, 24, 32))
mid2 = by + ch // 2
d.line([bx, mid2, bx + cw * 24, mid2], fill=(80, 80, 100), width=1)

def bar(x, w16, color, label):
    # w16 signed
    s = w16 - 0x10000 if w16 >= 0x8000 else w16
    hgt = abs(s) * (ch // 2 - 10) / 32768.0
    if s >= 0:
        d.rectangle([x + 2, mid2 - hgt, x + 22, mid2], fill=color)
    else:
        d.rectangle([x + 2, mid2, x + 22, mid2 + hgt], fill=color)

idx = 0
for i, (lo, hi) in enumerate(slots):
    x = bx + idx * cw
    bar(x, lo, (220, 90, 80), "lo")
    bar(x + 20, hi, (90, 160, 255), "hi")
    idx += 2

d.rectangle([24, H - 36, 44, H - 22], fill=(220, 90, 80))
d.text((50, H - 38), "lo (low 16)", fill=(180, 180, 200), font=f12)
d.rectangle([170, H - 36, 190, H - 22], fill=(90, 160, 255))
d.text((196, H - 38), "hi (high 16)", fill=(180, 180, 200), font=f12)

out = ROOT / "sine_table_plot.png"
img.save(out)
print("wrote", out)
print("N =", N)
print("first8:", [hex(v & 0xFFFFFFFF) for v in vals[:8]])
print("error max:", max(abs(vals[i] - ideal[i]) for i in range(N)))
