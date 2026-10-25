"""Measure the formal grass material through Core without rendering or rebuilding.

Usage: python tools/review_grass_pin.py <new-output.json>

The probe changes only an in-memory model. It checks the pinned lower stem and
every other drawable while sweeping the two existing flex axes at the approved
invitation pose. This proves input isolation; it does not certify absolute palm
contact, rendered pixels, spring response, pointer routing or visual approval.
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

import numpy as np

from probe_cubism_core import NativeModel, ProbeError, Vec2, digest, find_core, model_family


ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "assets/live2d/whale-girl/whale-girl-layered-draft.model3.json"
CLIP = ROOT / "assets/motions/grass-touch.motion.json"
GRASS = "ArtMeshHandwearR2"
EXPECTED_MOC = "f6929a38ae8f3b460d69666afd3f10f1af67172abec051eb6265b7f5c16fc781"
SOURCE_RECT = (797.0, 494.0, 922.0, 896.0)
SOURCE_ANCHORS = {"root": (913.0, 852.0), "mid": (884.0, 669.0), "tip": (806.0, 498.0)}
FLEX_AXES = ("ParamGrassSwing", "ParamGrassTipBend")
PIN_TOLERANCE_SOURCE_PX = 0.002


def material_frame(native, drawable):
    """Same UV/material contract used by CubismCanvas diagnostics."""
    pointer = native.get("DrawableVertexUvs")[drawable]
    uvs = np.asarray([[pointer[i].X, pointer[i].Y]
                      for i in range(native.vertex_counts[drawable])], dtype=float)
    lower, upper = uvs.min(axis=0), uvs.max(axis=0)
    if not np.all(upper > lower):
        raise ProbeError("Grass UV rectangle is degenerate.")
    x0, y0, x1, y1 = SOURCE_RECT
    material = np.column_stack((x0 + (x1-x0)*(uvs[:, 0]-lower[0])/(upper[0]-lower[0]),
                                y0 + (y1-y0)*(upper[1]-uvs[:, 1])/(upper[1]-lower[1])))
    pointer = native.get("DrawableIndices")[drawable]
    triangles = np.asarray([pointer[i] for i in range(native.index_counts[drawable])],
                           dtype=int).reshape(-1, 3)
    return material, uvs, triangles


def resolve_anchor(material, triangles, source):
    """Barycentric inside point or closest triangle edge, never extrapolated."""
    target = np.asarray(source, dtype=float)
    best = None
    cross = lambda a, b: a[0]*b[1]-a[1]*b[0]
    for triangle in triangles:
        p = material[triangle]
        area = cross(p[1]-p[0], p[2]-p[0])
        if abs(area) < 1e-12:
            continue
        b = cross(target-p[0], p[2]-p[0])/area
        c = cross(p[1]-p[0], target-p[0])/area
        weights = np.asarray((1-b-c, b, c))
        distance = 0.0
        if min(weights) < 0.0:
            distance = float("inf")
            for edge in range(3):
                following = (edge+1) % 3
                delta = p[following]-p[edge]
                length = float(delta@delta)
                t = float(np.clip((target-p[edge])@delta/length, 0, 1)) if length else 0.0
                candidate = p[edge]+t*delta-target
                squared = float(candidate@candidate)
                if squared < distance:
                    distance = squared
                    weights = np.zeros(3)
                    weights[edge], weights[following] = 1-t, t
        if best is None or distance < best[0]:
            best = distance, triangle.copy(), weights.copy()
            if distance == 0:
                break
    if best is None or best[0] >= 9:
        raise ProbeError("Grass source anchor is farther than 3px from its mesh.")
    distance, triangle, weights = best
    return {"indices": triangle.tolist(), "weights": weights.tolist(),
            "requested_source": list(source), "resolved_source": (weights@material[triangle]).tolist(),
            "source_residual_px": distance**0.5}


def anchor_point(vertices, anchor, ppu):
    point = np.asarray(anchor["weights"])@vertices[np.asarray(anchor["indices"])]
    # Canvas scales x right and flips Native y up to screen y down. Translation
    # and a positive common scale do not affect the direction/sign measurement.
    return point*np.asarray((ppu, -ppu))


def snapshot(native, parameters):
    native.update({"name": "grass-pin-probe", "parameters": parameters, "part_opacities": {}})
    result = [np.asarray(vertices, dtype=float) for vertices in native.positions()]
    if any(not np.isfinite(vertices).all() for vertices in result):
        raise ProbeError("Core returned non-finite vertices.")
    return result


def run(output, core_path=None):
    output = Path(output).resolve()
    if output.exists():
        raise ProbeError("Use a new output path; existing evidence is preserved.")
    moc_path, family = model_family(MODEL)
    core_path = find_core(core_path)
    protected = {Path(item["path"]) for item in family} | {CLIP, core_path, Path(__file__).resolve()}
    if output in protected:
        raise ProbeError("Output must not overwrite an input.")
    before = {path: digest(path) for path in protected}
    if before[moc_path] != EXPECTED_MOC:
        raise ProbeError("Formal MOC differs from the reviewed source-material contract.")
    clip = json.loads(CLIP.read_text(encoding="utf-8-sig"))
    hold_time = clip["interaction"]["enterEnd"]
    held = next(row["parameters"] for row in clip["keyframes"] if row["time"] == hold_time)
    if clip["approval"] != "approved" or held["ParamHandRGrip"] != 1 or held["ParamGrassVisible"] != 1:
        raise ProbeError("Expected the approved invitation with the palm gripping a visible root.")
    native = NativeModel(core_path, moc_path)
    index = native.drawable_ids.index(GRASS)
    if native.vertex_counts[index] != 878:
        raise ProbeError("Formal grass topology differs from the 878-vertex contract.")
    for axis in FLEX_AXES:
        if any(native.ranges[axis][key] != expected for key, expected in
               (("min", -1), ("max", 1), ("default", 0))):
            raise ProbeError("Native flex parameter range changed: "+axis)
    size, origin, ppu = Vec2(), Vec2(), ctypes.c_float()
    native.api["ReadCanvasInfo"](native.model, ctypes.byref(size), ctypes.byref(origin), ctypes.byref(ppu))
    material, uvs, triangles = material_frame(native, index)
    anchors = {name: resolve_anchor(material, triangles, source) for name, source in SOURCE_ANCHORS.items()}
    root, tip = np.asarray(SOURCE_ANCHORS["root"]), np.asarray(SOURCE_ANCHORS["tip"])
    stem = tip-root
    progress = (material-root)@stem/(stem@stem)
    pinned = np.flatnonzero((progress >= -.08) & (progress <= .10))
    if len(pinned) != 37:
        raise ProbeError("Expected 37 pinned lower-stem vertices.")
    baseline_parameters = dict(held, ParamGrassSwing=0.0, ParamGrassTipBend=0.0)
    reference = snapshot(native, baseline_parameters)
    baseline_root = anchor_point(reference[index], anchors["root"], ppu.value)
    rows = []
    values = (-1., -.75, -.5, -.35, 0., .35, .5, .75, 1.)
    for swing in values:
        for bend in values:
            current = snapshot(native, dict(held, ParamGrassSwing=swing, ParamGrassTipBend=bend))
            displacement = np.linalg.norm(current[index]-reference[index], axis=1)*ppu.value
            other_changed = [name for i, name in enumerate(native.drawable_ids)
                             if i != index and not np.array_equal(current[i], reference[i])]
            rows.append({"swing": swing, "tip_bend": bend,
                         "pinned_max_source_px": float(displacement[pinned].max()),
                         "root_source_px": float(np.linalg.norm(
                             anchor_point(current[index], anchors["root"], ppu.value)-baseline_root)),
                         "grass_max_source_px": float(displacement.max()),
                         "other_drawables_changed": other_changed})
    center, epsilon = .26, .02  # existing Hold Swing .4 has steady target .65*.4
    central = snapshot(native, dict(held, ParamGrassSwing=center, ParamGrassTipBend=0))[index]
    positive = snapshot(native, dict(held, ParamGrassSwing=center+epsilon, ParamGrassTipBend=0))[index]
    negative = snapshot(native, dict(held, ParamGrassSwing=center-epsilon, ParamGrassTipBend=0))[index]
    points = {name: anchor_point(central, anchor, ppu.value) for name, anchor in anchors.items()}
    direction = points["tip"]-points["root"]
    normal = np.asarray((-direction[1], direction[0]))/np.linalg.norm(direction)
    distal = points["tip"]-points["mid"]
    distal_normal = np.asarray((-distal[1], distal[0]))/np.linalg.norm(distal)
    tip_derivative = (anchor_point(positive, anchors["tip"], ppu.value)
                      - anchor_point(negative, anchors["tip"], ppu.value))/(2*epsilon)
    signed_motion = float(tip_derivative@normal)
    checks = {
        "formal_moc_hash_matches": True,
        "native_flex_ranges_match": True,
        "root_and_mid_inside_material": all(anchors[name]["source_residual_px"] < 1e-8
                                            for name in ("root", "mid")),
        "tip_closest_edge_within_3px": anchors["tip"]["source_residual_px"] < 3,
        "37_lower_stem_vertices_selected": len(pinned) == 37,
        "pins_invariant_within_0_002_source_px": all(row["pinned_max_source_px"] <= PIN_TOLERANCE_SOURCE_PX for row in rows),
        "root_invariant_within_0_002_source_px": all(row["root_source_px"] <= PIN_TOLERANCE_SOURCE_PX for row in rows),
        "all_other_drawables_exactly_unchanged": not any(row["other_drawables_changed"] for row in rows),
        "positive_swing_moves_screen_right_normal": signed_motion > 0,
        "inputs_unchanged": all(digest(path) == sha for path, sha in before.items()),
    }
    report = {
        "schema_version": 1, "kind": "formal_grass_flex_pin_probe", "passed": all(checks.values()),
        "created_utc": datetime.now(timezone.utc).isoformat(), "checks": checks,
        "scope": {"raw_core_only": True, "in_memory_parameter_sweep": True,
                  "rendered_pixels_evaluated": False, "canvas_postprocessing_evaluated": False,
                  "spring_response_evaluated": False, "absolute_palm_contact": "unmeasured",
                  "pointer_routing_evaluated": False, "visual_approval": "pending"},
        "model": str(MODEL), "moc_sha256": before[moc_path], "core_sha256": before[core_path],
        "tool_sha256": before[Path(__file__).resolve()], "clip_sha256": before[CLIP],
        "hold_time": hold_time, "held_parameters": held, "native_flex_ranges": {axis: native.ranges[axis] for axis in FLEX_AXES},
        "canvas_pixels_per_unit": ppu.value, "drawable_id": GRASS,
        "grass_vertices": len(material), "other_drawables": len(native.drawable_ids)-1,
        "topology_uv_sha256": hashlib.sha256(uvs.astype("<f4").tobytes()).hexdigest(),
        "topology_indices_sha256": hashlib.sha256(triangles.astype("<u2").tobytes()).hexdigest(),
        "source_rect": list(SOURCE_RECT), "anchors": anchors, "pinned_indices": pinned.tolist(),
        "pin_selection": {"source_root": root.tolist(), "source_tip": tip.tolist(),
                          "min_progress": -.08, "max_progress": .10},
        "sweep_samples": len(rows), "samples": rows,
        "max_pinned_drift_source_px": max(row["pinned_max_source_px"] for row in rows),
        "max_root_drift_source_px": max(row["root_source_px"] for row in rows),
        "screen_direction": {
            "coordinate_system": "x right, y down; common positive scale and translation omitted",
            "center_swing": center, "tip_bend": 0, "swing_difference_epsilon": epsilon,
            "root_to_tip_right_normal": normal.tolist(), "mid_to_tip_right_normal": distal_normal.tolist(),
            "tip_derivative_source_px_per_swing": tip_derivative.tolist(),
            "derivative_along_root_tip_right_normal": signed_motion,
            "derivative_along_distal_right_normal": float(tip_derivative@distal_normal),
            "positive_screen_right_normal_requires_positive_forcing": signed_motion > 0,
        },
        "input_hashes": {str(path): sha for path, sha in sorted(before.items(), key=lambda pair: str(pair[0]))},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf8")
    if not report["passed"]:
        raise ProbeError("Grass pin checks failed; see "+str(output))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--core", type=Path)
    args = parser.parse_args()
    try:
        result = run(args.output, args.core)
    except (ProbeError, OSError, ValueError, KeyError, StopIteration) as error:
        parser.exit(2, str(error)+"\n")
    print(json.dumps({"passed": result["passed"], "samples": result["sweep_samples"],
                      "max_pinned_drift_source_px": result["max_pinned_drift_source_px"],
                      "positive_forcing_moves_right_normal": result["screen_direction"][
                          "positive_screen_right_normal_requires_positive_forcing"]}))


if __name__ == "__main__":
    main()
