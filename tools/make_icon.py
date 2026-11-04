#!/usr/bin/env python3
"""Draws the 48x48 Homebrew Launcher icon in the Borderlands-ish game style.

Not part of the build (smdhtool only needs icon.png); this keeps the source of
the icon around so it can be tweaked without hand-editing a PNG.

    python tools/make_icon.py

Colours match the in-game palette: near-black field, gold trim, the five
damage-type colours and a chunky "ED" monogram with a black comic outline.
"""

from PIL import Image

S = 48
BG = (10, 10, 20, 255)
BG2 = (16, 18, 34, 255)
GOLD = (255, 210, 0, 255)
GOLD_DARK = (140, 96, 8, 255)
OUTLINE = (0, 0, 0, 255)
WHITE = (240, 240, 245, 255)
HP = (242, 46, 46, 255)
SHIELD = (60, 150, 255, 255)
ARMOR = (255, 200, 40, 255)
SLAG = (180, 60, 255, 255)
EXPLOSIVE = (255, 150, 20, 255)

img = Image.new("RGBA", (S, S), BG)
px = img.load()


def rect(x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(S, y1)):
        for x in range(max(0, x0), min(S, x1)):
            px[x, y] = c


# subtle two-tone field
rect(0, 0, S, S, BG)
rect(2, 2, S - 2, S - 2, BG2)

# chunky gold frame with chamfered corners + black inner outline
rect(1, 1, S - 1, 4, GOLD)          # top
rect(1, S - 4, S - 1, S - 1, GOLD)  # bottom
rect(1, 1, 4, S - 1, GOLD)          # left
rect(S - 4, 1, S - 1, S - 1, GOLD)  # right
for i in range(4):                  # chamfer the corners
    rect(1, 1 + i, 4 - i, 2 + i, BG)
    rect(S - 4 + i, 1, S - 1 - i, 2 + i, BG)
    rect(1, S - 2 - i, 4 - i, S - 1 - i, BG)
    rect(S - 4 + i, S - 2 - i, S - 1 - i, S - 1 - i, BG)
# inner black keyline
rect(4, 4, S - 4, 5, OUTLINE)
rect(4, S - 5, S - 4, S - 4, OUTLINE)
rect(4, 4, 5, S - 4, OUTLINE)
rect(S - 5, 4, S - 4, S - 4, OUTLINE)

# hazard ticks in the top corners (tiny yellow/black stripes)
for i in range(0, 7):
    rect(6 + i * 2, 6, 7 + i * 2, 10, GOLD if i % 2 == 0 else OUTLINE)
    rect(41 - i * 2, 6, 42 - i * 2, 10, GOLD if i % 2 == 0 else OUTLINE)

# "ED" monogram, 3x5-ish chunky pixels, 2x scale, with black outline first
E = ["111", "100", "110", "100", "111"]
D = ["110", "101", "101", "101", "110"]
mono = [E[i] + "." + D[i] for i in range(5)]


def glyph_rects(scale, ox, oy):
    out = []
    for gy, line in enumerate(mono[:5]):
        for gx, ch in enumerate(line[:7]):
            if ch == "1":
                out.append((ox + gx * scale, oy + gy * scale, scale))
    return out


def draw_glyphs(scale, ox, oy, color):
    for x, y, s in glyph_rects(scale, ox, oy):
        rect(x, y, x + s, y + s, color)


# outline pass (8 directions), then white fill, then gold inner highlight
for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1)):
    draw_glyphs(3, 12 + dx, 12 + dy, OUTLINE)
draw_glyphs(3, 12, 12, WHITE)

# five element chips along the bottom (one per damage type)
chip_cols = [HP, SHIELD, (60, 220, 60, 255), SLAG, EXPLOSIVE]
x = 6
for c in chip_cols:
    rect(x, 31, x + 6, 37, OUTLINE)
    rect(x + 1, 32, x + 5, 36, c)
    x += 8

# gold underline below the chips
rect(6, 39, 42, 40, GOLD_DARK)

img.save("icon.png")
print("wrote icon.png (%dx%d)" % (S, S))
