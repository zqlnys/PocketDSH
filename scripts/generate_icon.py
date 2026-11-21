#!/usr/bin/env python3
"""Generate the PocketDSH app icon.

The previous icon was a terminal window occupying the middle half of the canvas, with four
small dots near the corners. At the size iOS actually draws an icon - 60 pt on a home screen -
that reads as a small dark square with something indistinct in it: the window had too much
margin around it, and the dots looked like dust on the artwork.

This one fills the canvas: a bold prompt mark and cursor, no decoration that turns to noise at
small sizes, and a gradient with enough range that the icon has a light corner and a dark one.

Draughting notes:
- Everything is drawn at 2048 px and downsampled, so edges land on real pixels.
- No text. Glyph shapes stay legible at 40 px; letters do not.
- Full-bleed: iOS applies the rounded mask itself, and a pre-rounded icon shows corners.
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "PocketDSH" / "Resources" / "Assets.xcassets" / "AppIcon.appiconset" / "AppIcon-1024.png"

FINAL = 1024
SCALE = 2
N = FINAL * SCALE

# Palette. The accent green is the app's own; the deep end is nearly black so the icon has a
# definite dark corner rather than reading as flat.
TOP_LEFT = (16, 34, 38)
BOTTOM_RIGHT = (5, 10, 14)
GLOW = (34, 197, 134)
PROMPT = (52, 229, 158)
CURSOR = (110, 150, 255)


def radial_glow(size: int, centre: tuple[float, float], radius: float, colour: tuple[int, int, int]) -> Image.Image:
    """A soft accent glow, so the icon is not a flat gradient behind a symbol."""
    layer = Image.new("RGB", (size, size), (0, 0, 0))
    draw = ImageDraw.Draw(layer)
    steps = 64
    for step in range(steps, 0, -1):
        t = step / steps
        r = radius * t
        # Quadratic falloff: a linear ramp has a visible ring at the edge.
        intensity = (1 - t) ** 2
        draw.ellipse(
            [centre[0] - r, centre[1] - r, centre[0] + r, centre[1] + r],
            fill=tuple(round(c * intensity) for c in colour),
        )
    return layer.filter(ImageFilter.GaussianBlur(radius / 6))


def background(size: int) -> Image.Image:
    """Diagonal gradient, computed per row against a precomputed column ramp."""
    image = Image.new("RGB", (size, size))
    pixels = image.load()
    denominator = 2 * (size - 1)
    # Precompute the horizontal term; the vertical one is added per row.
    column = [(BOTTOM_RIGHT[0] - TOP_LEFT[0]) * x / denominator for x in range(size)]
    for y in range(size):
        yterm = (BOTTOM_RIGHT[1] - TOP_LEFT[1]) * y / denominator
        for x in range(size):
            pixels[x, y] = (0, 0, 0)
        row = []
        for x in range(size):
            t = (x + y) / denominator
            row.append(
                (
                    round(TOP_LEFT[0] + (BOTTOM_RIGHT[0] - TOP_LEFT[0]) * t),
                    round(TOP_LEFT[1] + (BOTTOM_RIGHT[1] - TOP_LEFT[1]) * t),
                    round(TOP_LEFT[2] + (BOTTOM_RIGHT[2] - TOP_LEFT[2]) * t),
                )
            )
        for x, value in enumerate(row):
            pixels[x, y] = value
    del column, yterm
    return image


def thick_segment(p1, p2, half_width):
    """The quad covering a line of the given half-thickness, as four points.

    Building the chevron from two of these is far less error-prone than working out the six
    points of an outlined angle by hand - the first attempt at that produced a wedge.
    """
    dx = p2[0] - p1[0]
    dy = p2[1] - p1[1]
    length = math.hypot(dx, dy)
    nx = -dy / length * half_width
    ny = dx / length * half_width
    return [
        (p1[0] + nx, p1[1] + ny),
        (p2[0] + nx, p2[1] + ny),
        (p2[0] - nx, p2[1] - ny),
        (p1[0] - nx, p1[1] - ny),
    ]


def draw_mark(image: Image.Image, size: int) -> None:
    """The prompt chevron and the cursor bar."""
    draw = ImageDraw.Draw(image)

    centre_y = 0.500 * size
    left = 0.240 * size
    point_x = left + 0.235 * size
    arm_y = 0.215 * size            # vertical reach of each arm
    half = 0.055 * size              # half the stroke thickness

    upper = [(left, centre_y - arm_y), (point_x, centre_y)]
    lower = [(point_x, centre_y), (left, centre_y + arm_y)]
    for arm in (upper, lower):
        draw.polygon(thick_segment(arm[0], arm[1], half), fill=PROMPT)
    # Square off both ends by drawing the joins as discs; a mitre would leave a notch at the
    # point where the two arms meet.
    for end_point in (upper[0], lower[0], (point_x, centre_y)):
        draw.ellipse(
            [end_point[0] - half, end_point[1] - half,
             end_point[0] + half, end_point[1] + half],
            fill=PROMPT,
        )
    # The point should be a mitre, not a bulge, so trim the disc back to the two arms' envelope
    # by redrawing the outer wedge over it.
    draw.polygon(
        [
            (point_x - half * 0.6, centre_y - half * 1.35),
            (point_x + half * 0.55, centre_y),
            (point_x - half * 0.6, centre_y + half * 1.35),
        ],
        fill=PROMPT,
    )

    bar_left = 0.545 * size
    bar_right = 0.775 * size
    bar_h = 0.115 * size
    draw.rounded_rectangle(
        [bar_left, centre_y - bar_h / 2, bar_right, centre_y + bar_h / 2],
        radius=bar_h / 2,
        fill=CURSOR,
    )


def main() -> None:
    base = background(N)

    glow = radial_glow(N, (0.30 * N, 0.62 * N), 0.62 * N, GLOW)
    base = Image.blend(base, Image.new("RGB", (N, N), (0, 0, 0)), 0.0)
    # Screen-style add keeps the gradient's dark corner intact while lifting the glow area.
    base = Image.composite(
        Image.new("RGB", (N, N), GLOW),
        base,
        glow.convert("L").point(lambda v: int(v * 0.55)),
    )

    draw_mark(base, N)

    final = base.resize((FINAL, FINAL), Image.LANCZOS)
    final.save(OUTPUT, optimize=True)
    print(f"wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
