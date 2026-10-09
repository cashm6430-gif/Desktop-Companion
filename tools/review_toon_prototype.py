"""Stage and review the isolated 3D prototype without changing the Qt pet.

Runs the real graphical renderer for images. The Win32 audit observes our
window and hit region only; desktop compositing and real clicks remain a
separate manual check.
"""

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]
GODOT_DEFAULT = Path("D:/tool/godot/Godot_v4.7.2-stable_win64.exe")
PROJECT_SOURCE = ROOT / "godot/toon-prototype"
MODEL_SOURCE = ROOT / "assets/3d/prototype/character.glb"
SOURCE_EXCLUDES = (".godot", "*.tmp", "*.obj", "*.pdb", ".sconsign.dblite")


def fingerprint(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return {"bytes": path.stat().st_size, "sha256": sha.hexdigest()}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf8")


def load_review_profile(path):
    if path is None:
        return None
    data = json.loads(path.read_text(encoding="utf8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError("Review profile requires schema_version: 1")
    camera = data.get("camera", {})
    if not isinstance(camera, dict):
        raise ValueError("Camera profile must be an object")
    vectors = []
    for name in ("position", "target"):
        value = camera.get(name)
        if value is not None:
            if (not isinstance(value, list) or len(value) != 3 or any(
                    not isinstance(item, (int, float)) or isinstance(item, bool)
                    or not math.isfinite(item) for item in value)):
                raise ValueError(f"Camera {name} must be a finite 3-vector")
            vectors.append(value)
    if len(vectors) == 2 and math.dist(*vectors) < 0.01:
        raise ValueError("Camera position and target must differ")
    size = camera.get("size", 2.3)
    if not isinstance(size, (int, float)) or isinstance(size, bool) or not math.isfinite(size) or not 0.3 <= size <= 10:
        raise ValueError("Camera size must be within 0.3..10")
    window = data.get("window_size", [360, 420])
    if (not isinstance(window, list) or len(window) != 2 or any(
            not isinstance(item, int) or isinstance(item, bool) or not 240 <= item <= 1920
            for item in window)):
        raise ValueError("window_size must contain two integers within 240..1920")
    if not isinstance(data.get("hide_proxy_body", True), bool):
        raise ValueError("hide_proxy_body must be boolean")
    expression_probe = data.get("expression_probe", {})
    if not isinstance(expression_probe, dict) or set(expression_probe) - {"blink_peak", "smile_peak"}:
        raise ValueError("expression_probe supports blink_peak and smile_peak only")
    for value in expression_probe.values():
        if (not isinstance(value, (int, float)) or isinstance(value, bool)
                or not math.isfinite(value) or not 0 <= value <= 1):
            raise ValueError("Expression probe values must be within 0..1")
    yaw = data.get("model_yaw_degrees", 0)
    if not isinstance(yaw, (int, float)) or isinstance(yaw, bool) or not math.isfinite(yaw):
        raise ValueError("model_yaw_degrees must be finite")
    plan = data.get("capture_plan")
    if plan is not None:
        if not isinstance(plan, list) or not 1 <= len(plan) <= 64:
            raise ValueError("capture_plan requires 1..64 entries")
        last_time, names = -1.0, set()
        for item in plan:
            at = item.get("time") if isinstance(item, dict) else None
            name = item.get("name", "") if isinstance(item, dict) else ""
            if (not isinstance(at, (int, float)) or isinstance(at, bool)
                    or not math.isfinite(at) or not 0 <= at <= 150 or at <= last_time
                    or not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9-]{0,47}", name)
                    or name in names or name == "shutdown-transparent"):
                raise ValueError("capture_plan needs ordered finite times and unique safe names")
            last_time = at
            names.add(name)
    return data


def stage(output, model_source=MODEL_SOURCE):
    if output.exists():
        raise ValueError(f"Review output already exists; use a fresh directory: {output}")
    if not (PROJECT_SOURCE / "project.godot").is_file() or not model_source.is_file():
        raise FileNotFoundError("Prototype project/model is not ready yet")
    output.mkdir(parents=True)
    project = output / "project"
    shutil.copytree(PROJECT_SOURCE, project,
                    ignore=shutil.ignore_patterns(*SOURCE_EXCLUDES))
    (project / "assets").mkdir(exist_ok=True)
    shutil.copy2(model_source, project / "assets/character.glb")
    # Park the FocusGuard GDExtension manifest before the import pass: the
    # first editor scan (cold import) crashes at shutdown when any extension
    # class is registered (upstream godot-cpp#2024 / godot#1900, doc-cache
    # thread vs extension teardown race). The runtime still needs it, so
    # run_review restores the file after importing.
    parked = project / "focus-guard.gdextension"
    if parked.is_file():
        shutil.move(str(parked), str(output / "focus-guard.gdextension.parked"))
    sources = {path.relative_to(ROOT).as_posix(): fingerprint(path)
               for path in sorted(PROJECT_SOURCE.rglob("*"))
               if path.is_file() and ".godot" not in path.parts
               and not any(path.match(pattern) for pattern in SOURCE_EXCLUDES)}
    model_key = model_source.relative_to(ROOT).as_posix() if model_source.is_relative_to(ROOT) else str(model_source)
    sources[model_key] = fingerprint(model_source)
    sources[Path(__file__).relative_to(ROOT).as_posix()] = fingerprint(Path(__file__))
    return project, sources


def audit_window(start):
    if sys.platform != "win32":
        return {"status": "unavailable", "reason": "Win32 only"}
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    pid = int(start.get("pid", 0))
    handle = int(start.get("native_handle", 0))
    user32.IsWindow.argtypes = [wintypes.HWND]
    user32.IsWindow.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    if not handle or not user32.IsWindow(handle):
        candidates = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        @callback_type
        def collect(hwnd, unused):
            actual_pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(actual_pid))
            if actual_pid.value == pid:
                candidates.append(hwnd)
            return True

        user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        user32.EnumWindows.restype = wintypes.BOOL
        user32.EnumWindows(collect, 0)
        if not candidates:
            return {"status": "failed", "reason": "Own runtime window not found", "pid": pid}
        handle = candidates[0]
    actual_pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(handle, ctypes.byref(actual_pid))
    if actual_pid.value != pid:
        return {"status": "failed", "reason": "HWND belongs to a different process"}
    get_style = getattr(user32, "GetWindowLongPtrW", user32.GetWindowLongW)
    get_style.argtypes = [wintypes.HWND, ctypes.c_int]
    get_style.restype = ctypes.c_ssize_t
    style = get_style(handle, -16) & 0xFFFFFFFF
    extended = get_style(handle, -20) & 0xFFFFFFFF
    rect = wintypes.RECT()
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.GetWindowRect.restype = wintypes.BOOL
    if not user32.GetWindowRect(handle, ctypes.byref(rect)):
        return {"status": "failed", "reason": "GetWindowRect failed", "native_handle": handle}
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.WindowFromPoint.argtypes = [wintypes.POINT]
    user32.WindowFromPoint.restype = wintypes.HWND
    point = wintypes.POINT(rect.left + 2, rect.top + 2)
    corner = user32.WindowFromPoint(point)
    foreground = user32.GetForegroundWindow()
    foreground_pid = wintypes.DWORD()
    if foreground:
        user32.GetWindowThreadProcessId(foreground, ctypes.byref(foreground_pid))
    facts = {
        "pid": pid, "native_handle": handle, "visible": bool(user32.IsWindowVisible(handle)),
        "rect_physical_pixels": [rect.left, rect.top, rect.right, rect.bottom],
        "style_hex": hex(style), "exstyle_hex": hex(extended),
        "caption_absent": not bool(style & 0x00C00000),
        "topmost": bool(extended & 0x8), "no_activate": bool(extended & 0x08000000),
        "owns_foreground": foreground == handle,
        "foreground_handle": foreground, "foreground_pid": foreground_pid.value,
        "outside_corner_hits_own_window": corner == handle,
        "desktop_composite_visual_review": "pending",
        "actual_drag_and_click_review": "pending",
        "note": "WindowFromPoint is read-only; no clicks, screenshots of the desktop, or UI automation.",
    }
    facts["status"] = "passed" if (
        facts["visible"] and facts["caption_absent"] and facts["topmost"]
        and facts["no_activate"] and not facts["owns_foreground"]
        and not facts["outside_corner_hits_own_window"]
    ) else "failed"
    return facts


def import_project(godot, project, output):
    imports = [str(godot), "--headless", "--editor", "--import", "--path", str(project)]
    imported = subprocess.run(imports, capture_output=True, text=True,
                              encoding="utf8", errors="replace", timeout=120)
    import_log = imported.stdout + imported.stderr
    (output / "import.log").write_text(import_log, encoding="utf8")
    if imported.returncode or re.search(r"(?:SCRIPT ERROR|^ERROR:)", import_log, re.MULTILINE):
        raise RuntimeError("Godot import failed; see import.log")
    # Restore the FocusGuard extension manifest before the runtime pass. The
    # runtime discovers extensions via .godot/extension_list.cfg, which the
    # import pass only generates when a .gdextension is present, so seed it.
    parked = output / "focus-guard.gdextension.parked"
    if parked.is_file():
        shutil.move(str(parked), str(project / "focus-guard.gdextension"))
        ext_cfg = project / ".godot/extension_list.cfg"
        entry = "res://focus-guard.gdextension\n"
        existing = ext_cfg.read_text(encoding="utf8") if ext_cfg.is_file() else ""
        if entry not in existing:
            ext_cfg.write_text(existing + entry, encoding="utf8")


def run_review(godot, project, output, duration, interactive=False, record_frames=False,
               review_kind="demo", profile=None):
    import_project(godot, project, output)
    capture = output / "captures"
    capture.mkdir()
    startup = None
    if sys.platform == "win32":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        # Suppress the first show while Godot sets its NO_FOCUS flag. Its later
        # native show uses SW_SHOWNA; no other application's focus is changed.
        startup.wShowWindow = 0  # SW_HIDE for the first ShowWindow only.
    command = [str(godot), "--path", str(project)]
    user_arguments = []
    if review_kind != "demo":
        user_arguments += ["--review-kind", review_kind]
    if profile is not None:
        user_arguments += ["--review-profile", str(profile)]
    if not interactive:
        user_arguments += ["--review-output", str(capture), "--quit-after", str(duration)]
        if record_frames:
            user_arguments += ["--record-frames"]
    if user_arguments:
        command += ["--", *user_arguments]
    if interactive:
        subprocess.Popen(command, cwd=ROOT, startupinfo=startup)
        return {"status": "interactive_started", "command": command,
                "captures": "not requested", "manual_review": "pending"}
    audit = None
    audit_samples = []
    last_audit_time = 0.0
    foreground_before = None
    if sys.platform == "win32":
        user32 = ctypes.WinDLL("user32")
        user32.GetForegroundWindow.restype = wintypes.HWND
        foreground_before = user32.GetForegroundWindow()
    with (output / "runtime.log").open("w", encoding="utf8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT,
                                   startupinfo=startup)
        launched = time.monotonic()
        deadline = launched + duration + 40
        try:
            while process.poll() is None:
                if time.monotonic() - last_audit_time >= 0.5 and (capture / "runtime-start.json").is_file():
                    try:
                        start = json.loads((capture / "runtime-start.json").read_text(encoding="utf8"))
                        if start.get("window_region_ready", False):
                            sample = audit_window(start)
                            sample["wall_seconds_since_launch"] = time.monotonic() - launched
                            audit_samples.append(sample)
                            if audit is None or sample.get("status") != "passed":
                                audit = sample
                            last_audit_time = time.monotonic()
                    except (ValueError, OSError):
                        pass
                if time.monotonic() >= deadline:
                    raise TimeoutError("Graphical review exceeded its deadline")
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    runtime_log = (output / "runtime.log").read_text(encoding="utf8")
    fatal_errors = re.findall(r"^.*(?:SCRIPT ERROR|ERROR:).*$", runtime_log, re.MULTILINE)
    report_path = capture / "report.json"
    runtime = json.loads(report_path.read_text(encoding="utf8")) if report_path.exists() else {}
    images = sorted(capture.glob("*.png"))
    image_checks = []
    from PIL import Image
    for path in images:
        with Image.open(path) as image:
            rgba = image.convert("RGBA")
            alpha = rgba.getchannel("A")
            extremes = alpha.getextrema()
            shutdown_frame = path.name == "shutdown-transparent.png"
            image_checks.append({"file": path.name, "pixels": list(image.size),
                                 "role": "shutdown" if shutdown_frame else "key_pose",
                                 "alpha_range": list(extremes),
                                 "has_transparent_background_and_visible_model": extremes == (0, 255),
                                 "expected_alpha_passed": extremes == ((0, 0) if shutdown_frame else (0, 255)),
                                 **fingerprint(path)})
    passed = bool(runtime) and process.returncode == 0 and not fatal_errors and bool(image_checks)
    passed = passed and runtime.get("visual_capture_completed", False) and not runtime.get("errors", [])
    passed = passed and all(item["expected_alpha_passed"] for item in image_checks)
    passed = passed and runtime.get("shutdown_gpu_frame_presented", False)
    passed = passed and runtime.get("shutdown_visible_alpha_pixels", -1) == 0
    passed = passed and audit is not None and audit.get("status") == "passed"
    return {"status": "technical_capture_passed" if passed else "failed",
            "exit_code": process.returncode, "command": command, "errors": fatal_errors,
            "window_audit": audit, "runtime": runtime, "images": image_checks,
            "window_audit_samples": audit_samples, "foreground_before_launch": foreground_before,
            "windows_startup_show_mode": "SW_HIDE_then_Godot_NO_FOCUS_show" if startup else "platform_default",
            "character_art_approval": "pending" if review_kind == "head-style" else "not_requested_technical_placeholder",
            "desktop_composite_visual_review": "pending"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, default=Path(os.environ.get(
        "DESKTOP_COMPANION_GODOT", str(GODOT_DEFAULT))))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--model", type=Path, default=MODEL_SOURCE,
                        help="Explicit candidate GLB; does not replace the canonical asset")
    parser.add_argument("--contract", type=Path, help="Freeze the matching author rig contract with this review")
    parser.add_argument("--duration", type=float, default=17.0)
    parser.add_argument("--interactive", action="store_true")
    parser.add_argument("--record-frames", action="store_true",
                        help="Record true GPU sequence frames for continuity review")
    parser.add_argument("--review-kind", choices=("demo", "head-style"), default="demo")
    parser.add_argument("--review-profile", type=Path)
    args = parser.parse_args()
    if not (7 if args.review_kind == "head-style" else 12) <= args.duration <= 60:
        parser.error("duration must be 7..60 seconds for head-style or 12..60 for demo")
    output = (args.output or ROOT / ".local/authoring/toon-prototype" /
              (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])).resolve()
    godot = args.godot.resolve()
    if not godot.is_file():
        parser.error(f"Godot executable not found: {godot}")
    try:
        profile_data = load_review_profile(args.review_profile)
        version = subprocess.run([str(godot), "--version"], check=True, capture_output=True,
                                 text=True, encoding="utf8", timeout=20).stdout.strip()
        model_source = args.model.resolve()
        contract = args.contract.resolve() if args.contract is not None else ROOT / "art/3d" / model_source.parent.name / "rig-contract.json"
        contract_data = None
        if contract.is_file():
            contract_data = json.loads(contract.read_text(encoding="utf8"))
            if contract_data.get("glb_sha256") != fingerprint(model_source)["sha256"]:
                raise ValueError("Author contract does not match the selected model")
        elif args.contract is not None:
            raise FileNotFoundError(f"Rig contract missing: {contract}")
        project, sources = stage(output, model_source)
        frozen_contract = None
        if contract_data is not None:
            shutil.copy2(contract, output / "rig-contract.json")
            frozen_contract = fingerprint(output / "rig-contract.json")
            for name in ("rebuild-recipe.json", "source-and-export-audit.json"):
                source = contract.parent / name
                if source.is_file():
                    shutil.copy2(source, output / name)
        profile = None
        if profile_data is not None:
            profile = output / "review-profile.json"
            write_json(profile, profile_data)
        receipt = {"schema_version": 1, "status": "running", "kind": "toon_3d_technical_prototype",
                   "godot_version": version, "godot_executable": str(godot),
                   "godot_fingerprint": fingerprint(godot), "sources": sources,
                   "model_source": str(model_source), "model_fingerprint": fingerprint(model_source),
                   "frozen_model_contract": frozen_contract,
                   "review_kind": args.review_kind, "review_profile": profile_data,
                   "output": str(output), "live2d_backend_replaced": False}
        write_json(output / "review-receipt.json", receipt)
        result = run_review(godot, project, output, args.duration, args.interactive,
                            args.record_frames, args.review_kind, profile)
        receipt.update(result)
        write_json(output / "review-receipt.json", receipt)
        print(json.dumps({"status": receipt["status"], "output": str(output),
                          "images": len(receipt.get("images", []))}, ensure_ascii=False))
        return 0 if receipt["status"] in ("technical_capture_passed", "interactive_started") else 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        if output.exists():
            path = output / "review-receipt.json"
            receipt = json.loads(path.read_text(encoding="utf8")) if path.exists() else {}
            receipt.update(status="failed", error=str(error))
            write_json(path, receipt)
        print(f"Prototype review failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
