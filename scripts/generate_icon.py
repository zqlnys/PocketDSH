#!/usr/bin/env python3
"""Generate the PocketDSH app icon with Pillow."""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "PocketDSH" / "Resources" / "Assets.xcassets" / "AppIcon.appiconset" / "AppIcon-1024.png"

SIZE = 1024
SCALE = 2
CANVAS = SIZE * SCALE


def diagonal_gradient(size: int, start: tuple[int, int, int], end: tuple[int, int, int]) -> Image.Image:
    image = Image.new("RGB", (size, size))
    denominator = 2 * (size - 1)
    pixels = []
    for y in range(size):
        for x in range(size):
            t = (x + y) / denominator
            pixels.append(
                (
                    round(start[0] + (end[0] - start[0]) * t),
                    round(start[1] + (end[1] - start[1]) * t),
                    round(start[2] + (end[2] - start[2]) * t),
                )
            )
    image.putdata(pixels)
    return image


def build_icon() -> Image.Image:
    background = diagonal_gradient(SIZE, (6, 14, 20), (17, 35, 45)).resize(
        (CANVAS, CANVAS), Image.Resampling.BICUBIC
    )

    glow = Image.new("RGB", (CANVAS, CANVAS), (0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    glow_draw.ellipse(
        (CANVAS * 0.38, CANVAS * 0.36, CANVAS * 1.02, CANVAS * 1.02),
        fill=(24, 160, 112),
    )
    glow = glow.filter(ImageFilter.GaussianBlur(CANVAS * 0.16))
    background = Image.blend(background, glow, 0.22)

    draw = ImageDraw.Draw(background)
    margin = int(CANVAS * 0.16)
    top = int(CANVAS * 0.24)
    bottom = int(CANVAS * 0.76)
    radius = int(CANVAS * 0.07)
    draw.rounded_rectangle(
        (margin, top, CANVAS - margin, bottom),
        radius=radius,
        fill=(11, 23, 30),
        outline=(61, 220, 151),
        width=int(CANVAS * 0.012),
    )

    bar_y = top + int(CANVAS * 0.075)
    for offset, color in [(0.0, (255, 107, 107)), (0.028, (245, 184, 75)), (0.056, (61, 220, 151))]:
        cx = margin + int(CANVAS * 0.055) + int(CANVAS * offset)
        dot_r = int(CANVAS * 0.009)
        draw.ellipse((cx - dot_r, bar_y - dot_r, cx + dot_r, bar_y + dot_r), fill=color)

    draw.line(
        (margin, bar_y + int(CANVAS * 0.035), CANVAS - margin, bar_y + int(CANVAS * 0.035)),
        fill=(30, 58, 68),
        width=int(CANVAS * 0.004),
    )

    prompt_x = margin + int(CANVAS * 0.09)
    prompt_y = bar_y + int(CANVAS * 0.16)
    chevron = int(CANVAS * 0.055)
    line_width = int(CANVAS * 0.022)
    draw.line(
        (prompt_x, prompt_y, prompt_x + chevron, prompt_y + chevron),
        fill=(61, 220, 151),
        width=line_width,
        joint="curve",
    )
    draw.line(
        (prompt_x + chevron, prompt_y + chevron, prompt_x, prompt_y + chevron * 2),
        fill=(61, 220, 151),
        width=line_width,
        joint="curve",
    )

    cursor_x = prompt_x + chevron + int(CANVAS * 0.055)
    draw.rounded_rectangle(
        (
            cursor_x,
            prompt_y + int(CANVAS * 0.075),
            cursor_x + int(CANVAS * 0.24),
            prompt_y + int(CANVAS * 0.105),
        ),
        radius=int(CANVAS * 0.012),
        fill=(91, 140, 255),
    )

    for x, y, radius_px, opacity in [
        (0.20, 0.18, 0.010, 120),
        (0.82, 0.16, 0.006, 100),
        (0.84, 0.83, 0.008, 90),
        (0.16, 0.84, 0.005, 80),
    ]:
        star = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
        star_draw = ImageDraw.Draw(star)
        cx, cy = int(CANVAS * x), int(CANVAS * y)
        r = int(CANVAS * radius_px)
        star_draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 255, 255, opacity))
        background = Image.alpha_composite(background.convert("RGBA"), star).convert("RGB")

    return background.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    icon = build_icon()
    icon.save(OUTPUT, "PNG", optimize=True)
    print(f"Wrote {OUTPUT} ({icon.width}x{icon.height})")


if __name__ == "__main__":
    main()
