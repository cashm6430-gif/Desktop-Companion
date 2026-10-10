"""T4 driver: evaluate parameter candidates by silhouette fit.

For each candidate parameter set this script
  1. deforms the authoring blend and exports a candidate GLB (Blender, headless),
  2. renders the three reference views of that GLB (Blender, headless),
  3. scores the result against the T1 reference silhouettes.

Scoring follows what the T2.5 diagnosis showed: a global IoU is dominated by
the solid mass (two independently drawn reference views already only agree at
0.795), while the eye reads the *outline* and the *per-band coverage*. So the
objective is a blend of IoU, outline-band recall and the worst band's coverage,
and the report also carries hard floors so a candidate that buys outline at the
cost of, say, the legs is visible rather than silently accepted.

Run (host python, needs numpy + PIL):
  python tools/fit_silhouette_search.py --param hair_side_flare --values 0,0.06,0.12,0.18
  python tools/fit_silhouette_search.py --candidates candidates.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import numpy as np  # noqa: E402

import diagnose_silhouette_fit as diag  # noqa: E402

BLENDER = Path("D:/tool/blender/blender.exe")
BLEND = ROOT / "art/3d/whale-girl-head-r2/character-round3.blend"
RUNNER = ROOT / "tools/fit_silhouette_template.py"
RENDERER = ROOT / "tools/render_silhouette.py"
RUN_ROOT = ROOT / ".local/fit-run"
# Bump whenever the canonical frame, the render framing or the metrics change:
# cached candidate silhouettes are only comparable inside one generation.
# v3 = camera framed on the skinned asset instead of the whole imported scene
#      (measured effect on the baseline IoU: 0.7363 -> 0.7361).
# v4 = the side reference is paired with pos_x instead of neg_x. Every cached
#      candidate was scored against a mirrored side reference; the side IoU of
#      the shipped model moves 0.6221 -> 0.4958.
CACHE_VERSION = 4

# Objective weights. Every term is SYMMETRIC: the first coordinate-descent run
# showed what one-sided terms do -- "outline recall" and "band coverage" both
# rise when the model is simply inflated, so the optimiser flared the skirt and
# lengthened the hair until the score read 0.74 while the global IoU had fallen
# from 0.756 to 0.627. Outline F1 and per-band IoU cannot be bought that way.
W_IOU, W_OUTLINE_F1, W_BAND = 0.35, 0.40, 0.25
FLOORS = [("front", "hem/legs", 0.66), ("front", "torso", 0.80),
          ("left", "torso", 0.72), ("left", "hem/legs", 0.02),
          ("back", "hem/legs", 0.78), ("back", "torso", 0.75)]


def run_blender(script: Path, extra: list[str], tag: str) -> str:
    cmd = [str(BLENDER), "--background", "--factory-startup"]
    if script is RUNNER:
        cmd.append(str(BLEND))
    cmd += ["--python", str(script), "--"] + extra
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT),
                          encoding="utf-8", errors="replace")
    out = (proc.stdout or "") + (proc.stderr or "")
    (RUN_ROOT / tag / "blender.log").write_text(out, encoding="utf-8")
    return out


def evaluate(params: dict, tag: str, refs: dict, force: bool = False) -> dict:
    run_dir = RUN_ROOT / tag
    run_dir.mkdir(parents=True, exist_ok=True)
    glb = run_dir / "candidate.glb"
    sil = run_dir / "sil"

    stamp = run_dir / "done.json"
    fresh = stamp.exists() and json.loads(stamp.read_text()).get("v") == CACHE_VERSION
    if force or not fresh:
        (run_dir / "params.json").write_text(json.dumps(params, indent=1))
        log = run_blender(RUNNER, ["--params", str(run_dir / "params.json"),
                                   "--out", str(glb),
                                   "--report", str(run_dir / "runner.json")], tag)
        if "T3_RUNNER PASS" not in log:
            return {"tag": tag, "params": params, "ok": False, "stage": "deform",
                    "log_tail": log[-800:]}
        log = run_blender(RENDERER, [str(glb), str(sil)], tag)
        if "T2_RESULT PASS" not in log:
            return {"tag": tag, "params": params, "ok": False, "stage": "render",
                    "log_tail": log[-800:]}
        (run_dir / "done.json").write_text(
            json.dumps({"params": params, "v": CACHE_VERSION}, indent=1))

    sils = {n: np.load(sil / f"sil-{v}.npy") for n, v in diag.VIEWS.items()}
    if any(sils[n].shape != refs[n].shape for n in diag.VIEWS):
        return {"tag": tag, "params": params, "ok": False, "stage": "stale-frame"}
    return score_sils(sils, refs, params, tag)


def score_sils(sils: dict, refs: dict, params: dict, tag: str) -> dict:
    """Score one set of rendered silhouettes against the T1 references."""
    views = {n: diag.analyse(refs[n], sils[n]) for n in diag.VIEWS}

    mean_iou = float(np.mean([views[n]["iou"] for n in views]))
    mean_f1 = float(np.mean([views[n]["outline_f1"] for n in views]))
    band_min = {n: min(b["iou"] for b in views[n]["bands"]) for n in views}
    mean_band = float(np.mean(list(band_min.values())))
    score = W_IOU * mean_iou + W_OUTLINE_F1 * mean_f1 + W_BAND * mean_band

    breached = []
    for view, band, floor in FLOORS:
        row = next(b for b in views[view]["bands"] if b["band"] == band)
        if (row["coverage"] or 0.0) < floor:
            breached.append(f"{view}:{band}={row['coverage']:.3f}<{floor}")

    return {"tag": tag, "params": params, "ok": True,
            "coverage": {n: views[n]["coverage"] for n in views},
            "iou": {n: views[n]["iou"] for n in views},
            "outline_recall": {n: views[n]["outline_recall"] for n in views},
            "outline_f1": {n: views[n]["outline_f1"] for n in views},
            "band_min": band_min,
            "miss_frac": {n: views[n]["miss_frac_of_ref"] for n in views},
            "excess_frac": {n: views[n]["excess_frac_of_ref"] for n in views},
            "mean_iou": round(mean_iou, 4), "mean_outline": round(mean_f1, 4),
            "mean_band": round(mean_band, 4), "score": round(score, 4),
            "floor_breaches": breached}


def greedy_descent(base: dict, refs: dict, order: list[str], points: int,
                   force: bool) -> tuple[dict, list[dict]]:
    """One pass of coordinate descent: for each parameter, sample the range,
    keep the best value, move on. Deterministic (fixed order, fixed grid), so a
    run is reproducible and a parameter never buys score by wrecking a floor."""
    import whale_fit_deform as deform

    best_params = {k: v[0] for k, v in deform.PARAM_SPEC.items()}
    best_params.update(base)
    trace = []

    def score_of(params: dict) -> dict:
        digest = hashlib.md5(
            json.dumps(sorted(params.items())).encode()).hexdigest()[:10]
        return evaluate(params, f"d-{digest}", refs, force=force)

    current = score_of(best_params)
    if not current.get("ok"):
        return best_params, trace
    print(f"[start] score={current['score']:.4f} {json.dumps(best_params)}")

    for param in order:
        _default, lo, hi = deform.PARAM_SPEC[param][:3]
        values = sorted({round(lo + (hi - lo) * i / (points - 1), 6)
                         for i in range(points)} | {best_params[param]})
        # A gated parameter (an offset that only acts on created layers) is
        # invisible while its gate is closed, so a plain coordinate sweep would
        # score it as inert and move on. Open the gate for the evaluation and
        # let the accepted candidate carry that move with it.
        gate = deform.PARAM_COUPLING.get(param)
        opened = bool(gate) and abs(best_params.get(gate, 0.0)) < 1e-9
        step = []
        for value in values:
            cand = dict(best_params)
            cand[param] = value
            if opened:
                cand[gate] = deform.PARAM_SPEC[gate][2]
            res = score_of(cand)
            if res.get("ok"):
                step.append((res["score"], value, cand, res))
        if not step:
            continue
        step.sort(key=lambda s: (-s[0], s[1]))
        gain = step[0][0] - current["score"]
        chosen = step[0][2]
        if chosen != best_params:
            best_params = chosen                  # adopt the whole move, gate included
        current = step[0][3]
        trace.append({"param": param, "value": step[0][1], "score": step[0][0],
                      "gain": round(gain, 4), "gate_opened": opened,
                      "tried": [{"value": v, "score": s} for s, v, _, _ in step]})
        print(f"[{param}] -> {step[0][1]:.4f}{' (gate opened)' if opened else ''}  "
              f"score {current['score']:.4f} ({gain:+.4f})  "
              f"iou={current['mean_iou']:.4f} outline={current['mean_outline']:.4f} "
              f"bandmin={current['mean_band']:.4f} "
              f"floors={current['floor_breaches'] or 'ok'}")
    return best_params, trace


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--param", default="hair_side_flare")
    ap.add_argument("--values", default="0,0.06,0.12,0.18")
    ap.add_argument("--base", default="{}", help="JSON dict of fixed parameters")
    ap.add_argument("--candidates", default=None,
                    help="JSON file holding a list of parameter dicts")
    ap.add_argument("--descent", action="store_true", help="greedy coordinate descent")
    ap.add_argument("--rescore", action="store_true", help="re-rank every cached candidate")
    ap.add_argument("--order", default=None, help="comma-separated parameter order")
    ap.add_argument("--points", type=int, default=4)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    refs = {n: diag.canonical(diag.load_png_mask(diag.REF_DIR / f"{n}-mask.png"))
            for n in diag.VIEWS}

    if args.descent:
        import whale_fit_deform as deform
        # crown_taper/foot_length/tail_flare are measured to have little or
        # negative authority; they stay in the space but go last.
        default_order = ["hair_side_flare", "hair_back_volume", "skirt_flare",
                         "hair_length", "skirt_drop", "foot_height", "bang_length",
                         "ahoge_scale", "foot_length", "tail_flare", "crown_taper",
                         "front_lock_extra", "front_lock_fwd", "front_lock_drop",
                         "side_lock_extra", "side_lock_spread", "side_lock_drop",
                         "back_lock_extra", "back_lock_back", "back_lock_drop"]
        order = args.order.split(",") if args.order else default_order
        order = [p for p in order if p in deform.PARAM_SPEC]
        best, trace = greedy_descent(json.loads(args.base), refs, order,
                                     args.points, args.force)
        (RUN_ROOT / "descent.json").write_text(
            json.dumps({"best": best, "trace": trace}, indent=1))
        print(f"\nDESCENT best score params={json.dumps(best)}")
        print(f"wrote {RUN_ROOT / 'descent.json'}")
        return 0

    if args.rescore:
        rows = []
        base_sil = ROOT / "art/3d/whale-girl/silhouette-fit/t2-baseline"
        if (base_sil / "sil-neg_y.npy").exists():
            sils = {n: np.load(base_sil / f"sil-{v}.npy") for n, v in diag.VIEWS.items()}
            rows.append(score_sils(sils, refs, {"_baseline": True}, "baseline(shipped)"))
        for d in sorted(RUN_ROOT.iterdir()):
            if not (d / "done.json").exists() or not (d / "sil").is_dir():
                continue
            params = json.loads((d / "done.json").read_text())["params"]
            sils = {n: np.load(d / "sil" / f"sil-{v}.npy") for n, v in diag.VIEWS.items()}
            if any(sils[n].shape != refs[n].shape for n in diag.VIEWS):
                continue                       # older frame generation
            rows.append(score_sils(sils, refs, params, d.name))
        rows.sort(key=lambda r: -r["score"])
        for r in rows:
            print(f"  {r['score']:.4f}  iou={r['mean_iou']:.4f} f1={r['mean_outline']:.4f} "
                  f"bandmin={r['mean_band']:.4f}  miss={ {k: round(v, 3) for k, v in r['miss_frac'].items()} }"
                  f"  floors={r['floor_breaches'] or 'ok'}")
            print(f"          {r['tag']}  {json.dumps({k: round(v, 4) for k, v in r['params'].items() if not k.startswith('_')})}")
        (RUN_ROOT / "rescore-table.json").write_text(json.dumps(rows, indent=1))
        print(f"\nwrote {RUN_ROOT / 'rescore-table.json'}")
        return 0

    if args.candidates:
        cands = json.loads(Path(args.candidates).read_text())
    else:
        base = json.loads(args.base)
        vals = [float(v) for v in args.values.split(",")]
        cands = [{**base, args.param: v} for v in vals]

    results = []
    for i, params in enumerate(cands):
        # key the run directory by the parameter set itself: a positional tag
        # would let a later sweep silently reuse an earlier candidate's GLB
        digest = hashlib.md5(
            json.dumps(sorted(params.items())).encode()).hexdigest()[:10]
        tag = f"c{i:02d}-{digest}"
        res = evaluate(params, tag, refs, force=args.force)
        results.append(res)
        if res["ok"]:
            print(f"[{tag}] score={res['score']:.4f} iou={res['mean_iou']:.4f} "
                  f"outline={res['mean_outline']:.4f} bandmin={res['mean_band']:.4f} "
                  f"miss={ {k: round(v, 3) for k, v in res['miss_frac'].items()} } "
                  f"floors={res['floor_breaches'] or 'ok'}")
        else:
            print(f"[{tag}] FAILED at {res['stage']}")
            print(res["log_tail"])
        print(f"        params={json.dumps(params)}")

    good = [r for r in results if r["ok"]]
    if good:
        best = max(good, key=lambda r: r["score"])
        print(f"\nbest {best['tag']} score={best['score']:.4f} params={json.dumps(best['params'])}")
    (RUN_ROOT / "search-table.json").write_text(json.dumps(results, indent=1))
    print(f"wrote {RUN_ROOT / 'search-table.json'}")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
