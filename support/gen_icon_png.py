#!/usr/bin/env python3
"""Draw support/icon.png: a tiny Windows-defrag style block grid.
make_icon.py turns it into the Workbench .info (dark -> black, light ->
white, grey -> transparent).  Re-run only when changing the look."""
import os
from PIL import Image, ImageDraw

W, H = 36, 40
GREY, BLACK, WHITE = (170, 170, 170), (0, 0, 0), (255, 255, 255)
img = Image.new("RGB", (W, H), GREY)
d = ImageDraw.Draw(img)
d.rectangle([1, 1, W - 2, H - 2], outline=BLACK, fill=WHITE)
# grid of 4x4 cells, some "used" (black), a few "fragmented" (checker)
pattern = [
    "####....",
    "####.#..",
    "###.....",
    "#.#.....",
    "##......",
    "........",
    "#.......",
    "........",
]
for r, row in enumerate(pattern):
    for c, ch in enumerate(row):
        x, y = 3 + c * 4, 3 + r * 4
        if ch == "#":
            d.rectangle([x, y, x + 2, y + 2], fill=BLACK)
        else:
            d.rectangle([x, y, x + 2, y + 2], outline=(200, 200, 200))
d.text((4, 30), "DF", fill=BLACK)
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.png")
img.save(out)
print("wrote", out)
