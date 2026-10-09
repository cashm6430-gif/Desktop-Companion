"""Stage the optional Qt/Godot host and capture isolated semantic event replay.

Default mode never enables Shell/hook listeners. --live is an explicit manual
launch using real sources, after the user closes the previous pet.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

from review_toon_prototype import (
    GODOT_DEFAULT, MODEL_SOURCE, ROOT, audit_window, fingerprint, import_project,
    load_review_profile, stage, write_json,
)

REPLAY = ROOT / "art/3d/integration/replay-smoke-v1.json"
PROFILE = ROOT / "art/3d/integration/review-profile.json"
QT_SOURCE_FILES = (
    "CMakeLists.txt", "src/main.cpp", "src/ToonHost.cpp", "src/ToonHost.h",
    "src/ToonEventBridge.cpp", "src/ToonEventBridge.h", "src/PetController.cpp",
    "src/PetController.h", "src/CodexTurnSource.cpp", "src/CodexTurnSource.h",
    "src/DesktopDeleteSource.cpp", "src/DesktopDeleteSource.h",
)


def launch_environment():
    environment = os.environ.copy()
    # Qt packages use a prefix named bin; DLLs are in its own bin child.
    qt_dlls = Path(os.environ.get("DESKTOP_COMPANION_QT_DLLS", "E:/Qt/release/bin/bin"))
    if qt_dlls.is_dir():
        environment["PATH"] = str(qt_dlls) + os.pathsep + environment.get("PATH", "")
    return environment


def package_host(host):
    """Deploy our own executable/dependencies into a normal fresh runtime dir.

    Some historical workspace files carry inherited Low integrity labels.
    Ordinary deployment copies preserve bytes without editing source ACLs or
    system policy; running the package also matches normal installed behavior.
    """
    if sys.platform != "win32":
        return host, None
    directory = Path(tempfile.mkdtemp(prefix="desktop-companion-toon-runtime-"))
    files = [host, *sorted(host.parent.glob("*.dll"))]
    hook = host.parent / "DesktopCompanionHook.exe"
    if hook.is_file():
        files.append(hook)
    if not (host.parent / "platforms/qwindows.dll").is_file():
        raise FileNotFoundError("Qt platform plugin missing next to host: platforms/qwindows.dll")
    files.extend(sorted((host.parent / "platforms").glob("*.dll")))
    provenance = {}
    for source in files:
        relative = source.relative_to(host.parent)
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        source_hash, target_hash = fingerprint(source), fingerprint(target)
        if source_hash != target_hash:
            raise RuntimeError(f"Runtime copy differs: {relative}")
        provenance[relative.as_posix()] = source_hash
    return directory / host.name, {"directory": str(directory), "files": provenance,
                                   "method": "ordinary_byte_identical_runtime_deployment",
                                   "source_or_system_permissions_changed": False}


def review(host, godot, model, output, replay, profile_path, live=False):
    profile = load_review_profile(profile_path)
    if profile is None:
        raise ValueError("Host review requires a camera/capture profile")
    replay_data = None
    if not live:
        replay_data = json.loads(replay.read_text(encoding="utf8"))
        duration_ms = replay_data.get("duration_ms") if isinstance(replay_data, dict) else None
        if (not isinstance(duration_ms, int) or isinstance(duration_ms, bool)
                or not 1 <= duration_ms <= 150000):
            raise ValueError("Replay requires integral duration_ms within 1..150000")
        if any(item["time"] * 1000 >= duration_ms for item in profile.get("capture_plan", [])):
            raise ValueError("Replay must outlast the entire capture plan")
    version = subprocess.run([str(godot), "--version"], check=True, capture_output=True,
                             text=True, encoding="utf8", timeout=20).stdout.strip()
    project, sources = stage(output, model)
    for name in QT_SOURCE_FILES:
        sources[name] = fingerprint(ROOT / name)
    sources[Path(__file__).relative_to(ROOT).as_posix()] = fingerprint(Path(__file__))
    model_key = model.relative_to(ROOT).as_posix() if model.is_relative_to(ROOT) else str(model)
    for name in sources:
        if name.startswith("godot/toon-prototype/") or name == model_key:
            continue
        source = ROOT / name
        if source.is_file():
            target = output / "source-snapshots" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    write_json(output / "review-profile.json", profile)
    if replay_data is not None:
        write_json(output / "replay.json", replay_data)
    receipt = {"schema_version": 1, "kind": "qt_godot_semantic_host_review",
               "status": "running", "sources": sources,
               "model_source": str(model), "model_fingerprint": fingerprint(model),
               "godot_version": version, "godot_executable": str(godot),
               "godot_fingerprint": fingerprint(godot), "qt_host_binary": str(host),
               "qt_host_fingerprint": fingerprint(host), "output": str(output),
               "review_profile": profile, "profile_fingerprint": fingerprint(output / "review-profile.json"),
               "isolated_replay": not live, "real_sources_enabled": live,
               "character_art_approval": "pending", "live2d_backend_replaced": False}
    if not live:
        receipt["replay_fingerprint"] = fingerprint(output / "replay.json")
    write_json(output / "review-receipt.json", receipt)
    import_project(godot, project, output)
    deployed_host, deployment = package_host(host)
    receipt["runtime_deployment"] = deployment
    captures = output / "captures"
    captures.mkdir()
    command = [str(deployed_host), "--toon-host", "--godot", str(godot),
               "--toon-project", str(project), "--toon-report", str(output / "qt-host-report.json"),
               "--toon-render-output", str(captures),
               "--toon-review-profile", str(output / "review-profile.json")]
    if not live:
        command += ["--toon-replay", str(output / "replay.json"), "--toon-record-frames"]
    startup = None
    if sys.platform == "win32":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    receipt["host_command"] = command  # Token is created inside Qt, never placed here.
    if live:
        process = subprocess.Popen(command, cwd=ROOT, startupinfo=startup, env=launch_environment())
        receipt.update(status="interactive_started", qt_host_pid=process.pid)
        write_json(output / "review-receipt.json", receipt)
        return receipt
    samples = []
    audit = None
    last_audit = 0.0
    with (output / "qt-host.log").open("w", encoding="utf8") as log:
        process = subprocess.Popen(command, cwd=ROOT, startupinfo=startup, env=launch_environment(),
                                   stdout=log, stderr=subprocess.STDOUT)
        launched = time.monotonic()
        deadline = launched + replay_data["duration_ms"] / 1000 + 35
        try:
            while process.poll() is None:
                now = time.monotonic()
                if now - last_audit >= 0.5 and (captures / "runtime-start.json").is_file():
                    try:
                        start = json.loads((captures / "runtime-start.json").read_text(encoding="utf8"))
                        if start.get("window_region_ready"):
                            item = audit_window(start)
                            item["wall_seconds_since_launch"] = now - launched
                            samples.append(item)
                            if audit is None or item.get("status") != "passed":
                                audit = item
                            last_audit = now
                    except (ValueError, OSError):
                        pass
                if now >= deadline:
                    raise TimeoutError("Qt/Godot replay exceeded its deadline")
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.terminate()  # Only our own opt-in replay host.
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    qt_path, runtime_path = output / "qt-host-report.json", captures / "report.json"
    qt = json.loads(qt_path.read_text(encoding="utf8")) if qt_path.exists() else {}
    runtime = json.loads(runtime_path.read_text(encoding="utf8")) if runtime_path.exists() else {}
    runtime_log = qt.get("renderer_output", "")
    (output / "runtime.log").write_text(runtime_log, encoding="utf8")
    errors = re.findall(r"^.*(?:SCRIPT ERROR|ERROR:).*$", runtime_log, re.MULTILINE)
    images = []
    from PIL import Image
    for path in sorted(captures.glob("*.png")):
        with Image.open(path) as image:
            alpha = image.convert("RGBA").getchannel("A").getextrema()
            shutdown = path.name == "shutdown-transparent.png"
            images.append({"file": path.name, "pixels": list(image.size), "alpha_range": list(alpha),
                           "expected_alpha_passed": alpha == ((0, 0) if shutdown else (0, 255)),
                           **fingerprint(path)})
    expected = {item["name"] + ".png" for item in profile.get("capture_plan", [])}
    expected.add("shutdown-transparent.png")
    passed = (process.returncode == 0 and qt.get("status") == "completed"
              and qt.get("ready_seen") is True and qt.get("isolated_replay") is True
              and qt.get("real_sources_enabled") is False and not errors
              and runtime.get("headless") is False and runtime.get("host_mode", {}).get("enabled") is True
              and runtime.get("visual_capture_completed") is True and not runtime.get("errors")
              and runtime.get("shutdown_gpu_frame_presented") is True
              and runtime.get("shutdown_visible_alpha_pixels") == 0
              and {item["file"] for item in images} == expected
              and all(item["expected_alpha_passed"] for item in images)
              and bool(samples) and all(item.get("status") == "passed" for item in samples))
    receipt.update(status="technical_capture_passed" if passed else "failed", exit_code=process.returncode,
                   window_audit=audit, window_audit_samples=samples, errors=errors,
                   images=images, runtime=runtime, qt_status=qt.get("status"),
                   desktop_composite_visual_review="manual_gate_A_passed_on_base_proxy; new_candidate_pending")
    write_json(output / "review-receipt.json", receipt)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=ROOT / "build/DesktopCompanion.exe")
    parser.add_argument("--godot", type=Path, default=GODOT_DEFAULT)
    parser.add_argument("--model", type=Path, default=MODEL_SOURCE)
    parser.add_argument("--replay", type=Path, default=REPLAY)
    parser.add_argument("--review-profile", type=Path, default=PROFILE)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--live", action="store_true", help="Manual real-source mode; close previous pet first")
    args = parser.parse_args()
    output = (args.output or ROOT / ".local/authoring/toon-host" /
              (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])).resolve()
    for path in (args.host, args.godot, args.model, args.review_profile):
        if not path.is_file():
            parser.error(f"Required file missing: {path}")
    try:
        receipt = review(args.host.resolve(), args.godot.resolve(), args.model.resolve(), output,
                         args.replay.resolve(), args.review_profile.resolve(), args.live)
        print(json.dumps({"status": receipt["status"], "output": str(output),
                          "isolated_replay": not args.live}, ensure_ascii=False))
        return 0 if receipt["status"] in ("technical_capture_passed", "interactive_started") else 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        if output.exists():
            path = output / "review-receipt.json"
            receipt = json.loads(path.read_text(encoding="utf8")) if path.exists() else {}
            receipt.update(status="failed", error=str(error))
            write_json(path, receipt)
        print(f"Toon host review failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
