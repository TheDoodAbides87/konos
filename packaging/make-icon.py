#!/usr/bin/env python3
"""Writes resources/konos.svg: the Konos mark.

A split K on a block grid (the same grid the Omarchy wordmark uses): a
straight stem, a one-block gap, then the two arms as a chevron that meets in
the middle. On the app icon, the lower leg is a second colour.
Theme::paintLogo() draws the same shape in one colour (keep the two in sync).
"""
import pathlib

ROWS = 13           # rows of blocks; the arms meet on the middle one
STEM = (1, 4)       # stem columns [left, right) on a 16-block grid
GAP = 1             # empty columns between the stem and the arms
ARM = 4             # arm width in blocks
RIGHT = 15          # rightmost column used (exclusive)

UNIT, OFF = 14, 16  # 16 blocks of 14 px on a 256 px canvas
TOP = (256 - ROWS * UNIT) // 2  # centred vertically


def arm(r):
    """The arm's [left, right) columns on row r."""
    left = STEM[1] + GAP + abs(r - ROWS // 2)
    return left, min(left + ARM, RIGHT)


def svg(fg="#8B7CF6", leg="#3DD6C0", tile="#16141F"):
    rects = []
    for r in range(ROWS):
        y = TOP + r * UNIT
        for (a, b), colour in ((STEM, fg), (arm(r), leg if r > ROWS // 2 else fg)):
            rects.append(f'<rect x="{OFF + a * UNIT}" y="{y}" width="{(b - a) * UNIT}" height="{UNIT}" fill="{colour}"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" shape-rendering="crispEdges">\n'
            f'  <rect x="8" y="8" width="240" height="240" rx="56" fill="{tile}"/>\n'
            "  " + "\n  ".join(rects) + "\n</svg>\n")


if __name__ == "__main__":
    out = pathlib.Path(__file__).resolve().parent.parent / "resources" / "konos.svg"
    out.write_text(svg())
    print(out)
