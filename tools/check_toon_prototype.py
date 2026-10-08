"""Check captured Godot rig facts, preserving the remaining visual review gaps.

The measured joint origins live in Skeleton3D space. Parent-to-child origin
distances are compared with the same pair in the trace baseline; they are not
assumed to equal a parent bone's length. This matters for unconnected children.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "art/3d/prototype/rig-contract.json"
CAPTURE_NAMES = (
    "standing", "blink", "wave", "half-sit", "seated", "rise", "turn45", "idle-return",
)
TOLERANCES = {
    "bone_scale_absolute_error": 0.001,
    "fixed_parent_child_origin_distance_error_m": 0.001,
    "source_baseline_origin_distance_error_m": 0.001,
    "foot_anchor_error_m": 0.01,
    "wave_upper_arm_angle_min_degrees": 20.0,
    "wave_wrist_displacement_min_m": 0.15,
    "sit_pelvis_drop_min_m": 0.15,
    "sit_hip_knee_angle_min_degrees": 10.0,
    "return_pelvis_error_m": 0.01,
    "return_hip_knee_angle_error_degrees": 0.5,
    "turn45_angle_error_degrees": 1.0,
}
LIMITATIONS = [
    "Numerical rig and viewport checks do not approve character art or likeness.",
    "No cloth simulation, garment-body intersection, fingers, or seat contact approval.",
    "Joint-origin spacing and unit scales do not prove mesh skinning is visually sound.",
    "GPU viewport alpha does not prove Windows desktop compositing has no black edges.",
    "Window flags and a read-only corner hit check do not prove real clicks or dragging.",
    "Foot anchors are joint origins in skeleton space, not a measured sole-floor contact.",
]


def read_json(path):
    return json.loads(path.read_text(encoding="utf8"))


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def vector(value, length=3):
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f"Expected a finite vector with {length} entries")
    if not all(isinstance(item, (float, int)) and math.isfinite(item) for item in value):
        raise ValueError("Non-finite or non-numeric vector")
    return value


def angle_degrees(left, right):
    left, right = vector(left, 4), vector(right, 4)
    divisor = math.sqrt(sum(x * x for x in left) * sum(x * x for x in right))
    if divisor < 1e-12:
        raise ValueError("Zero-length bone rotation quaternion")
    dot = abs(sum(x * y for x, y in zip(left, right))) / divisor
    return math.degrees(2 * math.acos(min(1.0, max(0.0, dot))))


def runtime_point(blender_point):
    x, y, z = vector(blender_point)
    return [x, z, -y]


def check_review(review, contract_path=CONTRACT):
    receipt = read_json(review / "review-receipt.json")
    runtime = read_json(review / "captures/report.json")
    trace = read_json(review / "captures/trace.json")
    contract = read_json(contract_path)
    checks = {}

    def check(name, passed, **facts):
        checks[name] = {"passed": bool(passed), **facts}

    check("receipt_runtime_consistency", receipt.get("runtime") == runtime,
          receipt_status=receipt.get("status"), runtime_status=runtime.get("status"))
    gpu = (runtime.get("headless") is False and runtime.get("display_server") != "headless"
           and runtime.get("renderer") in ("gl_compatibility", "forward_plus", "mobile")
           and bool(runtime.get("rendering_driver")) and bool(runtime.get("adapter"))
           and bool(runtime.get("adapter_api_version")) and bool(runtime.get("native_handle")))
    check("actual_gpu_runtime", gpu, headless=runtime.get("headless"),
          display_server=runtime.get("display_server"), renderer=runtime.get("renderer"),
          rendering_driver=runtime.get("rendering_driver"), adapter=runtime.get("adapter"),
          adapter_api_version=runtime.get("adapter_api_version"), native_handle=runtime.get("native_handle"))
    check("runtime_completed", receipt.get("exit_code") == 0
          and receipt.get("status") == "technical_capture_passed"
          and runtime.get("visual_capture_completed") is True
          and not runtime.get("errors") and not receipt.get("errors"),
          exit_code=receipt.get("exit_code"), errors=runtime.get("errors", []),
          receipt_errors=receipt.get("errors", []), receipt_status=receipt.get("status"),
          capture_completed=runtime.get("visual_capture_completed"))
    expected_glb_hash = contract.get("glb_sha256")
    staged_model = review / "project/assets/character.glb"
    actual_glb_hash = fingerprint(staged_model)["sha256"] if staged_model.is_file() else None
    source_glb_hash = receipt.get("model_fingerprint", {}).get("sha256") or receipt.get("sources", {}).get("assets/3d/prototype/character.glb", {}).get("sha256")
    check("model_identity", bool(expected_glb_hash)
          and runtime.get("model_sha256") == expected_glb_hash == actual_glb_hash == source_glb_hash,
          contract_glb_sha256=expected_glb_hash, staged_glb_sha256=actual_glb_hash,
          model_source=receipt.get("model_source", "assets/3d/prototype/character.glb"),
          runtime_glb_sha256=runtime.get("model_sha256"), receipt_glb_sha256=source_glb_hash)

    images = []
    expected_files = {name + ".png" for name in CAPTURE_NAMES}
    shutdown_file = "shutdown-transparent.png"
    allowed_files = expected_files | {shutdown_file}
    captures = runtime.get("captures", [])
    captured_files = [capture.get("file") for capture in captures]
    receipt_images = {item.get("file"): item for item in receipt.get("images", [])}
    actual_files = {path.name for path in (review / "captures").glob("*.png")}
    from PIL import Image
    for filename in sorted(expected_files):
        path = review / "captures" / filename
        item = {"file": filename, "exists": path.is_file(), "passed": False}
        if path.is_file():
            with Image.open(path) as image:
                rgba = image.convert("RGBA")
                alpha = rgba.getchannel("A")
                width, height = rgba.size
                item.update(pixels=[width, height], alpha_range=list(alpha.getextrema()),
                            transparent_pixels=alpha.histogram()[0], **fingerprint(path))
            listed = receipt_images.get(filename, {})
            item["receipt_hash_matches"] = listed.get("sha256") == item["sha256"] and listed.get("bytes") == item["bytes"]
            item["passed"] = (item["receipt_hash_matches"] and item["alpha_range"] == [0, 255]
                              and item["transparent_pixels"] > 0 and width > 0 and height > 0)
        images.append(item)
    check("eight_gpu_captures", len(captures) == 8 and len(set(captured_files)) == 8
          and set(captured_files) == expected_files and expected_files <= actual_files <= allowed_files
          and expected_files <= set(receipt_images) <= allowed_files and all(item["passed"] for item in images)
          and all(capture.get("source") == "gpu_viewport_after_frame_post_draw" for capture in captures),
          expected_files=sorted(expected_files), runtime_files=captured_files,
          actual_files=sorted(actual_files), receipt_files=sorted(receipt_images))
    shutdown_path = review / "captures" / shutdown_file
    shutdown = {"file": shutdown_file, "exists": shutdown_path.is_file()}
    if shutdown_path.is_file():
        with Image.open(shutdown_path) as image:
            alpha = image.convert("RGBA").getchannel("A")
            shutdown.update(pixels=list(image.size), alpha_range=list(alpha.getextrema()), **fingerprint(shutdown_path))
        listed = receipt_images.get(shutdown_file, {})
        shutdown["receipt_hash_matches"] = listed.get("sha256") == shutdown["sha256"] and listed.get("bytes") == shutdown["bytes"]
    check("shutdown_viewport_is_fully_transparent", shutdown.get("alpha_range") == [0, 0]
          and shutdown.get("receipt_hash_matches") is True
          and runtime.get("shutdown_gpu_frame_presented") is True
          and runtime.get("shutdown_visible_alpha_pixels") == 0
          and runtime.get("shutdown_strategy") == "hide_all_visual_content_then_frame_post_draw_then_quit", **shutdown,
          strategy=runtime.get("shutdown_strategy"), gpu_frame_presented=runtime.get("shutdown_gpu_frame_presented"),
          visible_alpha_pixels=runtime.get("shutdown_visible_alpha_pixels"),
          limitation="A transparent viewport frame supports cleanup sequencing, not desktop DWM composition")
    capture_edges = [{"file": capture.get("file"),
                      "clipped_at_viewport_edge": capture.get("alpha", {}).get("clipped_at_viewport_edge")}
                     for capture in captures]
    check("capture_viewport_margin", len(capture_edges) == 8
          and all(item["clipped_at_viewport_edge"] is False for item in capture_edges), images=capture_edges)

    expected_bones = {item["name"] for item in contract["bones"]}
    parents = {item["name"]: item["parent"] for item in contract["bones"] if item["parent"]}
    samples = trace.get("samples", [])
    if not samples:
        raise ValueError("Trace has no pose samples")
    for sample in samples:
        if not isinstance(sample.get("time"), (float, int)) or not math.isfinite(sample["time"]):
            raise ValueError("Trace contains an invalid time")
        if set(sample.get("bones", {})) != expected_bones:
            raise ValueError("Trace does not consistently contain the contract bone names")
        for bone in sample["bones"].values():
            for field in ("position", "global_position", "scale"):
                vector(bone[field])
            vector(bone["rotation_xyzw"], 4)
    baseline = samples[0]
    bones = baseline["bones"]
    sample_times = [sample["time"] for sample in samples]
    states = {sample.get("state") for sample in samples}
    check("trace_complete", len(samples) == runtime.get("trace_samples")
          and all(right > left for left, right in zip(sample_times, sample_times[1:]))
          and {"idle", "wave", "sit", "seated", "rise", "turn"} <= states,
          samples=len(samples), states=sorted(states), duration_seconds=sample_times[-1],
          max_sample_interval_seconds=max((b - a for a, b in zip(sample_times, sample_times[1:])), default=0))
    sequence_check = {"present": "sequence" in runtime, "status": "not_requested_in_this_legacy_capture"}
    if "sequence" in runtime:
        sequence = runtime["sequence"]
        manifest_path = review / "captures" / "sequence-manifest.json"
        manifest = read_json(manifest_path) if manifest_path.is_file() else {}
        frames = manifest.get("frames", [])
        expected_last_frame = int(runtime.get("clock", {}).get("animation_frames", 0)) // 4 * 4
        expected_frame_numbers = list(range(4, expected_last_frame + 1, 4))
        actual_frame_numbers = [item.get("frame") for item in frames]
        sequence_failures = []
        sequence_states = set()
        listed_files = []
        for item in frames:
            filename, frame = item.get("file", ""), item.get("frame")
            expected_filename = f"frames/frame-{frame:06d}.png" if isinstance(frame, int) else "invalid"
            if filename != expected_filename:
                sequence_failures.append({"file": filename, "error": "filename_not_pinned_to_frame_number"})
                continue
            listed_files.append(filename)
            path = review / "captures" / filename
            pose = item.get("pose", {})
            state = pose.get("state")
            if isinstance(state, str):
                sequence_states.add(state)
            frame_errors = []
            if not path.is_file():
                frame_errors.append("missing_png")
            else:
                if fingerprint(path)["sha256"] != item.get("sha256"):
                    frame_errors.append("sha256_mismatch")
                with Image.open(path) as image:
                    alpha = image.convert("RGBA").getchannel("A")
                    if alpha.getextrema() != (0, 255):
                        frame_errors.append("not_visible_model_with_transparent_background")
                    if image.size != (360, 420):
                        frame_errors.append("unexpected_viewport_size")
            if item.get("source") != "gpu_viewport_after_frame_post_draw":
                frame_errors.append("not_gpu_viewport_capture")
            animation_time = item.get("animation_time")
            if (not isinstance(animation_time, (float, int)) or not math.isfinite(animation_time)
                    or abs(animation_time - frame / 60.0) > 1e-6):
                frame_errors.append("animation_time_not_frame_over_60")
            if pose.get("frame") != frame or set(pose.get("bones", {})) != expected_bones:
                frame_errors.append("pose_frame_or_bone_inventory_mismatch")
            if not isinstance(pose.get("time"), (float, int)) or abs(pose["time"] - frame / 60.0) > 1e-6:
                frame_errors.append("pose_time_not_frame_over_60")
            if frame_errors:
                sequence_failures.append({"file": filename, "errors": frame_errors})
        actual_sequence_files = {path.relative_to(review / "captures").as_posix()
                                 for path in (review / "captures/frames").glob("*.png")}
        sequence_passed = (gpu and sequence.get("enabled") is True
                           and sequence.get("frame_count") == len(frames) == len(expected_frame_numbers)
                           and actual_frame_numbers == expected_frame_numbers
                           and sequence.get("animation_fps") == manifest.get("animation_fps") == 15
                           and sequence.get("stride_in_fixed_dt_frames") == manifest.get("frame_stride") == 4
                           and sequence.get("manifest_file") == "sequence-manifest.json"
                           and sequence.get("frames_directory") == "frames"
                           and sequence.get("source") == "gpu_viewport_after_frame_post_draw"
                           and abs(manifest.get("animation_dt_seconds", 0) - 1 / 60) < 1e-10
                           and set(listed_files) == actual_sequence_files
                           and {"idle", "wave", "sit", "seated", "rise", "turn"} <= sequence_states
                           and not sequence_failures)
        sequence_check = {"present": True, "passed": sequence_passed, "frame_count": len(frames),
                          "expected_frame_count": len(expected_frame_numbers), "animation_fps": sequence.get("animation_fps"),
                          "first_frame": actual_frame_numbers[0] if frames else None,
                          "last_frame": actual_frame_numbers[-1] if frames else None,
                          "states": sorted(sequence_states), "file_errors": sequence_failures,
                          "manifest_fingerprint": fingerprint(manifest_path) if manifest_path.is_file() else None,
                          "source": sequence.get("source"), "space": "Actual GPU viewport frames; not an approval of cloth or art"}
        check("continuous_gpu_sequence", sequence_passed, **{key: value for key, value in sequence_check.items() if key != "passed"})
    actual_animations = {name.split("/")[-1] for name in runtime.get("animation_names", [])}
    check("imported_animation_and_bone_inventory", len(expected_bones) == 21
          and runtime.get("bone_count") == 21 and actual_animations == {"idle", "wave", "sit"},
          bone_count=runtime.get("bone_count"), animations=sorted(actual_animations))

    expression_ranges = {}
    for expression in ("Blink_L", "Blink_R", "Smile"):
        keys = [name for name in baseline.get("blend_shapes", {}) if name.split("/")[-1] == expression]
        values = [sample.get("blend_shapes", {}).get(keys[0]) for sample in samples] if len(keys) == 1 else []
        valid = values and all(isinstance(value, (float, int)) and math.isfinite(value) for value in values)
        expression_ranges[expression] = ({"target": keys[0], "min": min(values), "max": max(values),
                                         "range": max(values) - min(values)} if valid else {"missing": True})
    expression_passed = all("missing" not in item and item["min"] >= -0.0001 and item["max"] <= 1.0001
                            for item in expression_ranges.values())
    if expression_passed:
        expression_passed = all(expression_ranges[name]["min"] <= 0.05
                                and expression_ranges[name]["max"] >= 0.8 for name in ("Blink_L", "Blink_R"))
        expression_passed = expression_passed and expression_ranges["Smile"]["range"] >= 0.5
    check("three_independent_expression_targets_change", expression_passed, expressions=expression_ranges,
          limitation="Values changing does not approve the visible facial shape")
    face_snapshots = []
    for capture in captures:
        if capture.get("file") in ("standing.png", "seated.png"):
            pose = capture.get("pose", {})
            face_snapshots.append({"file": capture["file"], "blend_shapes": pose.get("blend_shapes", {}),
                                   "head_rotation_xyzw": pose.get("bones", {}).get("head", {}).get("rotation_xyzw"),
                                   "spine_rotation_xyzw": pose.get("bones", {}).get("spine", {}).get("rotation_xyzw")})

    scale_rows = [{"time": sample["time"], "bone": name, "max_error": max(abs(value - 1) for value in bone["scale"])}
                  for sample in samples for name, bone in sample["bones"].items()]
    worst_scale = max(scale_rows, key=lambda row: row["max_error"])
    check("unit_bone_scales", worst_scale["max_error"] <= TOLERANCES["bone_scale_absolute_error"],
          maximum=worst_scale, tolerance=TOLERANCES["bone_scale_absolute_error"])
    contract_bones = {bone["name"]: bone for bone in contract["bones"]}
    pair_rows = []
    for child, parent in parents.items():
        baseline_distance = math.dist(bones[child]["global_position"], bones[parent]["global_position"])
        source_distance = math.dist(runtime_point(contract_bones[child]["head_blender"]),
                                    runtime_point(contract_bones[parent]["head_blender"]))
        distances = [(sample["time"], math.dist(sample["bones"][child]["global_position"],
                                               sample["bones"][parent]["global_position"])) for sample in samples]
        worst = max(distances, key=lambda row: abs(row[1] - baseline_distance))
        pair_rows.append({"child": child, "parent": parent, "baseline_m": baseline_distance,
                          "source_rest_origin_distance_m": source_distance,
                          "source_baseline_error_m": abs(baseline_distance - source_distance),
                          "max_baseline_distance_error_m": abs(worst[1] - baseline_distance),
                          "worst_time": worst[0], "animated_origin_translation": child == "pelvis"})
    fixed_rows = [row for row in pair_rows if not row["animated_origin_translation"]]
    check("fixed_parent_child_origin_spacing", all(row["max_baseline_distance_error_m"]
          <= TOLERANCES["fixed_parent_child_origin_distance_error_m"] for row in fixed_rows)
          and all(row["source_baseline_error_m"] <= TOLERANCES["source_baseline_origin_distance_error_m"] for row in pair_rows),
          pairs=pair_rows, excluded_dynamic_pair="root -> pelvis (authored pelvis translation)",
          tolerance_m=TOLERANCES["fixed_parent_child_origin_distance_error_m"],
          interpretation="Pair-specific head-to-head spacing; not parent tail length or skin coverage")

    foot_rows = []
    for side in ("L", "R"):
        name = "foot." + side
        anchor = contract["sit_contact"]["foot_anchor_" + side]
        rows = [(sample["time"], math.dist(sample["bones"][name]["global_position"], anchor)) for sample in samples]
        worst = max(rows, key=lambda row: row[1])
        foot_rows.append({"bone": name, "anchor_m": anchor, "max_error_m": worst[1], "worst_time": worst[0]})
    check("foot_joint_anchors", all(row["max_error_m"] <= TOLERANCES["foot_anchor_error_m"] for row in foot_rows),
          feet=foot_rows, tolerance_m=TOLERANCES["foot_anchor_error_m"],
          space="Skeleton3D model space before the intentional character yaw; not sole-floor contact")

    wave_contract = contract["animations"]["wave"]
    driven = wave_contract.get("driven_bone", "upper_arm.L")
    follower = wave_contract.get("follower_bone", "wrist.L")
    wave_samples = [sample for sample in samples if sample["state"] == "wave"]
    wave_angles = [angle_degrees(sample["bones"][driven]["rotation_xyzw"], bones[driven]["rotation_xyzw"])
                   for sample in wave_samples]
    wrist_moves = [math.dist(sample["bones"][follower]["global_position"], bones[follower]["global_position"])
                   for sample in wave_samples]
    check("authored_wave_chain_changes", wave_contract.get("side") == "L"
          and wave_contract.get("driven_bone") == "upper_arm.L"
          and wave_contract.get("follower_bone") == "wrist.L"
          and driven == "upper_arm.L" and follower == "wrist.L"
          and bool(wave_angles) and max(wave_angles) >= TOLERANCES["wave_upper_arm_angle_min_degrees"]
          and max(wrist_moves) >= TOLERANCES["wave_wrist_displacement_min_m"],
          side="L", driven_bone=driven, follower_bone=follower,
          maximum_upper_arm_rotation_degrees=max(wave_angles, default=0),
          maximum_wrist_displacement_m=max(wrist_moves, default=0),
          ownership="Explicit author wave side; no choice of whichever arm moves most")

    sit_samples = [sample for sample in samples if sample["state"] in ("sit", "seated")]
    hip_knee_angles = {name: max((angle_degrees(sample["bones"][name]["rotation_xyzw"], bones[name]["rotation_xyzw"])
                                 for sample in sit_samples), default=0)
                      for name in ("thigh.L", "thigh.R", "shin.L", "shin.R")}
    baseline_pelvis_y = bones["pelvis"]["global_position"][1]
    min_pelvis_y = min((sample["bones"]["pelvis"]["global_position"][1] for sample in sit_samples), default=baseline_pelvis_y)
    pelvis_drop = baseline_pelvis_y - min_pelvis_y
    check("sit_uses_pelvis_hips_and_knees", pelvis_drop >= TOLERANCES["sit_pelvis_drop_min_m"]
          and all(value >= TOLERANCES["sit_hip_knee_angle_min_degrees"] for value in hip_knee_angles.values()),
          standing_pelvis_y_m=baseline_pelvis_y, minimum_sit_pelvis_y_m=min_pelvis_y,
          pelvis_drop_m=pelvis_drop, maximum_joint_rotation_degrees=hip_knee_angles)
    rise_samples = [sample for sample in samples if sample["state"] == "rise"]
    return_samples = [sample for sample in samples if sample["state"] == "idle" and sample["time"] >= 14.75]
    return_rows = []
    for label, sample in (("last_rise", rise_samples[-1] if rise_samples else None),
                          ("idle_return", return_samples[0] if return_samples else None)):
        if sample is not None:
            return_rows.append({"stage": label, "time": sample["time"],
                                "pelvis_error_m": math.dist(sample["bones"]["pelvis"]["global_position"], bones["pelvis"]["global_position"]),
                                "max_hip_knee_angle_error_degrees": max(angle_degrees(sample["bones"][name]["rotation_xyzw"], bones[name]["rotation_xyzw"])
                                                                       for name in hip_knee_angles)})
    check("rise_returns_to_standing", len(return_rows) == 2 and all(
          row["pelvis_error_m"] <= TOLERANCES["return_pelvis_error_m"]
          and row["max_hip_knee_angle_error_degrees"] <= TOLERANCES["return_hip_knee_angle_error_degrees"] for row in return_rows),
          poses=return_rows)
    turn_samples = [sample for sample in samples if sample["state"] == "turn"]
    yaw_max = max((sample["character_yaw_degrees"] for sample in turn_samples), default=0)
    final_yaw = return_samples[0]["character_yaw_degrees"] if return_samples else None
    check("turn45_and_front_return", abs(yaw_max - 45) <= TOLERANCES["turn45_angle_error_degrees"]
          and final_yaw is not None and abs(final_yaw) <= 0.1,
          maximum_turn_yaw_degrees=yaw_max, idle_return_yaw_degrees=final_yaw)

    window_audit = receipt.get("window_audit") or {}
    window_facts = (window_audit.get("status") == "passed" and window_audit.get("visible") is True
                    and window_audit.get("caption_absent") is True and window_audit.get("topmost") is True
                    and window_audit.get("no_activate") is True and window_audit.get("owns_foreground") is False
                    and window_audit.get("outside_corner_hits_own_window") is False
                    and window_audit.get("pid") == runtime.get("pid")
                    and window_audit.get("native_handle") == runtime.get("native_handle"))
    check("native_window_observed_facts", window_facts, audit=window_audit,
          limitation="Only the observed window flags and corner lookup; desktop composition and actual clicks remain pending")
    focus_samples = receipt.get("window_audit_samples", [])
    focus_before = receipt.get("foreground_before_launch")
    own_focus_observations = [item for item in focus_samples if item.get("owns_foreground") is True]
    runtime_focus_events = [item for item in runtime.get("focus_events", []) if item.get("has_focus") is True]
    startup_had_focus = runtime.get("startup_window_flags", {}).get("has_focus_before_configuration") is True
    final_has_focus = runtime.get("window", {}).get("has_focus") is True
    if "window_audit_samples" in receipt or "focus_events" in runtime:
        check("foreground_observation_history", bool(focus_samples) and not own_focus_observations
              and all(item.get("status") == "passed" for item in focus_samples)
              and not runtime_focus_events and not startup_had_focus and not final_has_focus,
              sample_count=len(focus_samples), foreground_before_launch=focus_before,
              own_focus_observations=own_focus_observations, runtime_focus_events=runtime_focus_events,
              startup_had_focus=startup_had_focus, final_has_focus=final_has_focus,
              limitation="Any observed self-foreground event fails; later recovery does not erase it")
    failures = [name for name, item in checks.items() if not item["passed"]]
    return {"schema_version": 1, "kind": "toon_3d_technical_capture_check",
            "status": "technical_checks_passed_visual_review_pending" if not failures else "failed",
            "passed": not failures, "review": str(review), "contract": str(contract_path),
            "input_fingerprints": {"rig-contract.json": fingerprint(contract_path),
                                   "review-receipt.json": fingerprint(review / "review-receipt.json"),
                                   "captures/report.json": fingerprint(review / "captures/report.json"),
                                   "captures/trace.json": fingerprint(review / "captures/trace.json")},
            "runtime": checks["actual_gpu_runtime"], "images": images, "window_audit": window_audit,
            "shutdown_image": shutdown, "face_capture_context": face_snapshots,
            "sequence": sequence_check,
            "tolerances": TOLERANCES, "checks": checks, "failed_checks": failures,
            "visual_approval": "pending", "character_art_approved": False,
            "cloth_and_contact_approved": False, "limitations": LIMITATIONS,
            "manual_review_gaps": ["desktop alpha compositing and edge appearance", "real mouse passthrough and drag",
                                   "visual joint deformation, seat/garment contact and camera framing",
                                   "final Q-style Whale Girl likeness and expression quality", "high DPI and multiple monitors"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review", type=Path, required=True, help="Root produced by review_toon_prototype.py")
    parser.add_argument("--contract", type=Path, default=CONTRACT, help="Explicit rig contract; defaults to the canonical prototype")
    args = parser.parse_args()
    review = args.review.resolve()
    if not review.is_dir():
        parser.error(f"Review directory does not exist: {review}")
    try:
        result = check_review(review, args.contract.resolve())
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        result = {"schema_version": 1, "status": "insufficient_evidence", "passed": False,
                  "review": str(review), "runtime": {}, "images": [], "window_audit": {},
                  "error": str(error), "visual_approval": "pending", "limitations": LIMITATIONS}
    (review / "check.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf8")
    print(json.dumps({"status": result["status"], "output": str(review / "check.json"),
                      "failed_checks": result.get("failed_checks", []), "error": result.get("error")}, ensure_ascii=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
