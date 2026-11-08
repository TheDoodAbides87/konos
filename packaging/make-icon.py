#!/usr/bin/env python3
"""Makes Konos's icons from packaging/icon-source.png (the master artwork).

Writes:
  packaging/icons/<n>x<n>/konos.png   the app icon at the sizes desktops use
  resources/konos.png                 the window icon (built into the app)
  resources/konos-mark.png            the mark alone, without its tile, for
                                      drawing inside the app (Theme::paintLogo)

Needs Pillow and NumPy. Run it again after changing the source.
"""
import pathlib

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
TILE = np.array([2, 15, 64], np.float32)  # the tile's navy, lifted off for the mark


def mark(icon: Image.Image) -> Image.Image:
    """The mark on a transparent background: the tile colour keyed out, soft
    edges un-mixed from it so they don't keep a navy fringe."""
    a = np.asarray(icon.convert("RGBA")).astype(np.float32)
    rgb = a[..., :3]
    d = np.sqrt(((rgb - TILE) ** 2).sum(-1))
    t = np.clip((d - 28) / (95 - 28), 0, 1)
    alpha = t * t * (3 - 2 * t) * (a[..., 3] / 255.0)
    fg = np.clip(TILE + (rgb - TILE) / np.maximum(alpha, 0.35)[..., None], 0, 255)
    out = Image.fromarray(np.dstack([fg, alpha * 255]).astype(np.uint8), "RGBA")
    # Square around the mark, with a little room.
    x0, y0, x1, y1 = out.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
    side = max(x1 - x0, y1 - y0)
    pad = int(side * 0.02)
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    half = side // 2 + pad
    return out.crop((cx - half, cy - half, cx + half, cy + half))


def main():
    icon = Image.open(ROOT / "packaging" / "icon-source.png").convert("RGBA")
    for n in SIZES:
        path = ROOT / "packaging" / "icons" / f"{n}x{n}" / "konos.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        icon.resize((n, n), Image.LANCZOS).save(path, optimize=True)
    icon.resize((256, 256), Image.LANCZOS).save(ROOT / "resources" / "konos.png", optimize=True)
    mark(icon).resize((512, 512), Image.LANCZOS).save(ROOT / "resources" / "konos-mark.png", optimize=True)


if __name__ == "__main__":
    main()
