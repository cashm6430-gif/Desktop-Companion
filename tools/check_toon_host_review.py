"""Verify frozen Qt/Godot event replay, GPU frames and measured rig facts.

This gate preserves visual approval gaps. It uses the review's source snapshots,
not today's mutable working tree. A replay without disconnect cannot establish
reconnect behavior; the independent Qt bridge test evidence covers that case.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys


LIMITATIONS = [
    "These checks do not approve the Whale Girl art, likeness or anatomy.",
    "Foot measurements are Skeleton3D joint origins, not sole-floor contact.",
    "Unit bone scales do not prove skin weights or garment-body contact.",
    "Current deletion is a small expression overlay, not gripping, biting or file manipulation.",
    "Current seated work is static; typing, fingers and mouth opening are not implemented.",
    "GPU alpha and native flags do not replace desktop composite, drag or click review.",
    "This event replay does not inject a transport outage; reconnect truth is covered by Qt tests separately.",
]


def read_json(path):
    return json.loads(path.read_text(encoding="utf8"))


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"sha256": digest.hexdigest(), "bytes": path.stat().st_size}


def bounded_path(root, filename):
    if not isinstance(filename, str) or not filename:
        raise ValueError("Missing review-relative filename")
    path = (root / filename).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f"File escapes the frozen review: {filename}")
    return path


def finite_vector(value):
    return (isinstance(value, list) and len(value) == 3 and all(
        isinstance(item, (int, float)) and not isinstance(item, bool) and math.isfinite(item)
        for item in value))


def ordered_subsequence(values, expected):
    index = 0
    for value in values:
        if value == expected[index]:
            index += 1
            if index == len(expected):
                return True
    return False


def frozen_source_path(review, key, receipt):
    mapping = receipt.get("source_snapshots", {})
    if key in mapping:
        entry = mapping[key]
        return bounded_path(review, entry.get("file") if isinstance(entry, dict) else entry)
    if key.startswith("godot/toon-prototype/"):
        return bounded_path(review, "project/" + key.removeprefix("godot/toon-prototype/"))
    # An absolute model source is allowed in the receipt. It resolves to the
    # staged GLB, never to a mutable/external file during this verification.
    model_source = receipt.get("model_source", "")
    if key == model_source or (model_source and key.replace("\\", "/") == model_source.replace("\\", "/")):
        return review / "project/assets/character.glb"
    expected_model = receipt.get("model_fingerprint", {})
    if receipt.get("sources", {}).get(key) == expected_model:
        return review / "project/assets/character.glb"
    return bounded_path(review, "source-snapshots/" + key)


def check_review(review):
    receipt = read_json(review / "review-receipt.json")
    qt = read_json(review / "qt-host-report.json")
    runtime = read_json(review / "captures/report.json")
    trace = read_json(review / "captures/trace.json")
    manifest = read_json(review / "captures/sequence-manifest.json")
    profile = read_json(review / "review-profile.json")
    replay = read_json(review / "replay.json")
    checks = {}

    def check(name, passed, **facts):
        checks[name] = {"passed": bool(passed), **facts}

    check("isolated_review_completed", receipt.get("status") == "technical_capture_passed"
          and receipt.get("exit_code") == 0 and qt.get("status") == "completed"
          and qt.get("ready_seen") is True and qt.get("isolated_replay") is True
          and qt.get("real_sources_enabled") is False and not qt.get("error")
          and not receipt.get("errors") and not runtime.get("errors"),
          receipt_status=receipt.get("status"), qt_status=qt.get("status"),
          exit_code=receipt.get("exit_code"), errors=receipt.get("errors", []))
    runtime_errors = re.findall(r"^.*(?:SCRIPT ERROR|ERROR:).*$", qt.get("renderer_output", ""), re.M)
    check("renderer_diagnostics_clean", not runtime_errors, errors=runtime_errors)
    check("receipt_runtime_consistent", receipt.get("runtime") == runtime)
    check("actual_gpu", runtime.get("headless") is False and runtime.get("display_server") != "headless"
          and bool(runtime.get("native_handle")) and bool(runtime.get("adapter"))
          and bool(runtime.get("rendering_driver")) and runtime.get("host_mode", {}).get("enabled") is True,
          adapter=runtime.get("adapter"), driver=runtime.get("rendering_driver"))
    check("approval_still_pending", qt.get("formal_model_changed") is False
          and qt.get("visual_approval") == "pending" and runtime.get("approved_character") is False
          and receipt.get("live2d_backend_replaced") is False)

    source_rows = []
    for key, expected in receipt.get("sources", {}).items():
        path = frozen_source_path(review, key, receipt)
        actual = fingerprint(path) if path.is_file() else None
        source_rows.append({"source": key, "file": str(path.relative_to(review)),
                            "passed": actual == expected, "actual": actual, "expected": expected})
    check("frozen_source_hashes", bool(source_rows) and all(row["passed"] for row in source_rows), sources=source_rows)
    model = fingerprint(review / "project/assets/character.glb")
    ready_details = qt.get("final_status", {}).get("renderer_details", {})
    check("model_hashes", model == receipt.get("model_fingerprint")
          and model["sha256"] == runtime.get("model_sha256") == ready_details.get("model_sha256"), actual=model)
    profile_hash = receipt.get("profile_fingerprint", receipt.get("review_profile_fingerprint", {}))
    check("replay_and_profile_hashes", fingerprint(review / "replay.json") == receipt.get("replay_fingerprint")
          and fingerprint(review / "review-profile.json") == profile_hash)

    native = receipt.get("window_audit_samples", [])
    audit = receipt.get("window_audit", {})
    required_native = ("visible", "caption_absent", "topmost", "no_activate")
    native_pass = lambda row: (row.get("status") == "passed" and all(row.get(name) is True for name in required_native)
                               and row.get("owns_foreground") is False and row.get("outside_corner_hits_own_window") is False)
    check("native_window_samples", bool(native) and native_pass(audit) and all(native_pass(row) for row in native),
          samples=len(native), observed_self_foreground=sum(row.get("owns_foreground") is True for row in native))
    focus = [row for row in runtime.get("focus_events", []) if row.get("type", row.get("event")) == "focus_entered"]
    check("runtime_focus_not_acquired", not focus and runtime.get("window", {}).get("has_focus") is False
          and all(row.get("fg_is_self") is False for row in runtime.get("fg_probes", [])), focus_events=focus)

    qtrace = qt.get("trace", [])
    outbound = [row["message"] for row in qtrace if row.get("event") == "outbound"]
    snapshots = [message for message in outbound if message.get("type") == "activity.snapshot"]
    requests = [message for message in outbound if message.get("type") == "action.request"]
    request_by_id = {message.get("request_id"): message for message in requests}
    counts = [message.get("active_turns") for message in snapshots]
    count_valid = all(isinstance(count, int) and not isinstance(count, bool) and count >= 0
                      and message.get("busy") == (count > 0) for message, count in zip(snapshots, counts))
    check("concurrent_activity_truth", bool(snapshots) and count_valid and ordered_subsequence(counts, [2, 1, 0]), counts=counts)
    final = qt.get("final_status", {})
    check("final_idle_truth", final.get("activity") == "idle" and final.get("active_turns") == 0
          and final.get("foreground_request_id") == "" and final.get("foreground_action") == "", final=final)
    protocol_errors, active, seqs, revisions = [], None, {}, []
    for row in qtrace:
        if row.get("event") == "outbound":
            message = row["message"]
            epoch = (message.get("session_id"), message.get("connection_id"))
            sequence = message.get("seq")
            if (type(message.get("v")) is not int or message.get("v") != 1
                    or not all(epoch) or type(sequence) is not int
                    or sequence <= seqs.get(epoch, 0)):
                protocol_errors.append("invalid_outbound_epoch_or_sequence")
            seqs[epoch] = sequence if isinstance(sequence, int) else 0
            kind, request_id = message.get("type"), message.get("request_id")
            if kind == "action.request":
                if active is not None or not request_id or message.get("ttl_ms") != 12000:
                    protocol_errors.append("new_action_without_prior_clear_or_bad_ttl")
                active = request_id
            elif kind == "action.cancel":
                if request_id != active:
                    protocol_errors.append("cancel_not_current_request")
                active = None
            elif kind == "activity.snapshot":
                revisions.append(message.get("revision", -1))
                if message.get("foreground_request_id", "") != (active or ""):
                    protocol_errors.append("snapshot_foreground_not_current")
        elif row.get("event") == "action_ack":
            if row.get("request_id") != active:
                protocol_errors.append("old_ack_changed_current_action")
            if row.get("status") in ("finished", "interrupted", "rejected"):
                active = None
        elif row.get("event") == "action_cleared" and row.get("request_id") == active:
            active = None
    check("qt_request_sequence_and_current_ack", not protocol_errors and len(requests) == len(request_by_id)
          and bool(revisions) and all(b > a for a, b in zip(revisions, revisions[1:])), errors=protocol_errors)
    cancels = [message for message in outbound if message.get("type") == "action.cancel"]
    restarted = [message for message in cancels if message.get("reason") == "activity_changed"
                 and request_by_id.get(message.get("request_id"), {}).get("name") == "work.finished"]
    check("new_turn_cancels_return_gaze", bool(restarted), cancellations=restarted)

    delete_times = [event["at_ms"] for event in replay.get("events", []) if event.get("event") == "delete"]
    accepted_delete_clusters, last_delete = 0, -1e9
    for when in delete_times:
        if when - last_delete >= 450:
            accepted_delete_clusters += 1
            last_delete = when
    deletion_requests = [message for message in requests if message.get("name") == "delete.react"]
    ready_count = sum(row.get("event") == "renderer_ready" for row in qtrace)
    check("deletion_dedup_and_no_extra_replay", bool(delete_times)
          and len(deletion_requests) == accepted_delete_clusters and ready_count == 1,
          replay_delete_events=len(delete_times), expected_clusters=accepted_delete_clusters,
          actual_requests=len(deletion_requests), ready_connections=ready_count,
          scope="No unsolicited/replayed delete in this single-connection run; outage recovery is a separate Qt test.")
    events = runtime.get("host_events", [])
    renderer_snapshots = [event for event in events if event.get("event") == "snapshot"]
    snapshot_key = lambda row: (row.get("revision"), row.get("busy"), row.get("active_turns"))
    check("renderer_received_activity_truth", bool(renderer_snapshots)
          and [snapshot_key(row) for row in renderer_snapshots] == [snapshot_key(row) for row in snapshots],
          qt_snapshots=len(snapshots), renderer_snapshots=len(renderer_snapshots))
    renderer_requests = [event for event in events if event.get("event") == "request"]
    renderer_acks = [event for event in events if event.get("event") == "ack"]
    gd_errors = []
    if [(row.get("request_id"), row.get("name")) for row in renderer_requests] != [
            (row.get("request_id"), row.get("name")) for row in requests]:
        gd_errors.append("renderer_received_requests_differ_from_host")
    qt_ack_ids = {row.get("request_id") for row in qtrace if row.get("event") in ("action_ack", "stale_ack_rejected")}
    for ack in renderer_acks:
        if ack.get("request_id") not in request_by_id or ack.get("request_id") not in qt_ack_ids:
            gd_errors.append("renderer_ack_not_correlated_with_host")
        if ack.get("status") not in ("started", "finished", "interrupted", "rejected"):
            gd_errors.append("invalid_renderer_ack_status")
    for request_id in request_by_id:
        acknowledgements = [row.get("status") for row in renderer_acks if row.get("request_id") == request_id]
        if not acknowledgements or acknowledgements[-1] not in ("finished", "interrupted", "rejected"):
            gd_errors.append("renderer_request_lacks_terminal_ack")
    check("renderer_requests_and_ack_match_qt", not gd_errors and bool(renderer_requests), errors=gd_errors)
    check("capability_limits_are_explicit", all(ready_details.get(name) is False
          for name in ("typing", "mouth_open_close", "finger_grasp"))
          and ready_details.get("character_status") == "unapproved_proxy_or_candidate", details=ready_details)

    samples = trace.get("samples", [])
    times = [sample.get("time") for sample in samples]
    check("host_wall_clock_trace", bool(samples) and len(samples) == runtime.get("trace_samples")
          and all(isinstance(value, (int, float)) and math.isfinite(value) for value in times)
          and all(b > a for a, b in zip(times, times[1:]))
          and runtime.get("clock", {}).get("mode") == "wall_delta_animation_clock"
          and runtime.get("clock", {}).get("animation_dt_seconds") is None
          and abs(runtime.get("duration_seconds", -100) - runtime.get("wall_duration_seconds", 100)) <= 0.5,
          samples=len(samples), duration=runtime.get("duration_seconds"), wall_duration=runtime.get("wall_duration_seconds"))
    last_pose = samples[-1] if samples else {}
    check("renderer_final_idle_pose", last_pose.get("host", {}).get("busy") is False
          and last_pose.get("host", {}).get("action") == ""
          and last_pose.get("host", {}).get("sit_progress", 2) <= 0.001
          and last_pose.get("state") == "host-idle")
    plan = profile.get("capture_plan", [])
    captures = runtime.get("captures", [])
    expected_files = {item["name"] + ".png" for item in plan}
    check("capture_plan_completed", runtime.get("visual_capture_completed") is True
          and runtime.get("capture_plan") == plan and len(captures) == len(plan)
          and {item.get("file") for item in captures} == expected_files
          and all(item.get("source") == "gpu_viewport_after_frame_post_draw" for item in captures))
    from PIL import Image
    image_rows = []
    listed_images = {item.get("file"): item for item in receipt.get("images", [])}
    for filename in sorted(expected_files | {"shutdown-transparent.png"}):
        path = bounded_path(review / "captures", filename)
        row = {"file": filename, "passed": False}
        if path.is_file():
            with Image.open(path) as image:
                alpha = image.convert("RGBA").getchannel("A")
                row.update(alpha_range=list(alpha.getextrema()), pixels=list(image.size), **fingerprint(path))
            expected_alpha = [0, 0] if filename == "shutdown-transparent.png" else [0, 255]
            listing = listed_images.get(filename, {})
            row["passed"] = (row["alpha_range"] == expected_alpha and listing.get("sha256") == row["sha256"]
                             and listing.get("bytes") == row["bytes"])
        image_rows.append(row)
    check("gpu_key_images_and_hashes", all(row["passed"] for row in image_rows), images=image_rows)
    check("transparent_shutdown_frame", runtime.get("shutdown_gpu_frame_presented") is True
          and runtime.get("shutdown_visible_alpha_pixels") == 0
          and runtime.get("shutdown_strategy") == "hide_all_visual_content_then_frame_post_draw_then_quit"
          and next(row for row in image_rows if row["file"] == "shutdown-transparent.png")["passed"])
    frames = manifest.get("frames", [])
    frame_errors, all_poses = [], list(samples) + [capture.get("pose", {}) for capture in captures]
    frame_times, frame_numbers = [], []
    for frame in frames:
        number = frame.get("frame")
        filename = frame.get("file")
        if not isinstance(number, int) or filename != f"frames/frame-{number:06d}.png":
            frame_errors.append("invalid_frame_filename"); continue
        path = bounded_path(review / "captures", filename)
        if not path.is_file() or fingerprint(path)["sha256"] != frame.get("sha256"):
            frame_errors.append("missing_or_modified_frame"); continue
        frame_numbers.append(number)
        frame_times.append(frame.get("animation_time"))
        all_poses.append(frame.get("pose", {}))
        if frame.get("source") != "gpu_viewport_after_frame_post_draw" or frame.get("alpha", {}).get("clipped_at_viewport_edge") is not False:
            frame_errors.append("invalid_frame_source_or_clipped_viewport")
    sequence = runtime.get("sequence", {})
    check("variable_wall_gpu_sequence", bool(frames) and not frame_errors
          and len(frames) == sequence.get("frame_count") and sequence.get("enabled") is True
          and sequence.get("animation_fps") is None and manifest.get("animation_fps") is None
          and manifest.get("clock") == "variable_wall_dt"
          and all(isinstance(value, (int, float)) and math.isfinite(value) for value in frame_times)
          and all(b > a for a, b in zip(frame_numbers, frame_numbers[1:]))
          and all(b > a for a, b in zip(frame_times, frame_times[1:])),
          frames=len(frames), errors=frame_errors)
    worst_scale, foot_rows, pose_errors = 0.0, [], []
    baseline = samples[0].get("bones", {}) if samples else {}
    for pose in all_poses:
        bones = pose.get("bones", {})
        if not bones or set(bones) != set(baseline):
            pose_errors.append("incomplete_bone_set"); continue
        for bone in bones.values():
            if not finite_vector(bone.get("scale")) or not finite_vector(bone.get("global_position")):
                pose_errors.append("invalid_bone_vector"); continue
            worst_scale = max(worst_scale, *(abs(value - 1) for value in bone["scale"]))
    for name in ("foot.L", "foot.R"):
        anchor = baseline.get(name, {}).get("global_position")
        if not finite_vector(anchor):
            pose_errors.append("missing_foot_anchor"); continue
        errors = [math.dist(pose["bones"][name]["global_position"], anchor) for pose in all_poses
                  if finite_vector(pose.get("bones", {}).get(name, {}).get("global_position"))]
        foot_rows.append({"bone": name, "maximum_drift_m": max(errors, default=100), "anchor": anchor})
    check("bone_unit_scales", not pose_errors and worst_scale <= 0.001, maximum_error=worst_scale, errors=pose_errors)
    check("foot_joint_drift_at_most_2mm", not pose_errors and len(foot_rows) == 2
          and all(row["maximum_drift_m"] <= 0.002 for row in foot_rows), feet=foot_rows,
          space="Skeleton3D model space; not sole-floor contact")
    props_bad = [pose.get("time") for pose in all_poses if pose.get("host", {}).get("workstation_visible") is True
                 and (pose.get("host", {}).get("sit_progress", -1) < 0.999
                      or not (pose.get("host", {}).get("busy") is True or pose.get("host", {}).get("action") == "work.finished"))]
    props_seen = any(pose.get("host", {}).get("workstation_visible") is True for pose in all_poses)
    check("workstation_only_when_fully_seated", props_seen and not props_bad, violations=props_bad)
    failed = [name for name, result in checks.items() if not result["passed"]]
    inputs = ["review-receipt.json", "qt-host-report.json", "replay.json", "review-profile.json",
              "captures/report.json", "captures/trace.json", "captures/sequence-manifest.json"]
    return {"schema_version": 1, "kind": "qt_godot_semantic_host_check", "review": str(review),
            "status": "technical_checks_passed_visual_review_pending" if not failed else "failed",
            "passed": not failed, "input_fingerprints": {name: fingerprint(review / name) for name in inputs},
            "checks": checks, "failed_checks": failed, "visual_approval": "pending", "limitations": LIMITATIONS}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="Defaults to <review>/host-check.json")
    args = parser.parse_args()
    review = args.review.resolve()
    if not review.is_dir():
        parser.error(f"Review does not exist: {review}")
    try:
        result = check_review(review)
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        result = {"schema_version": 1, "kind": "qt_godot_semantic_host_check", "passed": False,
                  "status": "insufficient_evidence", "review": str(review), "error": str(error),
                  "visual_approval": "pending", "limitations": LIMITATIONS}
    output = args.output.resolve() if args.output else review / "host-check.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"status": result["status"], "output": str(output),
                      "failed_checks": result.get("failed_checks", []), "error": result.get("error")}, ensure_ascii=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
