"""Compose the level-B approval sheet from tools/render_fit_views.py output.

One row per artefact (shipped, fitted), one column per view, plus the head
close-up that makes "nothing new hangs in front of the face" checkable by eye
rather than by proxy.

Run (host python, needs PIL):
  python tools/draw_fit_approval.py --out <png> "出厂模型|<prefix>" "本轮拟合|<prefix>"
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
COLUMNS = [("front", "正面"), ("left", "左侧"), ("head", "头部特写")]
FONT_CANDIDATES = ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                   "C:/Windows/Fonts/Deng.ttf"]
PAD, GAP, LABEL_H = 18, 14, 34


def font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("rows", nargs="+", help="label|render_prefix")
    args = ap.parse_args()

    rows = []
    for entry in args.rows:
        label, _, prefix = entry.partition("|")
        images = []
        for key, _cn in COLUMNS:
            path = Path(f"{prefix}-{key}.png")
            images.append(Image.open(path).convert("RGB"))
        rows.append((label, images))

    col_w = max(img.width for _label, imgs in rows for img in imgs)
    row_h = max(img.height for _label, imgs in rows for img in imgs)
    width = PAD * 2 + col_w * len(COLUMNS) + GAP * (len(COLUMNS) - 1)
    height = PAD * 2 + (row_h + LABEL_H) * len(rows) + GAP * (len(rows) - 1)
    sheet = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    title, small = font(17), font(15)

    for index, (_key, caption) in enumerate(COLUMNS):
        x = PAD + index * (col_w + GAP)
        draw.text((x, PAD - 22), caption, font=title, fill=(40, 44, 52))

    y = PAD
    for label, images in rows:
        draw.text((PAD, y), label, font=title, fill=(40, 44, 52))
        y += LABEL_H
        for index, image in enumerate(images):
            sheet.paste(image, (PAD + index * (col_w + GAP), y))
        y += row_h + GAP

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f"wrote {out} ({sheet.width}x{sheet.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
