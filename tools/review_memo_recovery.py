"""Replay note recovery, tray actions and interruptions in an isolated Native process.

Usage: python tools/review_memo_recovery.py <fresh-directory>
       python tools/review_memo_recovery.py <existing-directory> --verify-only

The executable isolates QSettings and its one fixture note before PetWindow is
constructed. It exits before hooks, production IPC or desktop deletion watching.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess

from review_session import ROOT, deployment_snapshot, digest

EXE = ROOT / "build/DesktopCompanion.exe"
CORE = ROOT / "build/Live2DCubismCore.dll"


def verify(output):
    data = json.loads((output / "scene.json").read_text(encoding="utf8"))
    assert data["passed"] and not data["failure"], data["failure"]
    assert data["scenario"] == "memo-recovery"
    assert data["render_backend"] == "cubism_native" and not data["render_error"]
    assert all(data["checks"].values()), data["checks"]
    privacy = data["privacy"]
    assert privacy["fixture_only"] and privacy["isolated_store"] and privacy["isolated_settings"]
    assert not any(privacy[key] for key in ("default_endpoint_used", "real_hooks_used",
                                           "desktop_files_changed", "raw_text_in_trace"))
    frames = data["frames"]
    assert len(frames) == data["frame_count"] == 108
    for index, frame in enumerate(frames):
        assert frame["frame"] == index and frame["file"] == f"frame-{index:03}.png"
        assert (output / frame["file"]).is_file()
        assert all(math.isfinite(value) for value in frame["parameters"].values())
        assert frame["memo"]["store_ok"]
        assert "text" not in frame["memo"]
    assert frames[0]["memo"]["visual"] == "badge" and not frames[0]["memo"]["motion_active"]
    assert data["final_memo"]["origin"] == "none" and data["final_memo"]["visual"] == "none"
    return {"passed": True, "frames": len(frames), "checks": len(data["checks"]),
            "scope": data["scope"], "privacy": privacy, "scene_sha256": digest(output / "scene.json")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if args.verify_only:
        report = verify(output)
        report["live_capture_performed"] = False
    else:
        if output.exists():
            parser.error("Use a fresh output directory to preserve existing evidence.")
        assets = deployment_snapshot()
        inputs = {"executable_sha256": digest(EXE), "core_sha256": digest(CORE),
                  "review_tool_sha256": digest(Path(__file__))}
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("QT_"):
                env.pop(key)
        result = subprocess.run([str(EXE), "--render-interaction", str(output), "memo-recovery"],
                                cwd=ROOT, env=env, capture_output=True, timeout=55)
        if not output.is_dir():
            raise RuntimeError(f"Review failed before isolated output creation: {result.returncode}")
        (output / "capture.log").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            failure = json.loads((output / "scene.json").read_text(encoding="utf8"))["failure"] \
                if (output / "scene.json").is_file() else "initialization or settings isolation failed"
            raise RuntimeError(f"Native review failed ({result.returncode}): {failure}")
        report = verify(output)
        assert deployment_snapshot() == assets, "Assets changed during capture"
        assert inputs == {"executable_sha256": digest(EXE), "core_sha256": digest(CORE),
                          "review_tool_sha256": digest(Path(__file__))}, "Capture inputs changed"
        report.update(inputs)
        report["live_capture_performed"] = True
        report["source_deployment_equal"] = True
        (output / "memo-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
