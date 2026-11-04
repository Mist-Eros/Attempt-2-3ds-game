#!/usr/bin/env python3
"""Tiny script that draws the 48x48 Homebrew Launcher icon.

Not part of the build (smdhtool only needs icon.png); this just keeps the
source of the icon around in case I want to tweak it later.

    python tools/make_icon.py
"""

from PIL import Image

S = 48
BG = (10, 10, 26, 255)
BORDER = (255, 204, 51, 255)
FIRE = (242, 38, 38, 255)
WATER = (38, 102, 242, 255)
FOREST = (38, 230, 38, 255)
DARK = (20, 20, 40, 255)

img = Image.new("RGBA", (S, S), BG)
px = img.load()


def rect(x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(S, y1)):
        for x in range(max(0, x0), min(S, x1)):
            px[x, y] = c


# chunky rounded-ish border
rect(0, 0, S, 2, BORDER)
rect(0, S - 2, S, S, BORDER)
rect(0, 0, 2, S, BORDER)
rect(S - 2, 0, S, S, BORDER)
rect(2, 2, 6, 6, BG)
rect(S - 6, 2, S - 2, 6, BG)
rect(2, S - 6, 6, S - 2, BG)
rect(S - 6, S - 6, S - 2, S - 2, BG)


def flame(cx, cy, h):
    # simple pixel flame: wide base, pointed top, notch in the middle
    for dy in range(h):
        yy = cy + h // 2 - dy
        half = 1 + (dy * 2) // 3
        rect(cx - half, yy, cx + half + 1, yy + 1, FIRE)
    rect(cx - 1, cy + h // 2 - 1, cx + 1, cy + h // 2 + 1, (255, 220, 160, 255))


def droplet(cx, cy, h):
    # circle body + pointy top
    r = h // 2
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                px[cx + dx, cy + dy + 1] = WATER
    for i in range(h // 2):
        rect(cx - i // 2, cy - r + i + 1, cx + i // 2 + 1, cy - r + i + 2, WATER)
    rect(cx - 1, cy - 1, cx, cy, (200, 230, 255, 255))


def leaf(cx, cy, h):
    # diamond-ish leaf with a stem
    r = h // 2
    for dy in range(-r, r + 1):
        half = r - abs(dy)
        rect(cx - half - 1, cy + dy, cx + half + 1, cy + dy + 1, FOREST)
    rect(cx, cy + r, cx + 1, cy + r + 3, (120, 80, 30, 255))
    rect(cx - 1, cy - 1, cx + 1, cy, (200, 255, 200, 255))


# three elements above a little tower base
flame(15, 12, 9)
droplet(24, 12, 8)
leaf(33, 12, 8)

# tower: three stacked steps, centred
rect(20, 30, 28, 34, DARK)
rect(17, 34, 31, 38, DARK)
rect(14, 38, 34, 42, DARK)
rect(20, 30, 28, 31, BORDER)
rect(17, 34, 31, 35, BORDER)
rect(14, 38, 34, 39, BORDER)

img.save("icon.png")
print("wrote icon.png (%dx%d)" % (S, S))
