#!/usr/bin/env python3
"""Generate the PocketDSH app icon: a white field with a black O.

Draughting notes:
- Drawn at 2048 and downsampled, so the ring's edges land on real pixels. At the 60 pt a home
  screen uses the whole mark is 42 px across, and an edge that falls between pixels reads as
  blur rather than as an edge.
- The O is a ring, not a glyph: no font is involved, so nothing depends on which fonts a build
  machine has.
- The ring is a touch wider than tall. A circle inside a square looks slightly narrow, because
  the square's corners are empty; this corrects for that.
- Full bleed. iOS applies the rounded mask itself, and an icon that rounds its own corners shows
  them twice.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "PocketDSH" / "Resources" / "Assets.xcassets" / "AppIcon.appiconset" / "AppIcon-1024.png"

FINAL = 1024
SCALE = 4          # supersample for clean curves
N = FINAL * SCALE

WHITE = (255, 255, 255)
BLACK = (17, 17, 17)    # not pure black: pure black on pure white vibrates at small sizes


def main() -> None:
    image = Image.new("RGB", (N, N), WHITE)
    draw = ImageDraw.Draw(image)

    # Ring geometry as fractions of the canvas.
    cx, cy = 0.5 * N, 0.5 * N
    outer_w = 0.315 * N          # half-width of the ring's outer edge
    outer_h = 0.335 * N          # half-height: slightly taller, see the note above
    thickness = 0.098 * N        # stroke weight

    # The outer ellipse first, then the hole. Drawing the hole white rather than compositing a
    # mask keeps the edge crisp at the supersampled size.
    draw.ellipse([cx - outer_w, cy - outer_h, cx + outer_w, cy + outer_h], fill=BLACK)
    inner_w = outer_w - thickness
    inner_h = outer_h - thickness
    draw.ellipse([cx - inner_w, cy - inner_h, cx + inner_w, cy + inner_h], fill=WHITE)

    final = image.resize((FINAL, FINAL), Image.LANCZOS)
    final.save(OUTPUT, optimize=True)
    print(f"wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
