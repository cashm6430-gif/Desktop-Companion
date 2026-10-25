"""Review real Qt window dragging, held-pointer stops and release/interruption.

python tools/review_drag_input.py <fresh-directory> [--scenario idle]
python tools/review_drag_input.py <existing-full-capture> --verify-only

The isolated process uses synthetic Qt input, fixture turns and offscreen
window moves. It never invokes physical mouse input or a desktop file action.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess

from review_session import ROOT, deployment_snapshot, digest

KINDS = ("idle", "busy", "delete", "grass", "ungrab")
SCENARIOS = tuple("drag-input-" + kind for kind in KINDS)
SCOPE = "native_drag_stop_and_release_synthetic_qt_input"
EXE = ROOT / "build/DesktopCompanion.exe"
CORE = ROOT / "build/Live2DCubismCore.dll"


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
    require(data["scenario"] in SCENARIOS and data["scope"] == SCOPE, "Unexpected capture scope")
    require(data["passed"] is True and data["failure"] == "", "Native review failed")
    require(data["render_backend"] == "cubism_native" and not data["render_error"], "Native renderer error")
    kind = data["scenario"].removeprefix("drag-input-")
    required_checks = {"native_backend", "isolated_settings", "body_press_not_head", "body_press_pending",
        "actual_path_stays_160_while_still", "still_pointer_keeps_drag_owner", "no_false_long_drag_curiosity",
        "still_input_expires", "released_pointer_stays_clear", "settle_finishes_without_replay", "drag_seen",
        "actual_window_displacement_160", "final_pointer_and_motion_clear"}
    if kind == "busy":
        required_checks.add("busy_pose_and_real_turn_preserved")
    if kind in {"delete", "grass"}:
        required_checks.add("foreground_cancels_drag_immediately")
    require(required_checks <= data["checks"].keys() and all(v is True for v in data["checks"].values()),
            "Required Native checks failed or missing")
    privacy = data["privacy"]
    require(privacy["fixture_only"] is True and privacy["isolated_settings"] is True,
            "Capture must isolate settings")
    require(all(privacy[k] is False for k in ("default_endpoint_used", "real_hooks_used", "desktop_files_changed")),
            "Capture accessed production events")
    require(data["expected_actual_path_px"] == 160, "Input path contract changed")
    frames = data["frames"]
    require(len(frames) == data["frame_count"] == 120, "Expected 120 complete frames")
    for i, frame in enumerate(frames):
        require(frame["frame"] == i and frame["file"] == f"frame-{i:03}.png"
                and (path.parent / frame["file"]).is_file(), "Incomplete Native frame sequence")
        require(finite(frame["time"]) and abs(frame["time"] - (i + 1) * .05) < 1e-8
                and finite(frame["dt"]) and abs(frame["dt"] - .05) < 1e-9, "Manual frame clock changed")
        require(frame["parameters"] and all(finite(v) for v in frame["parameters"].values()),
                "Non-finite model parameter")
        drag = frame["drag"]
        require(all(isinstance(drag[k], bool) for k in
                    ("following", "curious_triggered", "dragging", "drag_motion_active")), "Invalid drag flags")
        require(all(finite(drag[k]) and drag[k] >= 0 for k in ("input_age", "travelled")),
                "Invalid input freshness or travelled distance")
        require(point(frame["window_position"]) and point(frame["pointer_global"]), "Invalid fixture coordinates")
        require(not drag["preview_enabled"] and not drag["wave_active"]
                and drag["reaction"] != "head-pat", "Drag started a competing gesture")
        if 12 <= i < 40:
            require(drag["following"] and drag["dragging"] and drag["gesture"] == "drag"
                    and abs(drag["travelled"] - 160) < 1e-6 and not drag["curious_triggered"],
                    "Stationary pointer invented travel or lost its owner")
        if i >= 40:
            require(not drag["following"] and not drag["dragging"] and drag["gesture"] == "none",
                    "Drag replayed after release")
            if kind in {"delete", "grass"}:
                require(not drag["drag_motion_active"], "Foreground event retained drag overlay")
        if i >= 92:
            require(not drag["drag_motion_active"], "Release settle exceeded its existing deadline")
        if kind == "busy":
            require(frame["active_turns"] == 1 and frame["state"] == 1, "Drag altered the real Busy state")
            require(all(frame["parameters"][axis] >= .98 for axis in
                        ("ParamSitPose", "ParamBusyLaptop", "ParamDeskVisible", "ParamLaptopVisible")),
                    "Drag lost seated work posture or props")
    require(frames[39]["drag"]["input_age"] >= 1.3, "Still pointer input did not expire")
    require(sum(f["event"] == "actual_mouse_move_20px" for f in frames) == 8, "Actual input path changed")
    require(frames[3]["event"] == "body_press" and frames[41]["event"] == "stale_move_after_release",
            "Press/stale-input checks missing")
    expected_release = {"delete": "delete_interrupt", "grass": "grass_interrupt", "ungrab": "ungrab"}.get(kind, "mouse_release")
    require(frames[40]["event"] == expected_release, "Release/interruption event missing")
    if kind == "ungrab":
        require(frames[44]["event"] == "duplicate_ungrab", "Duplicate ungrab check missing")
    before, after = frames[3]["window_position"], frames[-1]["window_position"]
    require(abs(after[0] - before[0] - 160) < 1e-6 and abs(after[1] - before[1]) < 1e-6,
            "Window did not follow exactly the actual input displacement")
    final = data["final_drag"]
    require(not final["following"] and not final["dragging"] and not final["drag_motion_active"],
            "Final pointer or overlay did not clear")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--scenario", choices=KINDS, action="append")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    scenarios = ["drag-input-" + name for name in args.scenario] if args.scenario else SCENARIOS
    if args.verify_only and not args.scenario:
        scenarios = [name for name in SCENARIOS if (output / name / "scene.json").is_file()]
        require(scenarios, "No captured scenes found")
    if not args.verify_only:
        if output.exists():
            parser.error("Use a fresh output directory to preserve existing evidence.")
        assets = deployment_snapshot()
        inputs = {"executable_sha256": digest(EXE), "core_sha256": digest(CORE),
                  "review_tool_sha256": digest(Path(__file__))}
        output.mkdir(parents=True)
        (output / "capture-start.json").write_text(json.dumps({"inputs": inputs, "assets": assets}, indent=2) + "\n", encoding="utf8")
    scenes = []
    for scenario in scenarios:
        folder = output / scenario
        if not args.verify_only:
            env = {k: v for k, v in os.environ.items() if not k.startswith("QT_")}
            result = subprocess.run([str(EXE), "--render-interaction", str(folder), scenario],
                                    cwd=ROOT, env=env, capture_output=True, timeout=60)
            require(folder.is_dir(), f"Review failed before isolated output creation: {result.returncode}")
            (folder / "capture.log").write_bytes(result.stdout + result.stderr)
            require(result.returncode == 0, f"Native capture failed ({scenario}): {result.returncode}")
        data = check_scene(folder / "scene.json")
        scenes.append({"scenario": scenario, "frames": len(data["frames"]), "checks": len(data["checks"]),
                       "scene_sha256": digest(folder / "scene.json")})
    report = {"passed": True, "scope": SCOPE, "scenarios": scenes,
              "live_capture_performed": not args.verify_only,
              "physical_windows_pointer_routing": "not_exercised",
              "visual_naturalness": "requires_rendered_sample_review"}
    if not args.verify_only:
        require(deployment_snapshot() == assets, "Assets changed during capture")
        require(inputs == {"executable_sha256": digest(EXE), "core_sha256": digest(CORE),
                           "review_tool_sha256": digest(Path(__file__))}, "Capture inputs changed")
        report.update(inputs)
        report["source_deployment_equal"] = True
        (output / "drag-input-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
