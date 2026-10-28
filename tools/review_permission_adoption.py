"""Exercise approved automatic permission cues through private IPC and Native.

python tools/review_permission_adoption.py <fresh-directory>
No real agent requests, production endpoint, installed hook, or user profile.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess

from review_session import ROOT, digest
from review_work_posture import deployed_assets

KINDS = ("standing", "laptop")
CHECKS = {"ordinary_auto_start", "duplicate_keeps_phase", "unbound_resolution_keeps_cue",
          "preview_off_retains_approved", "bound_resolution_cancels_only_cue", "same_request_never_replays",
          "finite_cue_returns_to_busy", "quiet_consumes_nomination", "quiet_resume_no_replay",
          "new_turn_interrupts_cue", "foreground_refuses_without_queue", "foreground_exit_no_replay",
          "session_end_preserves_other_client", "new_execution_can_reuse_request_id", "reset_cleans_fact_and_cue"}


def require(value, message):
    if not value:
        raise ValueError(message)


def compare_choreography():
    previous = json.loads(subprocess.check_output(
        ["git", "show", "596558c:assets/motions/permission-cue.motion.json"], cwd=ROOT))
    current = json.loads((ROOT / "assets/motions/permission-cue.motion.json").read_text(encoding="utf8"))
    require(current["approval"] == current["conceptApproval"] == "approved", "Approval missing")
    metadata = {"approval", "conceptApproval", "directorNotes", "reviewNotes"}
    require({k: v for k, v in previous.items() if k not in metadata}
            == {k: v for k, v in current.items() if k not in metadata}, "Approved choreography changed")


def verify(output):
    reports = []
    for kind in KINDS:
        data = json.loads((output / kind / "scene.json").read_text(encoding="utf8"))
        require(data["passed"] and not data["failure"], f"{kind}: {data['failure']}")
        require(data["scope"] == "private_source_controller_live_window_approved_automatic_entry", "Wrong chain")
        require(data["render_backend"] == "cubism_native" and not data["render_error"], "Native failed")
        require(CHECKS <= data["checks"].keys() and all(data["checks"].values()), "Missing/failed checks")
        frames = data["frames"]
        require(len(frames) == data["frame_count"] == 840, "Incomplete frame ledger")
        for i, frame in enumerate(frames):
            require(frame["frame"] == i and abs(frame["time"] - (i + 1) * .05) < 1e-8, "Clock changed")
            expected = 0 if i < 10 or i >= 628 else 1 if i < 80 or 568 <= i < 600 else 3 if 490 <= i < 500 else 2
            require(frame["active_turns"] == expected, "Permission altered true Busy count")
            require(not frame["reaction"]["preview_enabled"], "Approved entry required previews")
            if frame["file"]:
                require((output / kind / frame["file"]).is_file(), "Missing Native sample")
        starts = [i for i in range(1, len(frames))
                  if frames[i]["reaction"]["reaction"] == "permission-cue"
                  and frames[i-1]["reaction"]["reaction"] != "permission-cue"]
        require(starts == [300, 360, 480, 560, 620], f"Cue duplicated or replayed: {starts}")
        contact = ("ParamSitPose", "ParamBusyLaptop", "ParamLaptopVisible", "ParamDeskVisible",
                   "ParamArmLA", "ParamArmRA", "ParamElbowLA", "ParamElbowRA", "ParamWristRA")
        baseline = frames[360]["parameters"]
        for frame in frames[360:415]:
            require(all(abs(frame["parameters"][axis] - baseline[axis]) < .002 for axis in contact),
                    "Approved automatic cue moved contact posture")
        reports.append({"kind": kind, "frames": len(frames), "checks": len(data["checks"]),
                        "automatic_starts": starts, "passed": True})
    return {"passed": True, "scope": "approved automatic permission cue adoption", "approval": "approved",
            "unchanged_from_approved_rev1": True, "scenarios": reports,
            "real_model_task_executed": False, "physical_mouse_tray_tested": False}


def compose(output):
    from PIL import Image, ImageDraw
    sampled = (358, 364, 374, 386, 404, 422)
    board = Image.new("RGB", (1800, 660), "#edf4fc")
    pen = ImageDraw.Draw(board)
    for row, kind in enumerate(KINDS):
        for col, frame in enumerate(sampled):
            image = Image.open(output / kind / f"frame-{frame:03}.png").convert("RGBA")
            image.thumbnail((280, 280), Image.Resampling.LANCZOS)
            board.paste(image, (col * 300 + (300 - image.width) // 2, row * 330 + 35), image)
            pen.text((col * 300 + 12, row * 330 + 10), f"{kind} / auto {(frame-360)*.05:.2f}s", fill="#243d5c")
    board.save(output / "approved-key-poses.png")
    images = []
    for frame in range(358, 424, 2):
        canvas = Image.new("RGB", (600, 320), "#edf4fc")
        pen = ImageDraw.Draw(canvas)
        for col, kind in enumerate(KINDS):
            image = Image.open(output / kind / f"frame-{frame:03}.png").convert("RGBA")
            image.thumbnail((280, 280), Image.Resampling.LANCZOS)
            canvas.paste(image, (col * 300 + (300 - image.width) // 2, 30), image)
            pen.text((col * 300 + 12, 8), f"{kind} / approved automatic", fill="#243d5c")
        images.append(canvas)
    images[0].save(output / "approved-automatic.gif", save_all=True, append_images=images[1:], duration=100, loop=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    require(not output.exists(), "Refusing existing capture directory")
    compare_choreography()
    output.mkdir(parents=True)
    assets = deployed_assets(ROOT / "build")
    (output / "assets.json").write_text(json.dumps(assets, ensure_ascii=False, indent=2), encoding="utf8")
    names = ("src/PermissionAdoptionReview.cpp", "src/WorkReactionCoordinator.cpp", "src/WorkReactionCoordinator.h",
             "src/PetController.cpp", "src/PetController.h", "src/ParameterMotion.cpp", "src/ParameterMotion.h",
             "src/main.cpp", "src/PetWindow.cpp", "assets/motions/permission-cue.motion.json",
             "tools/review_permission_adoption.py", "build/DesktopCompanion.exe")
    inputs = {name: digest(ROOT / name) for name in names}
    (output / "capture-inputs.json").write_text(json.dumps(inputs, indent=2), encoding="utf8")
    env = os.environ.copy()
    env["PATH"] = str(ROOT / "build") + os.pathsep + "E:/Qt/release/bin/bin" + os.pathsep + env.get("PATH", "")
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(ROOT / "build/platforms")

    def capture(kind):
        child = subprocess.run([str(ROOT / "build/DesktopCompanion.exe"), "--render-interaction",
                                str(output / kind), f"permission-adoption-{kind}"], cwd=ROOT,
                               env=env, capture_output=True, timeout=125)
        (output / f"{kind}.log").write_bytes(child.stdout + child.stderr)
        require(child.returncode == 0, f"{kind}: exit {child.returncode}; see scene.json")

    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(capture, KINDS))
    require(all(digest(ROOT / name) == sha for name, sha in inputs.items()), "Inputs changed while capturing")
    report = verify(output)
    compose(output)
    (output / "adoption-check.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
