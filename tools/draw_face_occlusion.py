"""T7 approval material: the face-occlusion sheet.

The numbers in the report say how much face is hidden; this sheet shows *where*,
which is the only way to judge whether the allowance was spent well. Panels are
cropped to the head, so a difference of a few percent of the face is visible
rather than a few pixels on a full-body frame.

Reads the per-run `.local/fit-run/<tag>/face/sem-classes.npy` written by
tools/render_face_masks.py and the matching face-occlusion.json.

Run (host python, needs numpy + PIL):
  python tools/draw_face_occlusion.py --out <png> \
      "出厂模型|<run_dir>" "第一版拟合（被否决）|<run_dir>" "本轮拟合|<run_dir>"
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import face_overlay as fo  # noqa: E402

SCALE = 2
CAP_H = 52 * SCALE          # caption band above each panel
LEG_H = 30 * SCALE          # legend strip at the bottom
GAP = 12 * SCALE
MARGIN = 16 * SCALE
FONT_CANDIDATES = ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                   "C:/Windows/Fonts/Deng.ttf"]


def font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def panel(run_dir: Path) -> tuple[np.ndarray, dict]:
    cls = np.load(run_dir / "face/sem-classes.npy")
    data = json.loads((run_dir / "face-occlusion.json").read_text())
    x0, y0, x1, y1 = fo.face_bbox(cls)
    return cls[y0:y1, x0:x1], data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--total-limit", type=float, default=None)
    ap.add_argument("--added-limit", type=float, default=None)
    ap.add_argument("entries", nargs="+", help="label|run_dir")
    args = ap.parse_args()

    panels = []
    for entry in args.entries:
        label, _, raw = entry.partition("|")
        cls, data = panel(Path(raw))
        image = Image.fromarray(fo.to_rgba(cls), "RGBA")
        image = image.resize((image.width * SCALE, image.height * SCALE), Image.NEAREST)
        panels.append((label, data, image))

    h = max(p[2].height for p in panels)
    w = sum(p[2].width for p in panels) + GAP * (len(panels) - 1)
    sheet = Image.new("RGBA", (w + 2 * MARGIN, h + CAP_H + LEG_H + 2 * MARGIN),
                      (255, 255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    title_font, small = font(15 * SCALE // 2), font(13 * SCALE // 2)

    x = MARGIN
    for label, data, image in panels:
        sheet.alpha_composite(image, (x, MARGIN + CAP_H))
        total = data["occlusion_total"]
        new = data["occlusion_added_new"]
        flags = []
        if args.total_limit is not None and total > args.total_limit:
            flags.append("总遮脸超限")
        if args.added_limit is not None and new > args.added_limit:
            flags.append("新增层超限")
        verdict = ("  超限：" + "、".join(flags)) if flags else ""
        colour = (176, 42, 42) if flags else (26, 108, 62)
        draw.text((x, MARGIN + 2), label, font=title_font, fill=(28, 30, 36))
        draw.text((x, MARGIN + 24 * SCALE // 2),
                  f"总遮脸 {total:.4f}   新增层新遮 {new:.4f}{verdict}",
                  font=small, fill=colour)
        x += image.width + GAP

    # legend
    ly = MARGIN + CAP_H + h + 8 * SCALE // 2
    lx = MARGIN
    for index, text in fo.LEGEND:
        rgba = fo.PALETTE[index]
        if rgba[3]:
            draw.rectangle([lx, ly, lx + 9 * SCALE, ly + 9 * SCALE], fill=rgba[:3])
        lx += 12 * SCALE
        draw.text((lx, ly - 2 * SCALE // 2), text, font=small, fill=(60, 62, 70))
        lx += draw.textlength(text, font=small) + 14 * SCALE

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.convert("RGB").save(out)
    print(f"wrote {out} ({sheet.width}x{sheet.height})")
    return 0



if __name__ == "__main__":
    sys.exit(main())
