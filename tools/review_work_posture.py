"""Capture the real work-posture tray choices with isolated settings and turns.

python tools/review_work_posture.py <fresh-directory> [--executable path/to/DesktopCompanion.exe]
python tools/review_work_posture.py <existing-capture> --verify-only

The review never opens production IPC, installs hooks, or touches desktop files.
Each 0.05 s step has a complete parameter ledger. Selected Native frames include
a continuous 6 s standing transition sampled at 10 fps, plus two menu snapshots.
Saved and invalid preference restoration each run in their own fresh process;
the application owns a single Cubism canvas throughout every process lifetime.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess

from review_session import ROOT, digest, input_files

SCENARIO = "work-posture-preference"
SCOPE = "native_work_posture_real_qaction_and_fixture_turn"
SNAPSHOTS = {0, 1, 19, 20, 100, 199, 218, 219, 230, 240, 250, 260, 280,
             299, 300, 305, 310, 315, 320, 330, 340, 360, 399, 419, 420,
             430, 440, 450, 460, 480, 520, 579, 580, 590, 600, 610, 620,
             640, 680, 740, 799} | set(range(280, 401, 2))
REQUIRED_CHECKS = {
    "native_backend", "default_automatic", "idle_menu_only_saves_preference",
    "menu_idle_snapshot", "first_laptop_settled",
    "menu_shows_preference_before_pose_recovers", "menu_pending_snapshot",
    "standing_settled_without_reset", "second_laptop_settled",
    "menu_exclusive_and_tracks_preference", "only_fixture_turn_controls_busy",
    "preference_preserves_work_clocks", "ten_second_sit_guard",
    "idle_never_enters_work_pose", "stop_recovers_idle_with_saved_preference",
    "stable_string_saved",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def deployed_assets(build):
    source_names = {path.name for path in (ROOT / "assets/motions").glob("*.motion.json")}
    deployed_names = {path.name for path in (build / "assets/motions").glob("*.motion.json")}
    require(source_names == deployed_names, "Deployed motion set differs from source")
    records = []
    for source in input_files():
        relative = source.relative_to(ROOT)
        deployed = build / relative
        require(deployed.is_file(), f"Missing deployed asset: {relative}")
        record = {"path": relative.as_posix(), "source_sha256": digest(source),
                  "deployed_sha256": digest(deployed)}
        require(record["source_sha256"] == record["deployed_sha256"],
                f"Asset deployment differs: {relative}")
        records.append(record)
    require(records, "No review assets found")
    return records


def check_scene(path):
    path = Path(path)
    data = json.loads(path.read_text(encoding="utf8"))
    require(data["scenario"] == SCENARIO and data["scope"] == SCOPE, "Unexpected capture scope")
    require(data["passed"] is True and not data["failure"], f"Native review failed: {data['failure']}")
    require(data["render_backend"] == "cubism_native" and not data["render_error"], "Native render failed")
    require(REQUIRED_CHECKS <= data["checks"].keys()
            and all(value is True for value in data["checks"].values()), "Required checks failed or missing")
    privacy = data["privacy"]
    require(privacy["fixture_only"] is True and privacy["isolated_settings"] is True
            and all(privacy[key] is False for key in
                    ("default_endpoint_used", "real_hooks_used", "desktop_files_changed")),
            "Review isolation contract failed")
    frames = data["frames"]
    require(len(frames) == data["frame_count"] == 800, "Incomplete manual frame ledger")
    previous_busy = previous_continuous = 0.0
    for i, frame in enumerate(frames):
        require(frame["frame"] == i and abs(frame["time"] - (i + 1) * .05) < 1e-8
                and frame["dt"] == .05, "Manual clock changed")
        expected_file = f"frame-{i:03}.png" if i in SNAPSHOTS else ""
        require(frame["file"] == expected_file, "Native sampling schedule changed")
        if expected_file:
            require((path.parent / expected_file).is_file(), "Missing sampled Native frame")
        require(frame["parameters"] and all(finite(value) for value in frame["parameters"].values()),
                "Non-finite model parameter")
        posture, menu, params = frame["posture"], frame["menu"], frame["parameters"]
        require(set(menu) == {"automatic", "standing", "laptop"}
                and sum(value is True for value in menu.values()) == 1
                and menu[posture["preference"]] is True, "Menu differs from saved preference")
        require(all(finite(posture[key]) and posture[key] >= 0
                    for key in ("busy_time", "continuous_busy_time", "next_choice_time", "desk_visible")),
                "Invalid work clocks")
        busy = 20 <= i < 580
        require(posture["controller_state"] == (1 if busy else 0)
                and posture["active_turns"] == (1 if busy else 0), "Preference invented or cleared work")
        expected_preference = ("automatic" if i == 0 else
                               "standing" if 300 <= i < 420 else "laptop")
        require(posture["preference"] == expected_preference, "Preference action not applied")
        if busy:
            for key, previous in (("busy_time", previous_busy), ("continuous_busy_time", previous_continuous)):
                require(previous <= posture[key] <= previous + .05001, "Preference reset or advanced work time")
            previous_busy, previous_continuous = posture["busy_time"], posture["continuous_busy_time"]
            if posture["continuous_busy_time"] < 10 - 1e-6:
                require(all(abs(params[axis]) < .002 for axis in
                            ("ParamSitPose", "ParamDeskVisible", "ParamLaptopVisible")), "10 s sit guard bypassed")
    require(frames[1]["event"] == "tray_laptop_idle"
            and frames[20]["event"] == "fixture_turn_started"
            and frames[300]["event"] == "tray_standing_while_seated"
            and frames[420]["event"] == "tray_laptop_again"
            and frames[580]["event"] == "fixture_turn_stopped", "Required event order changed")
    require(frames[300]["parameters"]["ParamSitPose"] >= .98
            and frames[300]["menu"]["standing"] is True, "Menu was tied to visible pose")
    for i in (299, 579):
        require(frames[i]["posture"]["desk_phase"] == "work"
                and all(frames[i]["parameters"][axis] >= .98 for axis in
                        ("ParamSitPose", "ParamBusyLaptop", "ParamDeskVisible", "ParamLaptopVisible")),
                "Laptop did not settle")
    for i in (419, 799):
        require(frames[i]["posture"]["desk_phase"] == "hidden"
                and all(abs(frames[i]["parameters"][axis]) < .002 for axis in
                        ("ParamSitPose", "ParamBusyLaptop", "ParamDeskVisible", "ParamLaptopVisible")),
                "Standing did not recover")
    require(data["persistence"]["saved_value"] == "laptop"
            and Path(data["persistence"]["settings_file"]).is_file(), "Actual saved profile is absent")
    require(all((path.parent / name).is_file() for name in
                ("menu-idle-laptop.png", "menu-pending-standing.png")), "Missing menu screenshots")
    return data


def check_restart(path, source_settings, invalid):
    path = Path(path)
    data = json.loads(path.read_text(encoding="utf8"))
    scenario = "work-posture-invalid" if invalid else "work-posture-restore"
    expected = "automatic" if invalid else "laptop"
    require(data["scenario"] == scenario
            and data["scope"] == "native_work_posture_fresh_process_restore"
            and data["passed"] is True, "Fresh-process preference restore failed")
    require(data["render_backend"] == "cubism_native" and not data["render_error"], "Restart Native render failed")
    required = {"native_backend", "fixture_settings_copied", "restored_preference",
                "restart_does_not_invent_work", "menu_tracks_restored_preference", "fixture_setting_preserved"}
    require(required <= data["checks"].keys()
            and all(value is True for value in data["checks"].values()), "Restart checks missing or failed")
    state = data["posture"]
    require(state["preference"] == expected and state["controller_state"] == 0
            and state["active_turns"] == 0 and state["laptop_selected"] is False, "Restart invented work")
    require(set(data["menu"]) == {"automatic", "standing", "laptop"}
            and sum(value is True for value in data["menu"].values()) == 1
            and data["menu"][expected] is True, "Restart menu selection incorrect")
    require(data["parameters"] and all(finite(value) for value in data["parameters"].values()),
            "Restart parameters invalid")
    require(data["frame"] == "frame-000.png" and (path.parent / data["frame"]).is_file(),
            "Restart Native frame missing")
    require(Path(data["source_settings_file"]).resolve() == source_settings.resolve()
            and Path(data["settings_file"]).resolve().is_relative_to(path.parent.resolve()),
            "Restart settings escaped fixture roots")
    privacy = data["privacy"]
    require(privacy["fixture_only"] is True and privacy["isolated_settings"] is True
            and all(privacy[key] is False for key in
                    ("default_endpoint_used", "real_hooks_used", "desktop_files_changed")),
            "Restart isolation contract failed")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--executable", type=Path, default=ROOT / "build/DesktopCompanion.exe")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    output, executable = args.output.resolve(), args.executable.resolve()
    scene = output / SCENARIO
    if not args.verify_only:
        require(not output.exists(), "Use a fresh output directory to preserve earlier evidence")
        assets = deployed_assets(executable.parent)
        inputs = {"executable_sha256": digest(executable),
                  "core_sha256": digest(executable.parent / "Live2DCubismCore.dll"),
                  "review_tool_sha256": digest(Path(__file__))}
        output.mkdir(parents=True)
        (output / "capture-start.json").write_text(
            json.dumps({"inputs": inputs, "assets": assets}, indent=2) + "\n", encoding="utf8")
        env = {key: value for key, value in os.environ.items() if not key.startswith("QT_")}
        result = subprocess.run([str(executable), "--render-interaction", str(scene), SCENARIO],
                                cwd=ROOT, env=env, capture_output=True, timeout=115)
        require(scene.is_dir(), f"Capture failed before isolated output creation: {result.returncode}")
        (scene / "capture.log").write_bytes(result.stdout + result.stderr)
        require(result.returncode == 0, f"Native review returned {result.returncode}; inspect scene.json")
    data = check_scene(scene / "scene.json")
    source_settings = Path(data["persistence"]["settings_file"])
    require(source_settings.resolve().is_relative_to(scene.resolve()), "Primary settings escaped fixture root")
    source_hash = digest(source_settings)
    restarts = []
    for invalid in (False, True):
        scenario = "work-posture-invalid" if invalid else "work-posture-restore"
        folder = output / scenario
        if not args.verify_only:
            result = subprocess.run([str(executable), "--render-interaction", str(folder), scenario,
                                     str(source_settings)], cwd=ROOT, env=env,
                                    capture_output=True, timeout=30)
            require(folder.is_dir(), f"Restart failed before isolated output creation: {result.returncode}")
            (folder / "capture.log").write_bytes(result.stdout + result.stderr)
            require(result.returncode == 0, f"Fresh-process {scenario} returned {result.returncode}")
        restored = check_restart(folder / "scene.json", source_settings, invalid)
        require(digest(source_settings) == source_hash, "Restart modified the primary saved profile")
        restarts.append({"scenario": scenario, "passed": restored["passed"],
                         "checks": len(restored["checks"]),
                         "report_sha256": digest(folder / "scene.json")})
    report = {"passed": True, "scope": SCOPE, "manual_frames": len(data["frames"]),
              "sampled_native_frames": len(SNAPSHOTS), "menu_snapshots": 2,
              "checks": len(data["checks"]), "restart_reviews": restarts,
              "restart_frames": len(restarts), "scene_sha256": digest(scene / "scene.json"),
              "live_capture_performed": not args.verify_only,
              "not_exercised": ["physical tray clicks", "production hook delivery"],
              "visual_naturalness": "requires_rendered_sample_review"}
    if not args.verify_only:
        require(deployed_assets(executable.parent) == assets, "Assets changed during capture")
        require(inputs == {"executable_sha256": digest(executable),
                           "core_sha256": digest(executable.parent / "Live2DCubismCore.dll"),
                           "review_tool_sha256": digest(Path(__file__))}, "Capture inputs changed")
        report.update(inputs)
        report["source_deployment_equal"] = True
        (output / "work-posture-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
