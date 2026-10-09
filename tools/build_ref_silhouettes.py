"""T1: build normalized reference silhouettes for silhouette-driven template fitting.

Reads the authoritative front view (PSD full-body composite) plus the
AI-extrapolated left/back views, extracts the alpha silhouette from each,
normalizes all three into one canonical frame (height = TARGET_H px,
bottom-aligned on BOTTOM_Y, horizontally centred) and writes binary masks,
a metadata sidecar and a verification sheet.

Verification gates printed to stdout (T1):
  V1a  every saved mask is strictly binary (values in {0, 255})
  V1b  every normalized silhouette is exactly TARGET_H tall
  V1c  each silhouette's widest row sits in the lower half (skirt band),
       i.e. the mask is a plausible standing character, not a crop artefact
  V1d  raw (pre-normalization) aspect ratios are reported for human review

Run: <venv python> tools/build_ref_silhouettes.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "art/3d/whale-girl/silhouette-fit/refs"

SOURCES = {
    "front": ROOT / "art/3d/whale-girl-head-r2/alignment-fullbody.png",
    "left":  ROOT / "art/3d/whale-girl/multiview/ref-side-ai.png",
    "back":  ROOT / "art/3d/whale-girl/multiview/ref-back-ai.png",
}

CANVAS = 1024
TARGET_H = 1000
BOTTOM_Y = 1012
ALPHA_T = 128


def extract_silhouette(path: Path) -> tuple[np.ndarray, dict]:
    img = Image.open(path).convert("RGBA")
    alpha = np.array(img.getchannel("A"))
    ys, xs = np.where(alpha > ALPHA_T)
    if len(xs) == 0:
        raise RuntimeError(f"empty silhouette: {path}")
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()) + 1, int(ys.min()), int(ys.max()) + 1
    raw_w, raw_h = x1 - x0, y1 - y0
    meta = {
        "source": str(path.relative_to(ROOT)),
        "raw_bbox": [x0, y0, x1, y1],
        "raw_w": raw_w,
        "raw_h": raw_h,
        "raw_aspect": round(raw_w / raw_h, 4),
    }
    return alpha[y0:y1, x0:x1], meta


def normalize(cropped: np.ndarray) -> tuple[np.ndarray, float]:
    h, w = cropped.shape
    scale = TARGET_H / h
    new_w = max(1, int(round(w * scale)))
    resized = Image.fromarray(cropped).resize((new_w, TARGET_H), Image.LANCZOS)
    binary = (np.array(resized) > ALPHA_T).astype(np.uint8) * 255

    canvas = np.zeros((CANVAS, CANVAS), dtype=np.uint8)
    # Centre on the mask centroid, not the bounding box: an asymmetric
    # appendage (the tail) inflates the bbox and would bias the whole frame.
    cols = np.where(binary.any(axis=0))[0]
    cx = float(cols.mean()) if len(cols) else new_w / 2.0
    x = int(round(CANVAS / 2.0 - cx))
    x = max(0, min(CANVAS - new_w, x))
    canvas[BOTTOM_Y - TARGET_H:BOTTOM_Y, x:x + new_w] = binary
    return canvas, scale


def profile_report(mask: np.ndarray) -> dict:
    rows = (mask > 0).sum(axis=1)
    widest = int(rows.argmax())
    frac_from_bottom = round((BOTTOM_Y - widest) / TARGET_H, 4)
    return {
        "widest_row_y": widest,
        "widest_px": int(rows[widest]),
        "widest_frac_from_bottom": frac_from_bottom,
        "widest_in_lower_half": bool(frac_from_bottom < 0.5),
    }


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    metas: dict[str, dict] = {}
    masks: dict[str, np.ndarray] = {}
    failures: list[str] = []

    for name, path in SOURCES.items():
        if not path.exists():
            print(f"V1x FAIL  source missing: {path}")
            return 1
        cropped, meta = extract_silhouette(path)
        mask, scale = normalize(cropped)
        meta["scale"] = round(scale, 6)
        meta.update(profile_report(mask))
        masks[name] = mask
        metas[name] = meta
        # Save as RGBA: the mask travels in BOTH rgb and alpha so that any
        # consumer works (PIL convert("L") uses rgb, Blender's loader uses alpha;
        # a plain grayscale png would make Blender see an opaque image).
        Image.fromarray(np.dstack([mask, mask, mask, mask]), "RGBA").save(
            OUT / f"{name}-mask.png")

    print("--- V1a binary check ---")
    for name, mask in masks.items():
        vals = np.unique(mask)
        ok = set(vals.tolist()) <= {0, 255}
        print(f"  {name}: values={vals.tolist()} -> {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"V1a:{name}")

    print("--- V1b normalized height check ---")
    for name, mask in masks.items():
        ys, _ = np.where(mask > 0)
        height = int(ys.max() - ys.min() + 1)
        ok = height == TARGET_H
        print(f"  {name}: height={height} (target {TARGET_H}) -> {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"V1b:{name}")

    print("--- V1c widest row in lower half ---")
    for name, meta in metas.items():
        ok = meta["widest_in_lower_half"]
        print(f"  {name}: widest at {meta['widest_frac_from_bottom']:.2f} from bottom "
              f"-> {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"V1c:{name}")

    print("--- V1d raw aspect ratios (human review) ---")
    for name, meta in metas.items():
        print(f"  {name}: raw {meta['raw_w']}x{meta['raw_h']} aspect={meta['raw_aspect']} "
              f"scale={meta['scale']}")

    cell, pad = 320, 20
    sheet = Image.new("RGB", (cell * 3 + pad * 4, cell + pad * 2 + 24), (245, 246, 248))
    d = ImageDraw.Draw(sheet)
    for i, name in enumerate(["front", "left", "back"]):
        thumb = Image.fromarray(masks[name]).resize((cell - 10, cell - 10), Image.LANCZOS)
        rgb = Image.new("RGB", thumb.size, (255, 255, 255))
        rgb.paste(Image.new("RGB", thumb.size, (40, 70, 130)), (0, 0), thumb)
        cx = pad + i * (cell + pad)
        sheet.paste(rgb, (cx, pad + 14))
        d.rectangle([cx - 1, pad + 13, cx + cell - 11, pad + 14 + cell - 10], outline=(200, 204, 212))
        d.text((cx, pad - 2), f"{name}  (h={TARGET_H}px)", fill=(40, 44, 52))
    sheet.save(OUT / "refs-sheet.png")

    (OUT / "meta.json").write_text(json.dumps(metas, indent=1, ensure_ascii=False))
    print(f"\nwrote {len(masks)} masks + meta.json + refs-sheet.png -> {OUT.relative_to(ROOT)}")
    if failures:
        print(f"T1_RESULT FAIL {failures}")
        return 1
    print("T1_RESULT PASS (V1a..V1d)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
