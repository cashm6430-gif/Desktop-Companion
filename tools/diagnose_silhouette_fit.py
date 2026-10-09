"""T2.5: decompose the silhouette mismatch into interpretable parts.

The plain overlay (red = reference only, blue = model only, grey = overlap)
shows *that* the fit is bad but not *what* is bad. This tool splits the same
two masks into:

  * coverage / precision / miss / excess, globally and per horizontal band
  * connected blobs of the miss region (what parts are absent) and of the
    excess region (what parts stick out), each with area, bbox and side
  * the outline band (dilate - erode) restricted IoU, which is the metric the
    eye actually reads -- a solid-mass IoU can look decent while every
    silhouette-defining detail is wrong

Verification gates (V2e -- instrument, not model):
  V2e1  this tool must reproduce the T2 baseline IoUs exactly (<= 1e-3), which
        proves the masks it reads are the ones the baseline was computed from
  V2e2  arithmetic identities hold: overlap + miss == ref area, and the
        labelled blobs account for the whole miss region (+/- small residual)
  V2e3  the shapes are non-identical in every view (miss > 0), otherwise the
        comparison would be vacuous

Run: <venv python> tools/diagnose_silhouette_fit.py
"""
from __future__ import annotations

import json
import sys
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
REF_DIR = ROOT / "art/3d/whale-girl/silhouette-fit/refs"
BASE_DIR = ROOT / "art/3d/whale-girl/silhouette-fit/t2-baseline"
RUN_ROOT_ALT = ROOT / ".local/fit-run"        # where multi-run comparisons land

CANVAS = 2048
TARGET_H = 1000
BOTTOM_Y = 2024
OUTLINE_R = 14          # outline band half-width in px
MIN_BLOB_PX = 1500      # ignore specks below this many full-res pixels
REDUCE = 4              # grid stride used for labelling / distance fields

VIEWS = {"front": "neg_y", "left": "neg_x", "back": "pos_y"}
BANDS = [("head-top", 0.00, 0.20), ("head-low", 0.20, 0.40),
         ("torso", 0.40, 0.60), ("skirt", 0.60, 0.80), ("hem/legs", 0.80, 1.00)]


# ---------------------------------------------------------------- mask basics
def load_png_mask(path: Path) -> np.ndarray:
    px = np.array(Image.open(path).convert("RGBA"))
    alpha = px[..., 3]
    if int(alpha.min()) >= 128:                      # opaque grayscale source
        return px[..., :3].mean(axis=2) > 128
    return alpha > 128


def canonical(mask: np.ndarray) -> np.ndarray:
    """Identical to tools/render_silhouette.py:canonical (nearest + centroid)."""
    ys, xs = np.where(mask)
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
    crop = mask[y0:y1, x0:x1]
    h, w = crop.shape
    nw = max(1, int(round(w * TARGET_H / h)))
    ri = np.clip((np.arange(TARGET_H) * (h / TARGET_H)).astype(int), 0, h - 1)
    ci = np.clip((np.arange(nw) * (w / nw)).astype(int), 0, w - 1)
    resized = crop[ri][:, ci]
    out = np.zeros((CANVAS, CANVAS), dtype=bool)
    cols = np.where(resized.any(axis=0))[0]
    cx = float(cols.mean()) if len(cols) else nw / 2.0
    x = max(0, min(CANVAS - nw, int(round(CANVAS / 2.0 - cx))))
    out[BOTTOM_Y - TARGET_H:BOTTOM_Y, x:x + nw] = resized
    return out


def iou(a: np.ndarray, b: np.ndarray) -> float:
    u = int((a | b).sum())
    return float((a & b).sum()) / u if u else 0.0


# ------------------------------------------------------------- grid reduction
def reduce_any(mask: np.ndarray, d: int = REDUCE) -> np.ndarray:
    h, w = mask.shape
    h2, w2 = h // d, w // d
    return mask[:h2 * d, :w2 * d].reshape(h2, d, w2, d).any(axis=(1, 3))


def _bfs(free: np.ndarray, seeds) -> np.ndarray:
    """Multi-source BFS distance (px, 4-neighbour) over a boolean 'free' grid."""
    h, w = free.shape
    dist = np.full((h, w), -1, dtype=np.int32)
    q = deque()
    for y, x in seeds:
        if free[y, x] and dist[y, x] < 0:
            dist[y, x] = 0
            q.append((y, x))
    while q:
        y, x = q.popleft()
        nd = dist[y, x] + 1
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < h and 0 <= nx < w and free[ny, nx] and dist[ny, nx] < 0:
                dist[ny, nx] = nd
                q.append((ny, nx))
    return dist


def label_blobs(mask: np.ndarray, min_px: int) -> tuple[list[dict], int]:
    """Connected components on the reduced grid, areas returned in full-res px."""
    red = reduce_any(mask)
    h, w = red.shape
    seen = np.zeros_like(red)
    blobs, covered = [], 0
    d2 = REDUCE * REDUCE
    for sy in range(h):
        for sx in range(w):
            if not red[sy, sx] or seen[sy, sx]:
                continue
            q = deque([(sy, sx)])
            seen[sy, sx] = True
            cells = []
            while q:
                y, x = q.popleft()
                cells.append((y, x))
                for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                    if 0 <= ny < h and 0 <= nx < w and red[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
            area = len(cells) * d2
            if area < min_px:
                continue
            ys = [c[0] for c in cells]
            xs = [c[1] for c in cells]
            col_c = float(np.mean(xs)) * REDUCE
            blobs.append({
                "area_px": area,
                "centroid": [round(col_c, 1), round(float(np.mean(ys)) * REDUCE, 1)],
                "bbox": [xs and int(min(xs)) * REDUCE, int(min(ys)) * REDUCE,
                         int(max(xs) + 1) * REDUCE, int(max(ys) + 1) * REDUCE],
                "side": "left" if col_c < CANVAS / 2 else "right",
            })
            covered += area
    blobs.sort(key=lambda b: -b["area_px"])
    return blobs, covered


# ------------------------------------------------------------------- metrics
def analyse(ref: np.ndarray, model: np.ndarray) -> dict:
    inter = ref & model
    miss = ref & ~model
    excess = model & ~ref
    ref_a, mod_a, int_a = int(ref.sum()), int(model.sum()), int(inter.sum())

    ys, _ = np.where(ref)
    y0, y1 = int(ys.min()), int(ys.max())

    bands = []
    for name, a, b in BANDS:
        ba = y0 + int(round((y1 - y0) * a))
        bb = y0 + int(round((y1 - y0) * b))
        r = ref[ba:bb]
        m = model[ba:bb]
        ra, ma, ia = int(r.sum()), int(m.sum()), int((r & m).sum())
        union = ra + ma - ia
        bands.append({
            "band": name, "ref_px": ra, "model_px": ma, "overlap_px": ia,
            "coverage": round(ia / ra, 4) if ra else None,
            "excess_ratio": round((ma - ia) / ra, 4) if ra else None,
            # band IoU is symmetric, so inflating the model cannot raise it
            "iou": round(ia / union, 4) if union else None,
        })

    # outline band: dilate(ref, R) minus erode(ref, R), and the same for the
    # model. A one-sided "recall" of the reference outline rewards simply
    # growing the model, so the diagnostic also reports the mirror term and
    # their harmonic mean.
    g_ref = reduce_any(ref)
    g_mod = reduce_any(model)
    R = OUTLINE_R // REDUCE

    def _band(g):
        d_fg = _bfs(np.ones_like(g), zip(*np.where(g)))
        d_bg = _bfs(np.ones_like(g), zip(*np.where(~g)))
        return ((d_fg >= 0) & (d_fg <= R) & ~((d_bg >= 0) & (d_bg > R)))

    ref_band = _band(g_ref)
    mod_band = _band(g_mod)

    def _ratio(num, den):
        return round(float(num) / float(den), 4) if den else None

    o_recall = _ratio((ref_band & g_mod).sum(), ref_band.sum())
    o_precision = _ratio((mod_band & g_ref).sum(), mod_band.sum())
    if o_recall and o_precision:
        o_f1 = round(2 * o_recall * o_precision / (o_recall + o_precision), 4)
    else:
        o_f1 = None

    cols_ref = np.where(miss.any(axis=0))[0]
    miss_l = int(miss[:, :CANVAS // 2].sum())
    miss_r = int(miss[:, CANVAS // 2:].sum())

    blobs_miss, cov_miss = label_blobs(miss, MIN_BLOB_PX)
    blobs_exc, cov_exc = label_blobs(excess, MIN_BLOB_PX)

    return {
        "iou": round(iou(ref, model), 4),
        "ref_area": ref_a, "model_area": mod_a, "overlap": int_a,
        "miss_px": int(miss.sum()), "excess_px": int(excess.sum()),
        "coverage": round(int_a / ref_a, 4),          # how much of the ref exists
        "precision": round(int_a / mod_a, 4),         # how much of the model is right
        "miss_frac_of_ref": round(int(miss.sum()) / ref_a, 4),
        "excess_frac_of_ref": round(int(excess.sum()) / ref_a, 4),
        "bands": bands,
        "outline_band_px": int(ref_band.sum()),
        "outline_recall": o_recall,
        "outline_precision": o_precision,
        "outline_f1": o_f1,
        "miss_side": {"left_px": miss_l, "right_px": miss_r,
                      "asymmetry": round(abs(miss_l - miss_r) / max(1, miss_l + miss_r), 4),
                      "miss_span_x": [int(cols_ref.min()) if len(cols_ref) else None,
                                      int(cols_ref.max()) if len(cols_ref) else None]},
        "miss_blobs": blobs_miss,
        "miss_blob_covered_px": cov_miss,
        "excess_blobs": blobs_exc,
        "excess_blob_covered_px": cov_exc,
    }


# ---------------------------------------------------------------- diagnostic sheet
PALETTE = [(220, 70, 70), (240, 150, 40), (200, 60, 160), (60, 160, 90),
           (140, 90, 200), (30, 130, 190), (170, 120, 60), (90, 90, 100)]


def content_crop(views_masks: dict, pad: int = 30) -> tuple[int, int, int]:
    """One square crop shared by every view of a run, so the panels in a row
    keep a common scale and the (now generous) canvas does not leave most of
    each cell empty."""
    ys, xs = [], []
    for mask in views_masks.values():
        for m in (mask if isinstance(mask, (list, tuple)) else [mask]):
            yy, xx = np.where(m)
            if len(yy):
                ys += [int(yy.min()), int(yy.max()) + 1]
                xs += [int(xx.min()), int(xx.max()) + 1]
    if not ys:
        return 0, 0, CANVAS
    y0, y1, x0, x1 = min(ys), max(ys), min(xs), max(xs)
    side = max(y1 - y0, x1 - x0) + pad * 2
    side = min(CANVAS, side)
    cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
    oy = max(0, min(CANVAS - side, cy - side // 2))
    ox = max(0, min(CANVAS - side, cx - side // 2))
    return ox, oy, side


def draw_view(sheet, d, ox: int, oy: int, cell: int, ref, model, res: dict, crop):
    cx0, cy0, side = crop
    s = cell / side
    inter, miss, exc = ref & model, ref & ~model, model & ~ref
    rgb = np.full((CANVAS, CANVAS, 3), 255, dtype=np.uint8)
    rgb[inter] = (152, 158, 166)
    rgb[exc] = (122, 158, 226)
    rgb[miss] = (226, 96, 96)
    tile = Image.fromarray(rgb).crop((cx0, cy0, cx0 + side, cy0 + side))
    sheet.paste(tile.resize((cell, cell), Image.BOX), (ox, oy))
    for i, b in enumerate(res["miss_blobs"][:8]):
        x0, y0, x1, y1 = b["bbox"]
        if x1 < cx0 or x0 > cx0 + side or y1 < cy0 or y0 > cy0 + side:
            continue
        col = PALETTE[i % len(PALETTE)]
        d.rectangle([ox + (x0 - cx0) * s, oy + (y0 - cy0) * s,
                     ox + (x1 - cx0) * s, oy + (y1 - cy0) * s], outline=col, width=2)
        d.text((ox + (x0 - cx0) * s + 3, oy + (y0 - cy0) * s + 2), f"{i + 1}", fill=col)
    d.rectangle([ox, oy, ox + cell, oy + cell], outline=(205, 210, 216))


def draw_bars(d: ImageDraw.ImageDraw, ox: int, oy: int, cell: int, res: dict):
    n = len(res["bands"])
    bw = (cell - 24) / n
    base = oy + cell - 26
    top = oy + 30
    d.line([ox + 12, base, ox + cell - 12, base], fill=(180, 186, 194))
    for i, b in enumerate(res["bands"]):
        x = ox + 12 + i * bw
        cov = b["coverage"] or 0.0
        h = (base - top) * cov
        d.rectangle([x + 4, base - h, x + bw - 6, base], fill=(60, 130, 200))
        # excess is drawn on the same axis (fraction of the reference band area)
        if b["excess_ratio"] and b["excess_ratio"] > 0:
            eh = min(base - top, (base - top) * min(b["excess_ratio"], 1.0))
            d.rectangle([x + 4, base - eh, x + bw - 6, base], outline=(214, 74, 74))
        d.text((x + 3, base + 3), b["band"].split("-")[0][:6], fill=(90, 96, 104))
        d.text((x + 3, base - h - 13), f"{cov:.2f}", fill=(40, 60, 90))


def sils_in(run_dir: Path) -> dict:
    """T2 writes its .npy masks next to the report; the fit runner nests them
    under sil/, so accept either layout."""
    src = run_dir / "sil" if (run_dir / "sil").is_dir() else run_dir
    return {n: np.load(src / f"sil-{v}.npy") for n, v in VIEWS.items()}


def load_run(run_dir: Path, refs: dict) -> dict:
    sils = sils_in(run_dir)
    return {n: analyse(refs[n], sils[n]) for n in VIEWS}


def parse_runs(argv: list[str]) -> list[tuple[str, Path]]:
    runs: list[tuple[str, Path]] = []
    for item in argv:
        label, _, path = item.partition("|")
        path = path or label
        runs.append((label if path != label else Path(path).name, Path(path)))
    return runs or [("shipped baseline", BASE_DIR)]


def main() -> int:
    runs = parse_runs(sys.argv[1:])
    refs = {n: canonical(load_png_mask(REF_DIR / f"{n}-mask.png")) for n in VIEWS}
    failures: list[str] = []

    # V2e1 can only run against a directory that carries the T2 baseline sidecar
    for label, run_dir in runs:
        sidecar = run_dir / "t2-baseline.json"
        if not sidecar.exists():
            continue
        base = json.loads(sidecar.read_text())
        print(f"--- V2e1 reproduction of the T2 baseline IoU ({label}) ---")
        for name, view in VIEWS.items():
            got = iou(refs[name], sils_in(run_dir)[name])
            want = base["baseline_iou"][name]
            ok = abs(got - want) <= 1e-3
            print(f"  {name}: recomputed={got:.4f} baseline={want:.4f} -> "
                  f"{'PASS' if ok else 'FAIL'}")
            if not ok:
                failures.append(f"V2e1:{name}")

    products: list[tuple[str, dict, dict]] = []
    for label, run_dir in runs:
        results = load_run(run_dir, refs)
        print(f"\n--- decomposition: {label} ---")
        for n, r in results.items():
            ident = r["overlap"] + r["miss_px"] == r["ref_area"]
            residual = r["miss_px"] - r["miss_blob_covered_px"]
            blob_ok = residual <= MIN_BLOB_PX * (len(r["miss_blobs"]) + 4)
            nonvacuous = r["miss_px"] > 0 and r["excess_px"] > 0
            ok = ident and blob_ok and nonvacuous
            if not ok:
                failures.append(f"V2e2/3:{label}:{n}")
            print(f"  [{n}] iou={r['iou']:.4f} coverage={r['coverage']:.4f} "
                  f"precision={r['precision']:.4f} outline r/p/f1="
                  f"{r['outline_recall']}/{r['outline_precision']}/{r['outline_f1']} "
                  f"[{'PASS' if ok else 'FAIL'} V2e2/V2e3]")
            print(f"       miss={r['miss_frac_of_ref']:.1%} of ref "
                  f"(excess {r['excess_frac_of_ref']:.1%}), "
                  f"miss side L/R={r['miss_side']['left_px']}/{r['miss_side']['right_px']} "
                  f"(asym {r['miss_side']['asymmetry']:.2f}), blobs={len(r['miss_blobs'])}")
            for i, b in enumerate(r["miss_blobs"][:4]):
                print(f"       blob{i + 1}: {b['area_px'] / r['ref_area']:.1%} of ref "
                      f"at x~{b['centroid'][0]:.0f} y~{b['centroid'][1]:.0f} ({b['side']})")
        products.append((label, results, sils_in(run_dir)))

    # --------------------------------------------------------------- sheet
    cell, pad, lab = 340, 20, 34
    width = cell * 3 + pad * 4
    height = pad + len(products) * (lab + cell + 8 + cell + pad) + 60
    sheet = Image.new("RGB", (width, height), (246, 247, 249))
    d = ImageDraw.Draw(sheet)
    y = pad
    for label, results, sils in products:
        d.text((pad, y + 3), label, fill=(24, 28, 34))
        y += lab
        crop = content_crop({n: [refs[n], sils[n]] for n in VIEWS})
        for i, n in enumerate(VIEWS):
            ox = pad + i * (cell + pad)
            d.text((ox, y - 12),
                   f"{n}  iou={results[n]['iou']:.3f}  cov={results[n]['coverage']:.3f}  "
                   f"outline f1={results[n]['outline_f1']:.3f}", fill=(60, 66, 74))
            draw_view(sheet, d, ox, y, cell, refs[n], sils[n], results[n], crop)
        y += cell + 8
        for i, n in enumerate(VIEWS):
            draw_bars(d, pad + i * (cell + pad), y, cell, results[n])
        y += cell + pad

    for i, (col, text) in enumerate((( (226, 96, 96), "red = only in reference (missing part)"),
                                     ((122, 158, 226), "blue = only in model (excess)"),
                                     ((152, 158, 166), "grey = overlap"))):
        d.rectangle([pad + i * 330, y + 6, pad + i * 330 + 12, y + 16], fill=col)
        d.text((pad + i * 330 + 18, y + 4), text, fill=(70, 76, 84))
    d.text((pad, y + 22), "bars: blue = share of the reference band covered, "
                          "red outline = model excess over that band", fill=(70, 76, 84))

    out = BASE_DIR / "t2-diagnosis-sheet.png"
    sheet.save(out)
    if len(products) > 1:
        alt = RUN_ROOT_ALT / "fit-compare-sheet.png"
        alt.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(alt)
        print(f"\nwrote {alt.relative_to(ROOT)}")

    print(f"wrote {out.relative_to(ROOT)}")
    if failures:
        print(f"T2.5_RESULT FAIL {failures}")
        return 1
    print("T2.5_RESULT PASS (V2e1..V2e3)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
