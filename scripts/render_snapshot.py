#!/usr/bin/env python3
"""Renders a `tui_snapshot.py --json` frame to a PNG.

    python3 scripts/tui_snapshot.py --json -- python3 ... | \
        python3 scripts/render_snapshot.py artifacts/browse.png

This exists so the terminal UI can be reviewed as an image in CI and in pull
requests, without a real terminal. It is a development tool, not part of the
shipped binary.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import matplotlib

from PIL import Image, ImageDraw, ImageFont

FONT_DIR = Path(matplotlib.__file__).parent / "mpl-data" / "fonts" / "ttf"
REGULAR = FONT_DIR / "DejaVuSansMono.ttf"
BOLD = FONT_DIR / "DejaVuSansMono-Bold.ttf"

CELL_W = 9
CELL_H = 19
PAD = 14


def xterm(index: int) -> tuple[int, int, int]:
    base = [
        (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
        (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
        (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
        (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
    ]
    if index < 16:
        return base[index]
    if index < 232:
        index -= 16
        steps = [0, 95, 135, 175, 215, 255]
        return steps[index // 36], steps[(index // 6) % 6], steps[index % 6]
    level = 8 + (index - 232) * 10
    return level, level, level


def parse_sgr(sgr: str) -> tuple[tuple[int, int, int], tuple[int, int, int], bool, bool]:
    fg, bg, bold, dim = (229, 229, 229), (18, 18, 18), False, False
    parts = [int(p) for p in sgr.split(";") if p]
    i = 0
    while i < len(parts):
        code = parts[i]
        if code == 1:
            bold = True
        elif code == 2:
            dim = True
        elif code == 38 and i + 2 < len(parts):
            fg = xterm(parts[i + 2])
            i += 2
        elif code == 48 and i + 2 < len(parts):
            bg = xterm(parts[i + 2])
            i += 2
        elif code == 39:
            fg = (229, 229, 229)
        elif code == 49:
            bg = (18, 18, 18)
        i += 1
    if dim:
        fg = tuple(int(c * 0.72) for c in fg)
    return fg, bg, bold, dim


def main() -> int:
    out = Path(sys.argv[1])
    payload = json.load(sys.stdin)
    cols, rows = payload["cols"], payload["rows"]
    width = cols * CELL_W + PAD * 2
    height = rows * CELL_H + PAD * 2
    image = Image.new("RGB", (width, height), (10, 10, 10))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(str(REGULAR), 15)
    bold_font = ImageFont.truetype(str(BOLD), 15)

    for y, row in enumerate(payload["cells"]):
        for x, (sgr, text) in enumerate(row):
            fg, bg, bold, _ = parse_sgr(sgr)
            left = PAD + x * CELL_W
            top = PAD + y * CELL_H
            draw.rectangle([left, top, left + CELL_W, top + CELL_H], fill=bg)
            if not text or text == " ":
                continue
            draw.text((left, top), text, font=bold_font if bold else font, fill=fg)

    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
