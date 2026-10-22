#!/usr/bin/env python3
"""Build res/status@{1,2,3,4}x.png, the Netplay status icon sheets.

Laid out like NextUI's assets@Nx.png: each state is a 12x12 icon in 1x units,
and each sheet is drawn pixel-exact at its scale (Varnish never resamples).

    x=0   armed       grey N  (0x99, NextUI's TRIAD_GRAY)
    x=12  connected   white N

Varnish multiplies the icons by the theme's icon colour (color6), as NextUI does
for its own status icons, so white stays the theme colour and grey a darker one.

This draws a placeholder N. To use real artwork, export each state's SVG to a
12*scale square PNG per scale and paste it at the same offsets, keeping the
layout above (launcher/status-icon.sh addresses icons by it).
"""

import os
import sys

from PIL import Image, ImageDraw

UNIT = 12
STATES = [("armed", (0x99, 0x99, 0x99)), ("connected", (0xFF, 0xFF, 0xFF))]
SCALES = (1, 2, 3, 4)
SUPERSAMPLE = 8

# The placeholder N in 1x units: two stems and the diagonal between them.
STEM_W = 2.5
TOP, BOTTOM, LEFT, RIGHT = 1.5, 10.5, 1.75, 10.25


def draw_n(size, colour):
    big = size * SUPERSAMPLE
    k = big / UNIT
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fill = colour + (255,)
    d.rectangle([LEFT * k, TOP * k, (LEFT + STEM_W) * k, BOTTOM * k], fill=fill)
    d.rectangle([(RIGHT - STEM_W) * k, TOP * k, RIGHT * k, BOTTOM * k], fill=fill)
    d.polygon([(LEFT * k, TOP * k), ((LEFT + STEM_W) * k, TOP * k),
               (RIGHT * k, BOTTOM * k), ((RIGHT - STEM_W) * k, BOTTOM * k)], fill=fill)
    return img.resize((size, size), Image.LANCZOS)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "res")
    os.makedirs(out_dir, exist_ok=True)
    for scale in SCALES:
        size = UNIT * scale
        sheet = Image.new("RGBA", (size * len(STATES), size), (0, 0, 0, 0))
        for i, (_, colour) in enumerate(STATES):
            sheet.paste(draw_n(size, colour), (i * size, 0))
        path = os.path.join(out_dir, f"status@{scale}x.png")
        sheet.save(path, optimize=True)
        print(f"wrote {os.path.normpath(path)} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
