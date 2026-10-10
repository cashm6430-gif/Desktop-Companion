#!/usr/bin/env python
"""Consistency gate for a multi-view reference set.

Why this exists
---------------
The first preview runs produced a two-tailed model. The input set was
self-contradictory: the front reference (the only authoritative view) puts the
tail fluke on the RIGHT of the screen, and the AI-generated back view also put
it on the RIGHT. A single off-axis tail *must* land on opposite screen sides
when seen from the front and from the back, so the pair asserts a tail on each
side -- and that is exactly what the generator built.

Gates
-----
V4a  every supplied view carries a non-empty, plausible silhouette
V4b  the tail fluke lands on OPPOSITE screen sides in front and back
V4c  side views are declared silent on the tail's left/right, and are exempt

Why V4b reads pixels rather than silhouettes
--------------------------------------------
In the back view the fluke sits *inside* the hair outline: the silhouette is
almost perfectly self-symmetric (iou(back, flip(back)) ~= 0.94), so no
silhouette statistic can tell which side the tail is on -- that is what made an
earlier attempt at this measurement report "no signal". The fluke is however the
only large area of near-black blue (median RGB ~ (61,72,144)) while the hair is
much lighter (~(70,114,207)), so a luminance cut isolates it. Three thresholds
must agree before the verdict is trusted.

Usage
-----
  python tools/check_multiview_consistency.py                       # the shipped set
  python tools/check_multiview_consistency.py --back <flipped.png>  # a corrected set
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import diagnose_silhouette_fit as diag                      # noqa: E402  (canonical, iou)

MV = ROOT / "art/3d/whale-girl/multiview"
DEFAULT_FRONT = MV / "ref-front-psd.png"
DEFAULT_BACK = MV / "ref-back-ai.png"
DEFAULT_LEFT = MV / "ref-side-ai.png"

LUM_THRESHOLDS = (90.0, 100.0, 110.0)   # all three must agree
MIN_PIXELS = 500                        # below this the sample is too small to read
MIN_MARGIN = 0.05                       # |right share - 0.5| required to call a side


def load_rgba(path: Path) -> np.ndarray:
    return np.array(Image.open(path).convert("RGBA")).astype(np.int16)


def frame(a: np.ndarray) -> tuple[np.ndarray, int, int, int, float]:
    """Return (opacity, y0, y1, height, body axis) for one image.

    The axis is taken from the UPPER 40% of the silhouette (head and shoulders).
    That band is tail-free, so the axis does not move when the tail does.
    """
    op = a[..., 3] > 128
    ys = np.where(op.any(axis=1))[0]
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    h = y1 - y0
    up = op[y0:y0 + int(0.40 * h)]
    ux = np.where(up.any(axis=0))[0]
    axis = float((ux.min() + ux.max()) / 2) if len(ux) else a.shape[1] / 2.0
    return op, y0, y1, h, axis


def tail_side(path: Path, lum_cut: float) -> dict:
    """Which screen side holds the fluke, at one luminance threshold."""
    a = load_rgba(path)
    op, y0, y1, h, axis = frame(a)
    rgb = a[..., :3]
    lum = rgb.mean(axis=2)
    blue_dom = rgb[..., 2] - rgb[..., 0]
    rows = np.arange(a.shape[0])[:, None]
    lower = op & (rows >= y0 + 0.50 * h)
    deep = lower & (blue_dom > 0) & (lum < lum_cut)     # blue-dominant + dark
    cols = np.arange(a.shape[1])
    left = int(deep[:, cols < axis].sum())
    right = int(deep[:, cols >= axis].sum())
    total = left + right
    share = (right / total) if total else 0.5
    return {"side": "RIGHT" if share > 0.5 else "LEFT", "left": left, "right": right,
            "share": share, "n": total, "axis": axis}


def verdict(path: Path) -> dict:
    rows = [(t, tail_side(path, t)) for t in LUM_THRESHOLDS]
    sides = {r["side"] for _, r in rows}
    solid = [r for _, r in rows if r["n"] >= MIN_PIXELS and abs(r["share"] - 0.5) >= MIN_MARGIN]
    if not solid:
        return {"side": None, "reason": "no solid dark-blue mass found", "rows": rows}
    if len(sides) != 1:
        return {"side": None, "reason": f"thresholds disagree: {sorted(sides)}", "rows": rows}
    return {"side": solid[0]["side"], "reason": "ok", "rows": rows,
            "share": sum(r["share"] for _, r in rows) / len(rows)}


def self_symmetry(path: Path) -> float:
    m = diag.canonical(diag.load_png_mask(path))
    return diag.iou(m, m[:, ::-1])


def panel(path: Path, label: str, side: str | None, out_h: int = 420) -> Image.Image:
    a = load_rgba(path)
    op, y0, y1, h, axis = frame(a)
    im = Image.open(path).convert("RGBA")
    crop = im.crop((0, y0 + int(0.35 * h), im.width, min(im.height, y1 + 8)))
    sc = out_h / crop.height
    crop = crop.resize((max(1, int(crop.width * sc)), out_h), Image.LANCZOS)
    bg = Image.new("RGB", crop.size, (250, 250, 252))
    bg.paste(crop, (0, 0), crop)
    d = ImageDraw.Draw(bg)
    ax = axis * sc
    d.line([ax, 0, ax, out_h], fill=(226, 50, 50), width=2)
    d.text((7, 6), label, fill=(16, 20, 26))
    d.text((ax + 4, 24), "axis", fill=(226, 50, 50))
    d.text((7, out_h - 17), "LEFT", fill=(16, 20, 26))
    d.text((crop.width - 44, out_h - 17), "RIGHT", fill=(16, 20, 26))
    if side:
        d.text((7, 24), f"fluke: {side}", fill=(16, 20, 26))
    return bg


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--front", default=str(DEFAULT_FRONT))
    ap.add_argument("--back", default=str(DEFAULT_BACK))
    ap.add_argument("--left", default=str(DEFAULT_LEFT))
    ap.add_argument("--right", default=None)
    ap.add_argument("--sheet", default=str(ROOT / ".local/fit-run/multiview-consistency.png"))
    args = ap.parse_args()

    views = {"front": Path(args.front), "back": Path(args.back),
             "left": Path(args.left), "right": Path(args.right) if args.right else None}
    failures: list[str] = []
    results: dict[str, dict] = {}

    print("--- V4a silhouette sanity ---")
    for name, p in views.items():
        if p is None or not p.exists():
            if name in ("front", "back"):
                print(f"  {name}: MISSING ({p}) -> FAIL")
                failures.append(f"V4a:{name}")
            else:
                print(f"  {name}: not supplied (optional)")
            continue
        a = load_rgba(p)
        op, y0, y1, h, axis = frame(a)
        ok = bool(op.any()) and h > 100
        m = diag.canonical(diag.load_png_mask(p))
        print(f"  {name}: {p.name}  sil height={h}px axis={axis:.0f} "
              f"area={int(op.sum())}  canonical_self_IoU={diag.iou(m, m[:, ::-1]):.4f} "
              f"-> {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"V4a:{name}")

    print("\n--- V4b tail-fluke screen side (three luminance cuts must agree) ---")
    for name in ("front", "back"):
        p = views[name]
        if p is None or not p.exists():
            continue
        v = verdict(p)
        results[name] = v
        for t, r in v["rows"]:
            print(f"    {name:5s} lum<{t:5.0f}  L={r['left']:6d} R={r['right']:6d} "
                  f"right-share={r['share']:.3f} -> {r['side']}")
        if v["side"] is None:
            print(f"  {name}: UNREADABLE ({v['reason']}) -> FAIL")
            failures.append(f"V4b:{name}")
        else:
            print(f"  {name}: fluke on the {v['side']}  (mean right-share "
                  f"{v['share']:.3f}, self-IoU {self_symmetry(p):.4f})")

    if "front" in results and "back" in results:
        fs, bs = results["front"]["side"], results["back"]["side"]
        if fs and bs:
            ok = fs != bs
            print(f"\n  front={fs}  back={bs}  -> {'PASS (opposite, i.e. one tail)' if ok else 'FAIL (same side: two tails)'}")
            if not ok:
                failures.append("V4b:same-side")
        else:
            print("\n  cannot compare: one of the views is unreadable")
    elif views["back"] is None or not Path(args.back).exists():
        pass

    # ------------------------------------------------------- V4c declaration
    print("\n--- V4c side views are silent on the fluke's left/right (exempt) ---")
    if views["left"] and views["left"].exists():
        print(f"  {views['left'].name}: an orthographic side view projects a tail at +X and at -X")
        print("    to the SAME screen x, so it carries no left/right evidence either way.")
        print("    A mirrored side view is therefore an equivalent input, not a contradiction.")
        print("    (Verified in .local/tmp/tail-probe.py: screen x differs by < 1e-6.)")
    else:
        print("  no side view supplied")

    # -------------------------------------------------------------- evidence
    tiles = []
    for name in ("front", "back", "left"):
        p = views[name]
        if p is None or not p.exists():
            continue
        tiles.append(panel(p, f"{name}: {p.name}", results.get(name, {}).get("side")))
    if tiles:
        pad = 18
        W = sum(t.width for t in tiles) + pad * (len(tiles) + 1)
        H = max(t.height for t in tiles) + pad * 2
        sheet = Image.new("RGB", (W, H), (238, 240, 244))
        x = pad
        for t in tiles:
            sheet.paste(t, (x, pad))
            x += t.width + pad
        out = Path(args.sheet)
        if not out.is_absolute():
            out = Path.cwd() / out
        out.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(out)
        try:
            shown = out.relative_to(ROOT)
        except ValueError:
            shown = out
        print(f"\nwrote {shown}")

    print("\nMULTIVIEW_CONSISTENCY " + ("FAIL " + ",".join(failures) if failures
                                       else "PASS (V4a,V4b; V4c declared)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
