#!/usr/bin/env python3
"""Writes resources/konos.svg: the Konos mark.

A blocky traffic cone stood on its tip, with a V cut down into it from the
top, drawn on a block grid (the same grid the Omarchy wordmark uses).
Theme::paintLogo() draws the same rows (keep the two in sync).
"""
import pathlib

# (outer [left, right), V cut [left, right) or None) per row, on a 16-block grid.
ROWS = [
    ((1, 15), (4, 12)), ((1, 15), (4, 12)),
    ((2, 14), (5, 11)), ((2, 14), (5, 11)),
    ((3, 13), (6, 10)), ((3, 13), (6, 10)),
    ((4, 12), (7, 9)), ((4, 12), (7, 9)),
    ((5, 11), None), ((5, 11), None),
    ((6, 10), None), ((6, 10), None),
    ((7, 9), None),
]

# Rows painted in the stripe colour: a traffic cone's reflective bands. Only
# the full-size icon has them; the small in-app mark is a plain V.
STRIPES = {3, 7}

UNIT, OFF = 14, 16  # 16 blocks of 14 px on a 256 px canvas
TOP = (256 - len(ROWS) * UNIT) // 2  # centred vertically


def svg(fg="#E8831C", stripe="#F2EEE8", tile="#1c1c1c"):
    rects = []
    for r, ((left, right), cut) in enumerate(ROWS):
        y = TOP + r * UNIT
        fill = f' fill="{stripe}"' if r in STRIPES else ""
        for a, b in [(left, right)] if not cut else [(left, cut[0]), (cut[1], right)]:
            rects.append(f'<rect x="{OFF + a * UNIT}" y="{y}" width="{(b - a) * UNIT}" height="{UNIT}"{fill}/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" shape-rendering="crispEdges">\n'
            f'  <rect x="8" y="8" width="240" height="240" rx="56" fill="{tile}"/>\n'
            f'  <g fill="{fg}">\n    ' + "\n    ".join(rects) + "\n  </g>\n</svg>\n")


if __name__ == "__main__":
    out = pathlib.Path(__file__).resolve().parent.parent / "resources" / "konos.svg"
    out.write_text(svg())
    print(out)
