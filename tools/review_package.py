"""Verify a packaged runtime and rehearse it without installing or changing hooks.

Native smoke exits before the production listener and Claude hook seeder start.
Full setup/upgrade/uninstall acceptance still needs a separate Windows account.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import zipfile


MANIFEST = "package-manifest.json"
MODEL = "assets/live2d/whale-girl/whale-girl-layered-draft.model3.json"
REQUIRED = {
    "DesktopCompanion.exe", "DesktopCompanionHook.exe", "Qt6Core.dll",
    "Qt6Gui.dll", "Qt6Network.dll", "Qt6OpenGL.dll", "Qt6OpenGLWidgets.dll",
    "Qt6Svg.dll", "Qt6Widgets.dll", "Live2DCubismCore.dll", "platforms/qwindows.dll",
    "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll",
    "concrt140.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "vccorlib140.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
    "install-hooks.cmd", "uninstall-hooks.cmd", "tools/install-hooks.ps1",
    "tools/seed-cowork-hooks.ps1", "tools/hook-config-common.ps1",
    "assets/live2d/whale-girl/whale-girl-layered-draft.model3.json",
    "assets/live2d/whale-girl/whale-girl-layered-draft.moc3",
    "assets/live2d/whale-girl/whale-girl-layered-draft.psd2live.json",
    "assets/live2d/whale-girl/hit-mask.png", "assets/props/box.png", "assets/props/rice-bowl.png",
    "assets/idle.png", "assets/busy.png", "assets/delete.png", "assets/grass.gif",
}
REQUIRED.update("assets/motions/" + clip + ".motion.json" for clip in (
    "busy-laptop", "busy-rest", "busy-stand", "busy-stretch", "cheer", "delete",
    "grass-touch", "grass", "greet", "head-pat", "idle", "turn-ended"))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError(f"Unsafe manifest path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or str(path) != value:
        raise ValueError(f"Unsafe manifest path: {value!r}")
    if any(ord(char) < 32 for char in value):
        raise ValueError("Control character in manifest path")
    return path


def verify_stage(stage):
    manifest = json.loads((stage / MANIFEST).read_text(encoding="utf8"))
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("files"), list):
        raise ValueError("Unsupported package manifest")
    listed = {}
    case_names = set()
    for entry in manifest["files"]:
        name = str(safe_relative(entry["path"]))
        if name == MANIFEST or name.casefold() in case_names:
            raise ValueError(f"Duplicate/reserved manifest path: {name}")
        case_names.add(name.casefold())
        path = stage / name
        if path.resolve().parent != stage.resolve() and stage.resolve() not in path.resolve().parents:
            raise ValueError(f"Path escapes package: {name}")
        if not path.is_file() or path.stat().st_size != entry["bytes"] or sha256(path) != entry["sha256"]:
            raise ValueError(f"Package bytes disagree with manifest: {name}")
        listed[name] = entry
    actual = {p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file()}
    if actual != set(listed) | {MANIFEST}:
        raise ValueError("Package contains missing or unlisted files")
    missing = REQUIRED - set(listed)
    if missing:
        raise ValueError(f"Required runtime missing from manifest: {sorted(missing)}")
    if "Qt6Test.dll" in listed or any(name.endswith("Test.exe") for name in listed):
        raise ValueError("Test-only binary in package")
    if not any(name.startswith("FrameworkShaders/") for name in listed):
        raise ValueError("Cubism shaders missing")
    # A valid inventory could still describe an incomplete export. Follow the
    # model's actual references and require our runtime-specific motion/prop data.
    refs = json.loads((stage / MODEL).read_text(encoding="utf8"))["FileReferences"]
    if not isinstance(refs.get("Moc"), str) or not refs.get("Textures"):
        raise ValueError("Model needs Moc and texture references")
    referenced = [refs["Moc"], *refs["Textures"]]
    referenced.extend(refs[key] for key in ("Physics", "DisplayInfo", "Pose", "UserData") if key in refs)
    referenced.extend(item["File"] for group in refs.get("Motions", {}).values() for item in group)
    referenced.extend(item["File"] for item in refs.get("Expressions", []))
    for name in referenced:
        path = (PurePosixPath(MODEL).parent / safe_relative(name)).as_posix()
        if path not in listed:
            raise ValueError(f"Model reference missing from package: {path}")
    return manifest, listed


def verify_portable(archive, stage, listed):
    expected = {"DesktopCompanion/" + name: entry for name, entry in listed.items()}
    expected["DesktopCompanion/" + MANIFEST] = {
        "bytes": (stage / MANIFEST).stat().st_size, "sha256": sha256(stage / MANIFEST)}
    with zipfile.ZipFile(archive) as source:
        files = [entry for entry in source.infolist() if not entry.is_dir()]
        if len(files) != len(expected) or {entry.filename for entry in files} != set(expected):
            raise ValueError("Portable archive inventory differs from stage")
        for entry in files:
            info = expected[entry.filename]
            if entry.file_size != info["bytes"]:
                raise ValueError(f"Portable size mismatch: {entry.filename}")
            digest = hashlib.sha256()
            with source.open(entry) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            if digest.hexdigest() != info["sha256"]:
                raise ValueError(f"Portable bytes mismatch: {entry.filename}")


def run(command, output, env, cwd, timeout=60):
    result = subprocess.run([str(arg) for arg in command], cwd=cwd, env=env,
                            capture_output=True, timeout=timeout)
    output.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} failed ({result.returncode}); see {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("output", type=Path, help="Fresh directory outside the stage")
    parser.add_argument("--portable", type=Path)
    parser.add_argument("--setup", type=Path, help="Hash only; never execute the installer")
    parser.add_argument("--lifecycle-test", type=Path,
                        help="HookLifecycleTest.exe to run against this packaged helper")
    args = parser.parse_args()
    stage, output = args.stage.resolve(), args.output.resolve()
    if output == stage or stage in output.parents:
        raise ValueError("Review output must be outside the package")
    if output.exists():
        raise ValueError("Review output already exists; choose a fresh directory")
    manifest, listed = verify_stage(stage)
    if args.portable:
        verify_portable(args.portable.resolve(), stage, listed)
    output.mkdir(parents=True)
    runtime = output / "runtime"
    shutil.copytree(stage, runtime)
    env = os.environ.copy()
    # Resolve every Qt dependency from the copied package, never the developer SDK.
    windows = Path(env.get("SystemRoot", "C:/Windows"))
    env["PATH"] = os.pathsep.join([str(windows / "System32"), str(windows)])
    for name in list(env):
        if name.startswith("QT_"):
            env.pop(name)
    env["TEMP"] = env["TMP"] = str(output)
    smoke = output / "smoke.png"
    run([runtime / "DesktopCompanion.exe", "--render-smoke", smoke],
        output / "native-smoke.log", env, runtime, timeout=45)
    native = json.loads(Path(str(smoke) + ".json").read_text(encoding="utf8"))
    if not native.get("frame_saved") or not smoke.is_file() or \
            native.get("render_backend") != "cubism_native" or native.get("render_error"):
        raise ValueError("Packaged Native smoke did not pass")
    lifecycle = False
    if args.lifecycle_test:
        test = args.lifecycle_test.resolve()
        env["PET_REVIEW_HELPER"] = str(runtime / "DesktopCompanionHook.exe")
        run([test, "-o", str(output / "hook-lifecycle.txt") + ",txt"],
            output / "hook-lifecycle.log", env, runtime)
        lifecycle = True
    # Review runs in a copy: the stage/archive bytes must remain unchanged.
    verify_stage(stage)
    report = {
        "schema_version": 1, "passed": True,
        "package_version": manifest["package_version"], "source": manifest["source"],
        "files": len(listed), "bytes": sum(entry["bytes"] for entry in listed.values()),
        "manifest_sha256": sha256(stage / MANIFEST),
        "portable": {"verified": True, "sha256": sha256(args.portable.resolve())}
                    if args.portable else {"verified": False},
        "setup": {"sha256": sha256(args.setup.resolve()), "executed": False}
                 if args.setup else {"executed": False},
        "native_smoke": native, "qt_sdk_path_removed": True,
        "packaged_helper_lifecycle_test": lifecycle,
        "real_client_hooks_executed": False, "installed_acceptance": False,
        "default_endpoint_used": False, "user_configs_modified": False,
        "moc_sha256": listed["assets/live2d/whale-girl/whale-girl-layered-draft.moc3"]["sha256"],
    }
    (output / "package-check.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf8")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired, zipfile.BadZipFile) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
