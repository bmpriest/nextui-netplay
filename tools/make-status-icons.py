#!/usr/bin/env python3
"""Build res/status@{1,2,3,4}x.png, the Netplay status icon sheets, from
res/netplay_logo_stretch.svg.

Laid out like NextUI's assets@Nx.png: each icon is 14x14 in 1x units (NextUI's
own are 12, but this one sits alone in a 30-unit pill), and each sheet is drawn
pixel-exact at its scale (Varnish never resamples).

    x=0   idle        whole N dimmed       (persistent icon, no session)
    x=14  hosting     left half lit        (armed, nobody connected yet)
    x=28  connected   both halves lit

Everything is drawn white whatever colour the SVG uses: Varnish multiplies the
icons by the theme's icon colour (color6), as NextUI does for its own status
icons, so white becomes the theme colour and the dimmed parts a fainter one.

The two halves are told apart by element id (LEFT / RIGHT below): the left
stem with its diagonal and the dot it ends in, and the right stem with its
diagonal and the arc around the dot. Only what Inkscape writes for these files
is understood: paths of straight segments (M L H V Z), ellipses, and
sodipodi arcs; transforms rotate/translate/scale/matrix.
"""

import math
import os
import re
import sys
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

UNIT = 14
SCALES = (1, 2, 3, 4)
SUPERSAMPLE = 16
DIM = 0.35          # alpha of the parts that are not lit
INSET = 0.0         # 1x units of clear space around the logo inside its cell
LOGO = "netplay_logo_stretch.svg"
LEFT = {"path3", "path2"}
RIGHT = {"path1", "path4"}
# (left lit, right lit) per icon, left to right on the sheet.
STATES = [("idle", False, False), ("hosting", True, False), ("connected", True, True)]

SVG = "{http://www.w3.org/2000/svg}"
SODI = "{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}"
NUM = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"


def parse_transform(text):
    """Affine (a, b, c, d, e, f) for an SVG transform attribute."""
    m = (1, 0, 0, 1, 0, 0)
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", text or ""):
        v = [float(x) for x in re.findall(NUM, args)]
        if name == "rotate":
            r = math.radians(v[0])
            t = (math.cos(r), math.sin(r), -math.sin(r), math.cos(r), 0, 0)
        elif name == "translate":
            t = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0)
        elif name == "scale":
            t = (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0)
        elif name == "matrix":
            t = tuple(v)
        else:
            raise SystemExit(f"unsupported transform {name}")
        a, b, c, d, e, f = m
        A, B, C, D, E, F = t
        m = (a * A + c * B, b * A + d * B, a * C + c * D, b * C + d * D,
             a * E + c * F + e, b * E + d * F + f)
    return m


def apply(m, pts):
    a, b, c, d, e, f = m
    return [(a * x + c * y + e, b * x + d * y + f) for x, y in pts]


def path_polygons(d):
    """Polygons of a path made only of straight segments."""
    tokens = re.findall(r"[MLHVZmlhvz]|" + NUM, d)
    polys, cur = [], []
    x = y = 0.0
    cmd = None
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                    x, y = cur[0]
                cur = []
            continue
        rel = cmd.islower()
        c = cmd.upper()
        if c in "ML":
            nx, ny = float(tokens[i]), float(tokens[i + 1])
            i += 2
            if rel:
                nx, ny = x + nx, y + ny
            if c == "M":
                if cur:
                    polys.append(cur)
                cur = [(nx, ny)]
                cmd = "l" if rel else "L"   # implicit lineto after moveto
            else:
                cur.append((nx, ny))
            x, y = nx, ny
        elif c == "H":
            x = x + float(tokens[i]) if rel else float(tokens[i])
            i += 1
            cur.append((x, y))
        elif c == "V":
            y = y + float(tokens[i]) if rel else float(tokens[i])
            i += 1
            cur.append((x, y))
        else:
            raise SystemExit(f"unsupported path command {cmd}")
    if cur:
        polys.append(cur)
    return polys


def style_of(el):
    style = dict(kv.split(":", 1) for kv in el.get("style", "").split(";") if ":" in kv)
    return style.get("fill", "#000000") != "none", float(style.get("stroke-width", 0)), \
        style.get("stroke", "none") != "none"


def shapes(path):
    """[(id, kind, points, width)]: kind 'fill' (polygon) or 'line' (stroke)."""
    root = ET.parse(path).getroot()
    out = []
    for el in root.iter():
        tag = el.tag.replace(SVG, "")
        if tag not in ("path", "ellipse"):
            continue
        m = parse_transform(el.get("transform"))
        fill, width, stroke = style_of(el)
        if el.get(SODI + "type") == "arc":
            cx, cy = float(el.get(SODI + "cx")), float(el.get(SODI + "cy"))
            rx, ry = float(el.get(SODI + "rx")), float(el.get(SODI + "ry"))
            a0, a1 = float(el.get(SODI + "start")), float(el.get(SODI + "end"))
            if a1 < a0:
                a1 += 2 * math.pi
            n = 96
            pts = [(cx + rx * math.cos(a0 + (a1 - a0) * k / n),
                    cy + ry * math.sin(a0 + (a1 - a0) * k / n)) for k in range(n + 1)]
            if stroke:
                out.append((el.get("id"), "line", apply(m, pts), width))
            if fill:
                out.append((el.get("id"), "fill", apply(m, pts + [(cx, cy)]), 0))
        elif tag == "ellipse":
            cx, cy = float(el.get("cx")), float(el.get("cy"))
            rx, ry = float(el.get("rx")), float(el.get("ry"))
            pts = [(cx + rx * math.cos(2 * math.pi * k / 96),
                    cy + ry * math.sin(2 * math.pi * k / 96)) for k in range(96)]
            out.append((el.get("id"), "fill", apply(m, pts), 0))
        else:
            for poly in path_polygons(el.get("d")):
                if fill:
                    out.append((el.get("id"), "fill", apply(m, poly), 0))
                if stroke:
                    out.append((el.get("id"), "line", apply(m, poly + poly[:1]), width))
    root_vb = [float(v) for v in root.get("viewBox").split()]
    return out, root_vb


def draw_icon(svg, size, left_lit, right_lit):
    elements, (vx, vy, vw, vh) = shapes(svg)
    big = size * SUPERSAMPLE
    pad = INSET * big / UNIT
    k = (big - 2 * pad) / max(vw, vh)
    alpha = Image.new("L", (big, big), 0)
    for lit in (False, True):
        # Each half drawn on its own mask, so overlapping shapes within it do
        # not double up, then laid down at its brightness.
        mask = Image.new("L", (big, big), 0)
        d = ImageDraw.Draw(mask)
        for eid, kind, pts, width in elements:
            if eid not in LEFT and eid not in RIGHT:
                raise SystemExit(f"{svg}: element '{eid}' is in neither half")
            part_lit = left_lit if eid in LEFT else right_lit
            if part_lit != lit:
                continue
            p = [(pad + (x - vx) * k, pad + (y - vy) * k) for x, y in pts]
            if kind == "fill":
                d.polygon(p, fill=255)
            else:
                d.line(p, fill=255, width=max(1, round(width * k)), joint="curve")
        level = 255 if lit else round(255 * DIM)
        alpha.paste(level, mask=mask)
    icon = Image.new("RGBA", (big, big), (255, 255, 255, 0))
    icon.putalpha(alpha)
    return icon.resize((size, size), Image.BOX)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    res = os.path.join(here, "..", "res")
    out_dir = sys.argv[1] if len(sys.argv) > 1 else res
    os.makedirs(out_dir, exist_ok=True)
    for scale in SCALES:
        size = UNIT * scale
        sheet = Image.new("RGBA", (size * len(STATES), size), (0, 0, 0, 0))
        for col, (_, left_lit, right_lit) in enumerate(STATES):
            icon = draw_icon(os.path.join(res, LOGO), size, left_lit, right_lit)
            sheet.paste(icon, (col * size, 0))
        path = os.path.join(out_dir, f"status@{scale}x.png")
        sheet.save(path, optimize=True)
        print(f"wrote {os.path.normpath(path)} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
