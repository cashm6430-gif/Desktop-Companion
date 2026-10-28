"""Capture the permission cue on R19; no production hook or profile.

python tools/review_permission_cue.py <fresh-directory> [--approval approved|pending]
python tools/review_permission_cue.py <existing-directory> --verify-only
One Cubism canvas per child process. Numeric checks do not approve artwork.
Verification of an old candidate is read-only and retains its recorded approval.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import subprocess

from review_session import ROOT, digest
from review_work_posture import deployed_assets

SCENARIOS = ("standing", "laptop", "stop", "delete", "drag", "preview-off")
CONTACT = ("ParamSitPose", "ParamBusyLaptop", "ParamLaptopVisible", "ParamDeskVisible",
           "ParamArmLA", "ParamArmRA", "ParamElbowLA", "ParamElbowRA", "ParamWristRA")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(output, approval):
    reports = []
    for kind in SCENARIOS:
        path = output / kind / "scene.json"
        data = json.loads(path.read_text(encoding="utf8"))
        require(data["scenario"] == f"permission-cue-{kind}", "Wrong scenario")
        require(data["capture"]["render_backend"] == "cubism_native"
                and not data["capture"]["render_error"], "Native render failed")
        frames = data["frames"]
        require(len(frames) == 75 and all((path.parent / f"frame-{i:03}.png").is_file()
                                        for i in range(75)), "Incomplete continuous capture")
        require(data.get("permission_cue_approval", approval) == approval, "Capture approval differs from its receipt")
        require(all(frame["default_pause_accepted"] == (approval == "approved")
                    for frame in frames if frame["time"] >= .4),
                "Ordinary entry did not follow the captured clip's approval")
        active = [frame for frame in frames if frame["interaction"] == "permission-cue"]
        require(active, "Permission cue never started")
        for frame in frames:
            require(all(math.isfinite(v) for v in frame["parameters"].values()), "Non-finite output")
        for before, after in zip(frames, frames[1:]):
            if before["interaction"] == "permission-cue" and after["state"] == 1:
                require(abs(after["busy_phase"] - before["busy_phase"]) < 1e-8,
                        "Cue advanced or reset the original work phase")
        if kind in ("standing", "laptop") or (kind == "preview-off" and approval == "approved"):
            require(not frames[-1]["interaction"] and frames[-1]["state"] == 1,
                    "Candidate did not release to Busy")
            require(frames[-1]["busy_phase"] > active[-1]["busy_phase"], "Work did not resume")
            baseline = active[0]["parameters"]
            require(all(abs(frame["parameters"][axis] - baseline[axis]) < .002
                        for frame in active for axis in CONTACT), "Contact posture drifted")
        else:
            require(not any(frame["interaction"] == "permission-cue"
                            for frame in frames if frame["time"] >= 1.2), "Interrupted cue replayed")
        reports.append({"scenario": kind, "frames": len(frames), "reaction_frames": len(active),
                        "scene_sha256": digest(path), "passed": True})
    inputs = json.loads((output / "capture-inputs.json").read_text(encoding="utf8"))
    clip_sha = inputs["assets/motions/permission-cue.motion.json"]
    return {"schema": 1, "passed": True, "scenarios": reports,
            "scope": "real ParameterMotion ordinary/draft entry and Native R19 continuous frames",
            "approval": approval, "ordinary_entry_accepted": approval == "approved", "automatic_entry_tested": False,
            "privacy": {"isolated_settings": True, "fixture_only": True,
                        "default_endpoint_used": False, "real_hooks_used": False,
                        "desktop_files_changed": False}, "clip_sha256": clip_sha}


def compose(output, approval="pending"):
    from PIL import Image, ImageDraw
    times = (6, 12, 20, 29, 40, 58)
    board = Image.new("RGB", (6 * 300, 2 * 330), "#edf4fc")
    draw = ImageDraw.Draw(board)
    animations = []
    for row, kind in enumerate(("standing", "laptop")):
        for col, frame in enumerate(times):
            image = Image.open(output / kind / f"frame-{frame:03}.png").convert("RGBA")
            image.thumbnail((280, 280), Image.Resampling.LANCZOS)
            x, y = col * 300 + (300 - image.width) // 2, row * 330 + 35
            board.paste(image, (x, y), image)
            draw.text((col * 300 + 12, row * 330 + 10), f"{kind}  {frame / 15:.2f}s", fill="#243d5c")
    board.save(output / "key-poses.png")
    detail = Image.new("RGB", (2240, 1120), "#122132")
    pen = ImageDraw.Draw(detail)
    for row, kind in enumerate(("standing", "laptop")):
        for col, frame in enumerate((14, 23, 28, 45)):
            image = Image.open(output / kind / f"frame-{frame:03}.png").convert("RGBA")
            image.thumbnail((540, 510), Image.Resampling.LANCZOS)
            detail.paste(image, (col * 560 + (560 - image.width) // 2, row * 560 + 30), image)
            pen.text((col * 560 + 12, row * 560 + 10), f"{kind} / {frame / 15:.2f}s", fill="#d8e6fa")
    detail.save(output / "dark-details.png")
    for i in range(75):
        canvas = Image.new("RGB", (600, 320), "#edf4fc")
        pen = ImageDraw.Draw(canvas)
        for col, kind in enumerate(("standing", "laptop")):
            image = Image.open(output / kind / f"frame-{i:03}.png").convert("RGBA")
            image.thumbnail((280, 280), Image.Resampling.LANCZOS)
            canvas.paste(image, (col * 300 + (300 - image.width) // 2, 30), image)
            pen.text((col * 300 + 12, 8), f"{kind} / {approval}  {i / 15:.2f}s", fill="#243d5c")
        animations.append(canvas)
    animations[0].save(output / "permission-cue.gif", save_all=True, append_images=animations[1:],
                       duration=67, loop=0, optimize=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--approval", choices=("approved", "pending"))
    args = parser.parse_args()
    output = args.output.resolve()
    if args.verify_only:
        metadata = output / "capture-metadata.json"
        receipt = metadata if metadata.is_file() else output / "cue-check.json"
        approval = json.loads(receipt.read_text(encoding="utf8"))["approval"]
        require(args.approval is None or args.approval == approval, "Cannot relabel historical approval")
        verify(output, approval)
        print(json.dumps({"passed": True, "scenarios": len(SCENARIOS), "frames": 450,
                          "approval": approval, "read_only": True, "output": str(output)}))
        return
    else:
        require(not output.exists(), "Refusing an existing capture directory")
        clip = json.loads((ROOT / "assets/motions/permission-cue.motion.json").read_text(encoding="utf8"))
        approval = clip["approval"]
        require(approval in ("approved", "pending"), "Unsupported clip approval")
        require(args.approval is None or args.approval == approval, "Requested approval differs from source asset")
        output.mkdir(parents=True)
        (output / "capture-metadata.json").write_text(json.dumps({"approval": approval,
            "revision": clip.get("revision"), "ordinary_entry": approval == "approved"}, indent=2), encoding="utf8")
        records = deployed_assets(ROOT / "build")
        (output / "assets.json").write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf8")
        sources = ["src/ParameterMotion.cpp", "src/ParameterMotion.h", "src/PetWindow.cpp", "src/main.cpp",
                   "assets/motions/permission-cue.motion.json", "tools/review_permission_cue.py"]
        inputs = {name: digest(ROOT / name) for name in sources}
        inputs["build/DesktopCompanion.exe"] = digest(ROOT / "build/DesktopCompanion.exe")
        (output / "capture-inputs.json").write_text(json.dumps(inputs, indent=2), encoding="utf8")
        env = os.environ.copy()
        env["PATH"] = str(ROOT / "build") + os.pathsep + "E:/Qt/release/bin/bin" + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(ROOT / "build/platforms")

        def capture(kind):
            result = subprocess.run([str(ROOT / "build/DesktopCompanion.exe"), "--render-interaction",
                                     str(output / kind), f"permission-cue-{kind}"],
                                    cwd=ROOT, env=env, capture_output=True, timeout=45)
            (output / f"{kind}.log").write_bytes(result.stdout + result.stderr)
            require(result.returncode == 0, f"{kind}: capture exited {result.returncode}")

        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(capture, SCENARIOS))
        require(all(digest(ROOT / name) == sha for name, sha in inputs.items()), "Capture inputs changed while running")
    report = verify(output, approval)
    compose(output, approval)
    (output / "cue-check.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps({"passed": True, "scenarios": len(SCENARIOS), "frames": 450,
                      "approval": approval, "output": str(output)}))


if __name__ == "__main__":
    main()
