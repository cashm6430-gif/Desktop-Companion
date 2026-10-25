"""Capture approved or draft soft grass input with isolated synthetic Qt input.

python tools/review_grass_drag.py <fresh-directory> [--scenario right]
python tools/review_grass_drag.py <existing-directory> --verify-only

No physical pointer, hooks, desktop files or production IPC are used. Approval
is read from the actual window; passing a capture never adopts a draft.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess

from review_session import ROOT, deployment_snapshot, digest

KINDS = ("right", "left", "up", "down", "limit", "timeout", "delete", "ungrab",
         "deactivate", "hide", "disable", "tap", "default", "approved")
SCENARIOS = tuple("grass-drag-" + name for name in KINDS)
SCOPE = "native_soft_grass_candidate_synthetic_qt_input"
APPROVED_SCOPE = "native_soft_grass_approved_synthetic_qt_input"
EXE = ROOT / "build/DesktopCompanion.exe"
CORE = ROOT / "build/Live2DCubismCore.dll"
INTERRUPTS = {"delete", "ungrab", "deactivate", "hide", "disable"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def point(value):
    return isinstance(value, list) and len(value) == 2 and all(finite(v) for v in value)


def check_scene(path):
    path = Path(path)
    data = json.loads(path.read_text(encoding="utf8"))
    scenario = data["scenario"]
    require(scenario in SCENARIOS and data["scope"] in {SCOPE, APPROVED_SCOPE}, "Unexpected capture scope")
    kind = scenario.removeprefix("grass-drag-")
    require(data["passed"] is True and data["failure"] == "", "Native review failed")
    adopted = data["adoption"] == "approved"
    require(data["adoption"] in {"pending", "approved"}
            and data["scope"] == (APPROVED_SCOPE if adopted else SCOPE), "Approval scope mismatch")
    require(kind != "approved" or adopted, "Approved entry requires approved runtime evidence")
    require(not adopted or kind != "disable", "Use approved scene for preview-off ownership")
    require(data["render_backend"] == "cubism_native" and not data["render_error"], "Native renderer error")
    checks = data["checks"]
    required_checks = {"native_backend", "isolated_settings", "grass_started",
        "background_start_keeps_grass", "actual_tip_hit_found", "grass_does_not_drag_window",
        "hold_and_press_seen", "real_turn_finished_idle", "final_tracking_clear",
        "candidate_or_default_entry", "only_taps_respond", "drag_threshold_or_tap",
        "release_rebounds", "timeout_branch", "background_resumes"}
    required_checks.add("approved_no_preview_entry" if adopted else "candidate_opt_in_only")
    if kind == "approved":
        required_checks.add("approved_preview_off_keeps_drag")
    require(required_checks <= checks.keys() and all(value is True for value in checks.values()),
            "Required Native checks failed or missing")
    privacy = data["privacy"]
    require(privacy["fixture_only"] is True and privacy["isolated_settings"] is True,
            "Capture must isolate settings")
    require(all(privacy[key] is False for key in
                ("default_endpoint_used", "real_hooks_used", "desktop_files_changed")),
            "Capture accessed production events")
    frames = data["frames"]
    require(len(frames) == data["frame_count"] == 156, "Expected 156 complete frames")
    for index, frame in enumerate(frames):
        require(frame["frame"] == index and frame["file"] == f"frame-{index:03}.png", "Frame sequence mismatch")
        require((path.parent / frame["file"]).is_file(), "Missing Native frame")
        require(finite(frame["time"]) and abs(frame["time"] - (index + 1) / 15) < 1e-7
                and finite(frame["dt"]) and abs(frame["dt"] - 1 / 15) < 1e-9,
                "Manual frame clock changed")
        parameters = frame["parameters"]
        require(parameters and all(finite(v) for v in parameters.values()), "Non-finite model parameter")
        for axis in ("ParamGrassSwing", "ParamGrassTipBend"):
            require(axis in parameters and abs(parameters[axis]) <= 1.000001, "Unbounded soft grass parameter")
        drag = frame["drag"]
        if adopted:
            require(drag.get("approved") is True and drag.get("preview_enabled") is False
                    and drag["enabled"] is True, "Approved entry depended on a draft switch")
        require(finite(drag["offset"]) and abs(drag["offset"]) <= .350001, "Unbounded drag force")
        require(point(drag["normal"]) and finite(drag["span"]), "Invalid drag projection")
        require(all(isinstance(drag[key], bool) for key in
                    ("enabled", "pressed", "moved", "active", "settling")), "Invalid drag flags")
        if drag["active"]:
            require(drag["enabled"] and drag["pressed"] and frame["grass_phase"] == "hold",
                    "Pointer capture escaped Hold")
            require(drag["span"] >= 12 and abs(math.hypot(*drag["normal"]) - 1) < 1e-5,
                    "Invalid press-time normal")
        if not drag["active"]:
            require(abs(drag["offset"]) < 1e-8, "Released force did not clear")
        geometry = drag["geometry"]
        require(isinstance(geometry["valid"], bool), "Missing geometry validity")
        if frame["grass_phase"] == "hold":
            require(geometry["valid"], "Missing Hold geometry")
            require(parameters["ParamHandRGrip"] > .98 and parameters["ParamGrassVisible"] > .98,
                    "Palm released held grass")
        if geometry["valid"]:
            require(all(point(geometry[key]) for key in ("root", "mid", "tip")), "Invalid grass geometry")
            require(len(geometry["pinned_vertices"]) == 37
                    and all(point(p) for p in geometry["pinned_vertices"]), "Missing pinned control vertices")
        require(point(frame["press_point"]) and point(frame["mouse_point"]), "Invalid synthetic pointer")
        require(not frame["pointer"]["dragging"] and frame["pointer"]["gesture"] == "none"
                and not frame["pointer"]["preview_enabled"], "Grass input started another gesture")

    events = {frame["event"] for frame in frames}
    require({"real_background_turn_start", "real_background_turn_stop"} <= events
            and bool({"start_approved_grass", "default_double_click"} & events),
            "Missing real controller events")
    if kind == "approved":
        require({"default_double_click", "approved_preview_off_keeps_drag"} <= events,
                "Approved normal entry or preview-off event missing")
    taps = kind == "tap" or (kind == "default" and not adopted)
    require(any(frame["grass_phase"] == "respond" for frame in frames) == taps,
            "Drag replayed the approved click response")
    if kind not in INTERRUPTS:
        require(any(frame["grass_phase"] == "timeout" for frame in frames) == (not taps),
                "Wrong response/timeout branch")
    release_events = {"qt_tip_tap_release", "qt_tip_drag_release", "late_release_after_cancel"}
    release = next((i for i, frame in enumerate(frames) if frame["event"] in release_events), None)
    require(release is not None, "Missing synthetic release")
    require(all(not frame["drag"]["active"] and not frame["drag"]["pressed"]
                for frame in frames[release:]), "Drag replayed after release")
    if kind in INTERRUPTS:
        cancel = next((i for i, frame in enumerate(frames) if frame["event"] == "cancel_" + kind), None)
        require(cancel is not None, "Missing interruption event")
        require(all(not frame["drag"]["active"] and not frame["drag"]["pressed"]
                    for frame in frames[cancel:]), "Drag replayed after interruption")
    elif not taps and kind != "timeout":
        require(any(frame["drag"]["settling"] for frame in frames), "Release rebound missing")
    if kind == "default" and not adopted:
        require(all(not frame["drag"]["enabled"] and not frame["drag"]["active"] for frame in frames),
                "Default input enabled candidate")
    elif kind not in {"tap"}:
        active = [f for f in frames if f["drag"]["moved"] and f["drag"]["active"]]
        require(active, "Drag never crossed movement threshold")
        if kind in {"right", "left", "limit", "default", "approved"}:
            sign = -1 if kind == "left" else 1
            require(max(sign * f["drag"]["offset"] for f in active) >= .34,
                    "Full pull did not reach bounded force")
    final = data["final_drag"]
    require(not final["pressed"] and not final["active"] and not final["settling"]
            and frames[-1]["state"] == 0 and frames[-1]["active_turns"] == 0,
            "Final state or pointer ownership did not recover")
    return data


def compare_pins(scenes):
    """Compare simultaneous Hold poses, avoiding any false static-root claim."""
    selected = [scenes[name] for name in ("grass-drag-right", "grass-drag-left", "grass-drag-limit")
                if name in scenes]
    if len(selected) < 2:
        return {"performed": False, "reason": "at least two opposite/limited pulls required"}
    compared = 0
    for index in range(156):
        frames = [scene["frames"][index] for scene in selected]
        if not all(frame["grass_phase"] == "hold" for frame in frames):
            continue
        reference = frames[0]["drag"]["geometry"]
        for frame in frames[1:]:
            geometry = frame["drag"]["geometry"]
            for first, second in zip([reference["root"]] + reference["pinned_vertices"],
                                     [geometry["root"]] + geometry["pinned_vertices"]):
                require(math.dist(first, second) <= .002, "Pull moved palm/root pinned vertices")
            for axis in ("ParamArmRA", "ParamElbowRA", "ParamWristRA", "ParamHandRGrip"):
                require(abs(frame["parameters"][axis] - frames[0]["parameters"][axis]) < 1e-7,
                        "Pull changed the authored arm pose")
            compared += 1
    require(compared > 10, "Too few simultaneous Hold controls")
    return {"performed": True, "comparisons": compared, "tolerance_px": .002}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--scenario", choices=KINDS, action="append")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    scenarios = ["grass-drag-" + name for name in args.scenario] if args.scenario else SCENARIOS
    if args.verify_only and not args.scenario:
        scenarios = [name for name in SCENARIOS if (output / name / "scene.json").is_file()]
        require(scenarios, "No captured scenes found")
    if not args.verify_only:
        motion = json.loads((ROOT / "assets/motions/grass-touch.motion.json").read_text(encoding="utf8"))
        adopted_asset = motion.get("interaction", {}).get("tipDrag", {}).get("approval") == "approved"
        if args.scenario and adopted_asset and "disable" in args.scenario:
            parser.error("The adopted feature has no draft action; use --scenario approved to test preview-off ownership.")
        if not args.scenario:
            scenes_to_skip = {"disable"} if adopted_asset else {"approved"}
            scenarios = ["grass-drag-" + name for name in KINDS if name not in scenes_to_skip]
    if not args.verify_only:
        if output.exists():
            parser.error("Use a fresh output directory to preserve existing evidence.")
        assets = deployment_snapshot()
        inputs = {"executable_sha256": digest(EXE), "core_sha256": digest(CORE),
                  "review_tool_sha256": digest(Path(__file__))}
        output.mkdir(parents=True)
        (output / "capture-start.json").write_text(json.dumps({"inputs": inputs, "assets": assets}, indent=2) + "\n", encoding="utf8")
    scenes = {}
    for scenario in scenarios:
        folder = output / scenario
        if not args.verify_only:
            env = {key: value for key, value in os.environ.items() if not key.startswith("QT_")}
            result = subprocess.run([str(EXE), "--render-interaction", str(folder), scenario],
                                    cwd=ROOT, env=env, capture_output=True, timeout=60)
            require(folder.is_dir(), f"Native review failed before isolated output creation: {result.returncode}")
            (folder / "capture.log").write_bytes(result.stdout + result.stderr)
            require(result.returncode == 0, f"Native capture failed ({scenario}): {result.returncode}")
        scenes[scenario] = check_scene(folder / "scene.json")
    approvals = {data["adoption"] for data in scenes.values()}
    require(len(approvals) == 1, "Mixed approval states in a capture run")
    adopted = approvals == {"approved"}
    report = {"passed": True, "scope": APPROVED_SCOPE if adopted else SCOPE,
              "adoption": "approved" if adopted else "pending",
              "scenarios": [{"scenario": name, "frames": len(data["frames"]), "checks": len(data["checks"]),
                             "scene_sha256": digest(output / name / "scene.json")} for name, data in scenes.items()],
              "pinned_control_comparison": compare_pins(scenes),
              "physical_windows_pointer_routing": "not_exercised",
              "live_capture_performed": not args.verify_only}
    if not args.verify_only:
        require(deployment_snapshot() == assets, "Assets changed during capture")
        require(inputs == {"executable_sha256": digest(EXE), "core_sha256": digest(CORE),
                           "review_tool_sha256": digest(Path(__file__))}, "Capture inputs changed")
        report.update(inputs)
        report["source_deployment_equal"] = True
        (output / "grass-drag-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
