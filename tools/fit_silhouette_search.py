"""T4 driver: evaluate parameter candidates by silhouette fit.

For each candidate parameter set this script
  1. deforms the authoring blend and exports a candidate GLB (Blender, headless),
  2. renders the three reference views of that GLB (Blender, headless),
  3. measures how much of the face the hair hides, from the front (Blender),
  4. scores the result against the T1 reference silhouettes.

Scoring follows what the T2.5 diagnosis showed: a global IoU is dominated by
the solid mass (two independently drawn reference views already only agree at
0.795), while the eye reads the *outline* and the *per-band coverage*. So the
objective is a blend of IoU, outline-band recall and the worst band's coverage,
and the report also carries hard floors so a candidate that buys outline at the
cost of, say, the legs is visible rather than silently accepted.

Face occlusion is a *hard constraint*, not a score term. The silhouette
objective is structurally blind to it: a lock falling in front of the face adds
exactly the same outline as a lock falling beside it, so the first descent
bought its side-view gain by dropping the fringe over the eyes and swinging the
new locks across the face -- and scored better for it. Rejected candidates are
still evaluated and reported, they just cannot be selected.

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
FACE_MASKER = ROOT / "tools/render_face_masks.py"
RUN_ROOT = ROOT / ".local/fit-run"
# Bump whenever the canonical frame, the render framing or the metrics change:
# cached candidate silhouettes are only comparable inside one generation.
# v3 = camera framed on the skinned asset instead of the whole imported scene
#      (measured effect on the baseline IoU: 0.7363 -> 0.7361).
# v4 = the side reference is paired with pos_x instead of neg_x. Every cached
#      candidate was scored against a mirrored side reference; the side IoU of
#      the shipped model moves 0.6221 -> 0.4958.
# v5 = face occlusion is measured per candidate and enforced as a hard
#      constraint. No silhouette value changes; the cached occlusion numbers do.
# v6 = the constraint gates on the *increment* the fitting layers add
#      (`occlusion_added_new`), not on "an added layer is front-most". The two
#      differ wherever a duplicated lock lands on the lock it was copied from,
#      which is the common case and would have rejected a harmless candidate.
CACHE_VERSION = 6

# Objective weights. Every term is SYMMETRIC: the first coordinate-descent run
# showed what one-sided terms do -- "outline recall" and "band coverage" both
# rise when the model is simply inflated, so the optimiser flared the skirt and
# lengthened the hair until the score read 0.74 while the global IoU had fallen
# from 0.756 to 0.627. Outline F1 and per-band IoU cannot be bought that way.
W_IOU, W_OUTLINE_F1, W_BAND = 0.35, 0.40, 0.25
FLOORS = [("front", "hem/legs", 0.66), ("front", "torso", 0.80),
          ("left", "torso", 0.72), ("left", "hem/legs", 0.02),
          ("back", "hem/legs", 0.78), ("back", "torso", 0.75)]

# Face-occlusion limits, measured on the shipped character (tools/render_face_masks.py):
#   fringe alone      0.3597 of the front-visible head
#   all hair together 0.5637
#   the fit's own extra layers 0.0000 (they do not exist until it creates them)
# The rejected first-fit solution: fringe 0.5551, added layers 0.2540, total
# 0.7335 -- +0.1951 from dropping the fringe and +0.2540 from the new locks.
# Sensitivity, measured: bang_length 0 -> 0.05 already takes the total to 0.6025,
# so this effectively pins the fringe at its shipped length. That is the intent:
# a fringe reaching the eyebrows is what "covers the face" looks like.
OCC_TOTAL_LIMIT = 0.5937      # shipped 0.5637 + 0.03 of tolerance
OCC_ADDED_LIMIT = 0.02        # new face the fitting layers may take away

# Which views the objective is allowed to score. Set by --views.
#
# Only the FRONT reference is authoritative: it is the PSD composite. left and
# back are ImageGen extrapolations, i.e. a model's guess at a view nobody drew.
# Scoring all three equally cost more than it looked: decomposed per view, the
# three-view fit is a *regression* on the only reference that is not a guess
# (front 0.5984 -> 0.5750, IoU 0.7736 -> 0.7546, outline F1 0.4652 -> 0.4249)
# while it gains almost everything on the guessed left view (0.3537 -> 0.5354)
# and is nearly flat on the guessed back (0.6598 -> 0.6714). The extra volume
# that fills the guessed profile is what reads as a wig in 3D.
# `back` stays in by default because its gains come from real back-of-head
# volume and back locks, which are visually benign; `left` is the one whose
# reward is only payable by swinging locks out in front of the character.
OBJECTIVE_VIEWS: tuple[str, ...] = ("front", "left", "back")


BLEND_SCRIPTS = (RUNNER, FACE_MASKER)      # these take the blend as a file argument


def blender_cmd(script: Path, extra: list[str]) -> list[str]:
    cmd = [str(BLENDER), "--background", "--factory-startup"]
    if script in BLEND_SCRIPTS:
        cmd.append(str(BLEND))
    return cmd + ["--python", str(script), "--"] + extra


def run_blender(script: Path, extra: list[str], tag: str, log_name: str = "blender.log") -> str:
    proc, handle, path = start_blender(script, extra, tag, log_name)
    return finish_blender(proc, handle, path)


def start_blender(script: Path, extra: list[str], tag: str, log_name: str):
    """Start a Blender pass without waiting; returns the (proc, handle, log) triple."""
    log_path = RUN_ROOT / tag / log_name
    log_path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(blender_cmd(script, extra), stdout=handle,
                            stderr=subprocess.STDOUT, cwd=str(ROOT), text=True,
                            encoding="utf-8", errors="replace")
    return proc, handle, log_path


def finish_blender(proc, handle, log_path: Path) -> str:
    proc.wait()
    handle.close()
    return log_path.read_text(encoding="utf-8", errors="replace")


def occlusion_breaches(occ: dict) -> list[str]:
    """Which face-occlusion limits this candidate breaks (empty == allowed)."""
    out = []
    total = occ.get("occlusion_total")
    added = occ.get("occlusion_added_new")
    if total is None or added is None:
        return ["no measurement"]
    if total > OCC_TOTAL_LIMIT:
        out.append(f"total {total:.4f}>{OCC_TOTAL_LIMIT}")
    if added > OCC_ADDED_LIMIT:
        out.append(f"new {added:.4f}>{OCC_ADDED_LIMIT}")
    return out


def load_occlusion(run_dir: Path) -> dict | None:
    path = run_dir / "face-occlusion.json"
    if not path.exists():
        return None
    data = json.loads(path.read_text())
    if data.get("failures"):
        return None
    return data


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
        # The silhouette render and the face pass are independent (different
        # cameras, different outputs, both read-only on the blend), so they run
        # as two Blender processes at once -- that is a third of the wall clock
        # saved on every one of the ~80 candidates a descent evaluates.
        render_job = start_blender(RENDERER, [str(glb), str(sil)], tag, "render.log")
        face_job = start_blender(FACE_MASKER, [str(BLEND), str(run_dir),
                                               str(run_dir / "params.json")],
                                 tag, "face.log")
        log = finish_blender(*render_job)
        face_log = finish_blender(*face_job)
        if "T2_RESULT PASS" not in log:
            return {"tag": tag, "params": params, "ok": False, "stage": "render",
                    "log_tail": log[-800:]}
        if "T7_FACE PASS" not in face_log:
            return {"tag": tag, "params": params, "ok": False, "stage": "face",
                    "log_tail": face_log[-800:]}
        (run_dir / "done.json").write_text(
            json.dumps({"params": params, "v": CACHE_VERSION}, indent=1))

    sils = {n: np.load(sil / f"sil-{v}.npy") for n, v in diag.VIEWS.items()}
    if any(sils[n].shape != refs[n].shape for n in diag.VIEWS):
        return {"tag": tag, "params": params, "ok": False, "stage": "stale-frame"}
    res = score_sils(sils, refs, params, tag)
    attrs = attach_occlusion(res, load_occlusion(run_dir), params, tag)
    return attrs


def attach_occlusion(res: dict, occ: dict | None, params: dict, tag: str) -> dict:
    """Fold the face measurement into the candidate result and decide feasibility.

    Kept separate from score_sils() so `--rescore` can re-rank cached candidates
    without re-running Blender -- it reads the same face-occlusion.json.
    """
    if occ is None:
        res.update({"occlusion": {}, "occ_total": None, "occ_added": None,
                    "occ_added_new": None,
                    "occ_breaches": ["no measurement"], "feasible": False})
        return res
    res.update({"occlusion": occ.get("occlusion", {}),
                "occ_total": occ.get("occlusion_total"),
                "occ_added": occ.get("occlusion_added"),
                "occ_added_new": occ.get("occlusion_added_new")})
    breaches = occlusion_breaches(occ)
    res["occ_breaches"] = breaches
    res["feasible"] = not breaches
    return res


def score_sils(sils: dict, refs: dict, params: dict, tag: str,
               views: tuple[str, ...] | None = None) -> dict:
    """Score one set of rendered silhouettes against the T1 references.

    Every view is measured and reported; only `views` (default OBJECTIVE_VIEWS)
    are averaged into the score. Keeping the unweighted numbers in the result
    is deliberate -- the whole reason the objective was wrong for a while is
    that a per-view decomposition was not visible at decision time.
    """
    measured = {n: diag.analyse(refs[n], sils[n]) for n in diag.VIEWS}
    use = tuple(views or OBJECTIVE_VIEWS)

    mean_iou = float(np.mean([measured[n]["iou"] for n in use]))
    mean_f1 = float(np.mean([measured[n]["outline_f1"] for n in use]))
    band_min = {n: min(b["iou"] for b in measured[n]["bands"]) for n in measured}
    mean_band = float(np.mean([band_min[n] for n in use]))
    score = W_IOU * mean_iou + W_OUTLINE_F1 * mean_f1 + W_BAND * mean_band

    breached = []
    for view, band, floor in FLOORS:
        row = next(b for b in measured[view]["bands"] if b["band"] == band)
        if (row["coverage"] or 0.0) < floor:
            breached.append(f"{view}:{band}={row['coverage']:.3f}<{floor}")

    return {"tag": tag, "params": params, "ok": True, "objective_views": list(use),
            "coverage": {n: measured[n]["coverage"] for n in measured},
            "iou": {n: measured[n]["iou"] for n in measured},
            "outline_recall": {n: measured[n]["outline_recall"] for n in measured},
            "outline_f1": {n: measured[n]["outline_f1"] for n in measured},
            "band_min": band_min,
            "miss_frac": {n: measured[n]["miss_frac_of_ref"] for n in measured},
            "excess_frac": {n: measured[n]["excess_frac_of_ref"] for n in measured},
            "mean_iou": round(mean_iou, 4), "mean_outline": round(mean_f1, 4),
            "mean_band": round(mean_band, 4), "score": round(score, 4),
            "floor_breaches": breached}


def greedy_descent(base: dict, refs: dict, order: list[str], points: int,
                   force: bool) -> tuple[dict, list[dict]]:
    """One pass of coordinate descent: for each parameter, sample the range,
    keep the best *feasible* value, move on. Deterministic (fixed order, fixed
    grid), so a run is reproducible and a parameter never buys score by wrecking
    a floor or by covering the face."""
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
    if not current.get("feasible"):
        print(f"[start] INFEASIBLE base: {current['occ_breaches']} "
              f"(occlusion total={current['occ_total']}, new={current['occ_added_new']})")
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
        infeasible = []
        for value in values:
            cand = dict(best_params)
            cand[param] = value
            if opened:
                cand[gate] = deform.PARAM_SPEC[gate][2]
            res = score_of(cand)
            if not res.get("ok"):
                continue
            if not res.get("feasible"):
                infeasible.append({"value": value, "score": res["score"],
                                   "occ_total": res["occ_total"],
                                   "occ_added": res["occ_added"],
                                   "occ_added_new": res["occ_added_new"],
                                   "breaches": res["occ_breaches"]})
                continue
            step.append((res["score"], value, cand, res))
        if not step:
            note = f"  ({len(infeasible)} rejected on face occlusion)" if infeasible else ""
            print(f"[{param}] no feasible value, kept {best_params[param]:.4f}{note}")
            trace.append({"param": param, "value": best_params[param],
                          "score": current["score"], "gain": 0.0, "kept": True,
                          "rejected": infeasible})
            continue
        # Ties are broken towards the value already in hand, not towards the
        # lowest: a parameter the objective cannot see (foot_length in a
        # front/back pass) scores identically everywhere, and "smallest" then
        # walks it to an extreme for nothing -- it halved the shoe length once.
        step.sort(key=lambda s: (-s[0], abs(s[1] - best_params[param]), s[1]))
        best_score, best_value, chosen, result = step[0]
        gain = best_score - current["score"]
        # Only move on an improvement. Adopting the best sample regardless (what
        # this used to do) lets a step that costs score hand the descent a worse
        # position than it already had -- and a later step is not obliged to
        # recover it. It did not: 0.6469 was reached and 0.6232 was reported.
        if gain > 1e-9:
            if chosen != best_params:
                best_params = chosen              # adopt the whole move, gate included
            current = result
        trace.append({"param": param, "value": best_value, "score": best_score,
                      "gain": round(gain, 4), "gate_opened": opened,
                      "adopted": gain > 1e-9,
                      "tried": [{"value": v, "score": s} for s, v, _, _ in step],
                      "rejected": infeasible})
        print(f"[{param}] -> {best_value:.4f}{' (gate opened)' if opened else ''}  "
              f"score {best_score:.4f} ({gain:+.4f})  "
              f"{'adopted' if gain > 1e-9 else 'not adopted'}  "
              f"iou={current['mean_iou']:.4f} outline={current['mean_outline']:.4f} "
              f"bandmin={current['mean_band']:.4f} "
              f"occ={current['occ_total']:.4f}(new {current['occ_added_new']:.4f})  "
              f"floors={current['floor_breaches'] or 'ok'}"
              f"{f'  [{len(infeasible)} rejected]' if infeasible else ''}")
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
    ap.add_argument("--views", default=None,
                    help="comma-separated views the objective scores, e.g. 'front,back' "
                         "(default: front,left,back)")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    global OBJECTIVE_VIEWS
    if args.views:
        chosen = tuple(v.strip() for v in args.views.split(",") if v.strip())
        unknown = [v for v in chosen if v not in diag.VIEWS]
        if unknown or not chosen:
            print(f"unknown views {unknown}; pick from {list(diag.VIEWS)}")
            return 2
        OBJECTIVE_VIEWS = chosen

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
        print(f"[objective] views={','.join(OBJECTIVE_VIEWS)}  "
              f"weights ioU={W_IOU} outline_f1={W_OUTLINE_F1} band={W_BAND}")
        best, trace = greedy_descent(json.loads(args.base), refs, order,
                                     args.points, args.force)
        (RUN_ROOT / "descent.json").write_text(
            json.dumps({"best": best, "trace": trace,
                        "objective_views": list(OBJECTIVE_VIEWS)}, indent=1))
        print(f"\nDESCENT best score params={json.dumps(best)}")
        print(f"wrote {RUN_ROOT / 'descent.json'}")
        return 0

    if args.rescore:
        rows = []
        base_sil = ROOT / "art/3d/whale-girl/silhouette-fit/t2-baseline"
        if (base_sil / "sil-neg_y.npy").exists():
            sils = {n: np.load(base_sil / f"sil-{v}.npy") for n, v in diag.VIEWS.items()}
            res = score_sils(sils, refs, {"_baseline": True}, "baseline(shipped)")
            rows.append(attach_occlusion(res, load_occlusion(RUN_ROOT / "face-factory"),
                                         {}, "baseline(shipped)"))
        for d in sorted(RUN_ROOT.iterdir()):
            if not (d / "done.json").exists() or not (d / "sil").is_dir():
                continue
            stamp = json.loads((d / "done.json").read_text())
            params = stamp["params"]
            sils = {n: np.load(d / "sil" / f"sil-{v}.npy") for n, v in diag.VIEWS.items()}
            if any(sils[n].shape != refs[n].shape for n in diag.VIEWS):
                continue                       # older frame generation
            res = score_sils(sils, refs, params, d.name)
            rows.append(attach_occlusion(res, load_occlusion(d), params, d.name))
        rows.sort(key=lambda r: (-r["feasible"], -r["score"]))
        for r in rows:
            occ = ("none" if r["occ_total"] is None
                   else f"{r['occ_total']:.4f}/new {r['occ_added_new']:.4f}")
            print(f"  {r['score']:.4f}  iou={r['mean_iou']:.4f} f1={r['mean_outline']:.4f} "
                  f"bandmin={r['mean_band']:.4f}  occ={occ}"
                  f"  {'FEASIBLE' if r['feasible'] else 'REJECTED ' + str(r['occ_breaches'])}"
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
                  f"occ={res['occ_total']:.4f}(new {res['occ_added_new']:.4f}) "
                  f"{'FEASIBLE' if res['feasible'] else 'REJECTED ' + str(res['occ_breaches'])} "
                  f"floors={res['floor_breaches'] or 'ok'}")
        else:
            print(f"[{tag}] FAILED at {res['stage']}")
            print(res["log_tail"])
        print(f"        params={json.dumps(params)}")

    good = [r for r in results if r["ok"]]
    ok_feasible = [r for r in good if r.get("feasible")]
    if ok_feasible:
        best = max(ok_feasible, key=lambda r: r["score"])
        print(f"\nbest feasible {best['tag']} score={best['score']:.4f} "
              f"occ={best['occ_total']:.4f}(new {best['occ_added_new']:.4f}) "
              f"params={json.dumps(best['params'])}")
    elif good:
        print(f"\nno feasible candidate ({len(good)} evaluated, all cover too much face)")
    (RUN_ROOT / "search-table.json").write_text(json.dumps(results, indent=1))
    print(f"wrote {RUN_ROOT / 'search-table.json'}")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
