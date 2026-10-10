"""Magnified crop comparison of one vertical band across several images.

The silhouette metrics answer *where* the candidate and the reference disagree.
They cannot answer *what the disagreement is made of*: a band can be empty
because geometry is missing, or because the same amount of geometry sits
somewhere else. This tool puts the same band of several sources side by side at
high magnification so that question can be settled by looking instead of
guessing.

Alignment is the whole point, so the sources are put in the same canonical
frame the metrics use: cropped to the silhouette bbox, scaled to a common body
height, and centred on the *horizontal centroid* (not the bbox centre, which a
stray tail or a flared hem would bias). The sheet then crops a horizontal
window around the band's own content, so the magnification is real.

usage:
  python tools/crop_band_compare.py --band 0.72,1.04 \
      --src "front (authoritative)|art/.../ref-front-psd.png" \
      --src "left (AI extrapolated)|art/.../ref-side-ai.png" \
      --overlay "left reference|art/.../refs/left-mask.png" \
      --overlay "model left|art/.../render-neg_x.png" \
      --out .local/fit-run/hem-band.png
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
TARGET_H = 1000                     # body height in the canonical frame
BG = (246, 247, 249)
INK = (32, 36, 44)
MUTED = (96, 104, 112)
RULE = (206, 210, 216)


# ------------------------------------------------------------------- loading
def mask_of(rgba: np.ndarray) -> np.ndarray:
    """Same convention as diagnose_silhouette_fit.load_png_mask."""
    alpha = rgba[..., 3]
    if int(alpha.min()) >= 128:                      # opaque grayscale source
        return rgba[..., :3].mean(axis=2) > 128
    return alpha > 128


def composite(rgba: np.ndarray, bg=(255, 255, 255)) -> np.ndarray:
    a = rgba[..., 3:4].astype(np.float32) / 255.0
    rgb = rgba[..., :3].astype(np.float32)
    out = rgb * a + np.array(bg, dtype=np.float32) * (1.0 - a)
    return out.astype(np.uint8)


def nearest_resize(arr: np.ndarray, h: int, w: int) -> np.ndarray:
    """Nearest index mapping identical to canonical() in render_silhouette."""
    sh, sw = arr.shape[:2]
    ri = np.clip((np.arange(h) * (sh / h)).astype(int), 0, sh - 1)
    ci = np.clip((np.arange(w) * (sw / w)).astype(int), 0, sw - 1)
    return arr[ri][:, ci]


def canonical_pair(rgba: np.ndarray, body_h: int = TARGET_H):
    """Body-height scale, centroid-centred horizontal alignment.

    Returns (rgba, mask) at the canonical scale, cropped tight vertically to the
    body but with the full original horizontal span, plus the centroid column.
    """
    m = mask_of(rgba)
    ys, xs = np.where(m)
    if not len(ys):
        raise SystemExit("source has an empty mask")
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
    crop, cmask = rgba[y0:y1, x0:x1], m[y0:y1, x0:x1]
    h, w = crop.shape[:2]
    nw = max(1, int(round(w * body_h / h)))
    crop = nearest_resize(crop, body_h, nw)
    cmask = nearest_resize(cmask.astype(np.uint8), body_h, nw).astype(bool)
    cols = np.where(cmask.any(axis=0))[0]
    cx = float(cols.mean()) if len(cols) else nw / 2.0
    return crop, cmask, cx


def place(items: list[dict], band: tuple[float, float], margin: int,
          pad_frac: float = 0.06):
    """Put every source in one shared frame and crop the band window once."""
    h = TARGET_H
    a = max(0, int(round(band[0] * h)))
    b = min(h, int(round(band[1] * h)))

    xs_min, xs_max = 10 ** 9, -10 ** 9
    for it in items:
        for mask in it["masks"]:
            sub = mask[a:b]
            cols = np.where(sub.any(axis=0))[0]
            if len(cols):
                # offset of this source's own centroid within its crop
                off = it["cx"]
                xs_min = min(xs_min, cols.min() - off)
                xs_max = max(xs_max, cols.max() + 1 - off)

    if xs_min > xs_max:
        raise SystemExit("band is empty in every source")
    span = xs_max - xs_min
    m = int(round(max(margin, span * pad_frac)))
    lo, hi = int(np.floor(xs_min)) - m, int(np.ceil(xs_max)) + m

    width = hi - lo
    out = []
    for it in items:
        cx = it["cx"]
        rgba = it["rgba"]
        canvas = np.zeros((h, width, 4), dtype=np.uint8)
        cmask = np.zeros((h, width), dtype=bool)
        # place this source so its centroid lands on the shared origin
        x0 = int(round(-lo - cx))
        src_lo, src_hi = max(0, -x0), min(rgba.shape[1], width - x0)
        if src_hi > src_lo:
            dst = slice(x0 + src_lo, x0 + src_hi)
            canvas[:, dst] = rgba[:, src_lo:src_hi]
            for msk in it["masks"]:
                cmask[:, dst] |= msk[:, src_lo:src_hi]
        out.append({"label": it["label"],
                    "rgba": composite(canvas[a:b]),
                    "mask": cmask[a:b]})
    return out, (lo, hi)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--band", required=True, help="a,b as fractions of body height")
    ap.add_argument("--src", action="append", default=[],
                    help="label|path, repeatable; shown as a photographic row")
    ap.add_argument("--overlay", action="append", default=[],
                    help="label|path, repeatable; shown as a coloured overlay row")
    ap.add_argument("--cell", type=int, default=560, help="target tile width in px")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    parts = args.band.split(",")
    if len(parts) != 2:
        raise SystemExit("--band expects a,b")
    band = (float(parts[0]), float(parts[1]))
    if not 0.0 <= band[0] < band[1] <= 1.0:
        raise SystemExit("--band fractions must satisfy 0 <= a < b <= 1")

    def load(items: list[str]) -> list[dict]:
        out = []
        for item in items:
            label, _, path = item.partition("|")
            path = path or label
            rgba = np.array(Image.open(ROOT / path).convert("RGBA"))
            crop, cmask, cx = canonical_pair(rgba)
            out.append({"label": label, "rgba": crop, "masks": [cmask], "cx": cx})
        return out

    src_items = load(args.src)
    ov_items = load(args.overlay)

    rows: list[tuple[str, list[dict]]] = []
    if src_items:
        placed, window = place(src_items, band, margin=14)
        rows.append(("sources, band %.2f-%.2f of body height" % band, placed))
    if len(ov_items) == 2:
        placed, window = place(ov_items, band, margin=14)
        A, B = placed[0]["mask"], placed[1]["mask"]
        rgb = np.full(A.shape + (3,), 255, dtype=np.uint8)
        rgb[A & B] = (150, 156, 164)
        rgb[A & ~B] = (222, 88, 88)
        rgb[B & ~A] = (104, 146, 224)
        rows.append(("%s (red) vs %s (blue); grey = overlap"
                     % (placed[0]["label"], placed[1]["label"]),
                     [{"label": f"band {band[0]:.2f}-{band[1]:.2f}",
                       "rgba": rgb}]))

    # ---------------------------------------------------------------- sheet
    zoom = min(3.0, args.cell / max(1, rows[0][1][0]["rgba"].shape[1]))
    zoom = max(zoom, 0.4)
    gutter, pad, hdr = 16, 20, 24

    def scaled(rgb: np.ndarray) -> Image.Image:
        im = Image.fromarray(rgb)
        return im.resize((max(1, int(rgb.shape[1] * zoom)),
                          max(1, int(rgb.shape[0] * zoom))), Image.NEAREST)

    images = [(cap, [(it["label"], scaled(it["rgba"])) for it in row])
              for cap, row in rows]
    col_w = max(im.width for _, row in images for _, im in row)
    row_h = [max(im.height for _, im in row) for _, row in images]
    n_col = max(len(row) for _, row in images)
    width = pad * 2 + n_col * col_w + (n_col - 1) * gutter
    height = pad * 2 + sum(hdr + rh + 22 for rh in row_h) + (len(images) - 1) * gutter
    sheet = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(sheet)

    y = pad
    for ri, (cap, row) in enumerate(images):
        d.text((pad, y + 4), f"{cap}   (x{zoom:.2f})", fill=INK)
        y += hdr
        x = pad
        for lab, im in row:
            sheet.paste(im, (x, y))
            d.rectangle([x, y, x + im.width - 1, y + im.height - 1], outline=RULE)
            d.text((x, y + im.height + 5), lab, fill=MUTED)
            x += col_w + gutter
        y += row_h[ri] + 22 + gutter
    sheet.save(ROOT / args.out)
    print(f"wrote {args.out}  ({sheet.width}x{sheet.height}, x{zoom:.2f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
