"""Validate an isolated Cubism Editor export against its preserved Native baseline.

python tools/review_authoring_roundtrip.py --run .local/authoring/roundtrips/RUN

Empty exports return waiting_input (exit 3) without writing any report. Multiple
models require --model; timestamps are never used to pick a candidate. Every
actual check writes a new reports/editor-check-UTC-random directory. No editor,
production asset, pose file, original author workspace or rig is modified.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import uuid

import compare_cubism_probes as comparison
import probe_cubism_core as probe


class PreflightFailure(ValueError):
    pass


class SelectionRequired(ValueError):
    def __init__(self, candidates):
        super().__init__("Multiple exported models: supply --model explicitly.")
        self.candidates = [str(path) for path in candidates]


def contained(path: Path, directory: Path, label: str) -> Path:
    path = path.resolve()
    if not path.is_relative_to(directory.resolve()):
        raise PreflightFailure(f"{label} resolves outside the Editor export directory: {path}")
    return path


def choose_model(export_directory: Path, explicit_model: Path | None) -> Path | None:
    if explicit_model is not None:
        model = contained(explicit_model, export_directory, "Selected model")
        if not model.name.endswith(".model3.json"):
            raise PreflightFailure("Selected model must be a .model3.json file.")
        return model if model.is_file() else None
    if not export_directory.exists():
        return None
    candidates = sorted(export_directory.rglob("*.model3.json"), key=lambda path: str(path))
    candidates = [contained(path, export_directory, "Model candidate") for path in candidates if path.is_file()]
    if len(candidates) > 1:
        raise SelectionRequired(candidates)
    return candidates[0] if candidates else None


def scoped_model_references(model: Path, directory: Path) -> dict[str, str]:
    """Reject escaped links before model_family hashes or Core reads them."""
    document, model_hash = probe.read_json_snapshot(model)
    refs = document.get("FileReferences") if isinstance(document, dict) else None
    if not isinstance(refs, dict):
        raise PreflightFailure("Editor model requires a FileReferences object.")
    hashes = {str(model): model_hash}

    def include(value, role):
        if not isinstance(value, str) or not value:
            raise PreflightFailure(f"Invalid local model reference: {role}")
        path = contained(model.parent / value, directory, role)
        if not path.is_file():
            raise PreflightFailure(f"Missing exported model reference {role}: {path}")
        hashes[str(path)] = probe.digest(path)

    for key in ("Moc", "Physics", "DisplayInfo", "Pose", "UserData"):
        if key in refs:
            include(refs[key], key)
    if "Moc" not in refs:
        raise PreflightFailure("Editor model requires FileReferences.Moc.")
    textures = refs.get("Textures", [])
    if not isinstance(textures, list):
        raise PreflightFailure("FileReferences.Textures must be a list.")
    for index, value in enumerate(textures):
        include(value, f"Textures[{index}]")

    def nested(value, role):
        if isinstance(value, dict):
            for key, item in value.items():
                if key in ("File", "Sound"):
                    include(item, f"{role}.{key}")
                else:
                    nested(item, f"{role}.{key}")
        elif isinstance(value, list):
            for index, item in enumerate(value):
                nested(item, f"{role}[{index}]")
    for key in ("Motions", "Expressions"):
        if key in refs:
            nested(refs[key], key)
    supported = {"Moc", "Physics", "DisplayInfo", "Pose", "UserData", "Textures", "Motions", "Expressions"}
    if set(refs) - supported:
        raise PreflightFailure(f"Unrecognized FileReferences fields need explicit review: {sorted(set(refs) - supported)}")
    base = model.name.removesuffix(".model3.json")
    for suffix in (".psd2live.json", ".cmo3"):
        sibling = model.with_name(base + suffix)
        if sibling.exists() or sibling.is_symlink():
            path = contained(sibling, directory, "Sibling export metadata")
            if not path.is_file():
                raise PreflightFailure(f"Missing sibling export metadata: {path}")
            hashes[str(path)] = probe.digest(path)
    comparison.verify(hashes)
    return hashes


def new_attempt(run: Path) -> Path:
    reports = (run / "reports").resolve()
    if not reports.is_relative_to(run):
        raise ValueError("Run reports directory resolves outside the selected run.")
    reports.mkdir(parents=True, exist_ok=True)
    label = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = reports / f"editor-check-{label}-{uuid.uuid4().hex[:8]}"
    output.mkdir(exist_ok=False)
    return output


def save_review(directory: Path, report: dict) -> None:
    target = directory / "review.json"
    with target.open("x", encoding="utf8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def review(run: Path, explicit_model: Path | None = None) -> tuple[int, dict]:
    attempt = None
    hashes = {}
    phase = "select_export"
    result = {
        "schema_version": 1, "kind": "isolated_editor_roundtrip_review",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "scope": {"raw_core_only": True, "runtime_patches_applied": False,
                  "physics_evaluated": False, "rendered_pixels_evaluated": False,
                  "editor_export_generated_by_this_tool": False},
    }
    try:
        run = run.resolve(strict=True)
        if not run.is_dir():
            raise ValueError("--run must be an existing roundtrip directory.")
        result["run"] = str(run)
        export_directory = (run / "exports/editor-cmo3-roundtrip").resolve()
        if not export_directory.is_relative_to(run):
            raise ValueError("Editor export directory resolves outside the selected run.")
        result["editor_export_directory"] = str(export_directory)
        model = choose_model(export_directory, explicit_model)
        if model is None:
            return 3, {"status": "waiting_input", "editor_export_directory": str(export_directory),
                       "reason": "No selected Editor .model3.json export exists; no Core or comparison report was created."}
        result["model"] = str(model)
        attempt = new_attempt(run)
        result["report_directory"] = str(attempt)
        baseline = (run / "reports/runtime-before.core.json").resolve()
        if not baseline.is_relative_to(run):
            raise ValueError("Preserved baseline resolves outside the selected run.")
        phase = "verify_baseline"
        before, baseline_hashes, _ = comparison.read_probe(baseline)
        hashes.update(baseline_hashes)
        poses_path = (run / "poses.json").resolve(strict=True)
        if Path(before["pose_input"]["path"]).resolve() != poses_path:
            raise ValueError("Baseline pose input is not the selected run's original poses.json.")
        core_path = Path(before["cubism_core"]["path"]).resolve(strict=True)
        document, poses_hash = probe.read_json_snapshot(poses_path)
        if poses_hash != before["pose_input"]["sha256"]:
            raise ValueError("Original pose input hash differs from the preserved baseline.")
        poses, _ = probe.normalize_poses(document)
        result["baseline"] = {"path": str(baseline), "sha256": probe.digest(baseline)}
        result["original_poses"] = {"path": str(poses_path), "sha256": poses_hash, "pose_count": len(poses)}
        result["original_core"] = before["cubism_core"]
        hashes.update({str(Path(__file__).resolve()): probe.digest(Path(__file__)),
                       str(Path(comparison.__file__).resolve()): probe.digest(Path(comparison.__file__))})
        phase = "export_preflight"
        hashes.update(scoped_model_references(model, export_directory))
        moc, family = probe.model_family(model)
        for row in family:
            path = contained(Path(row["path"]), export_directory, "Model family member")
            if str(path) in hashes and hashes[str(path)] != row["sha256"]:
                raise ValueError("Editor model family changed during preflight.")
            hashes[str(path)] = row["sha256"]
        comparison.verify(hashes)
        try:
            native = probe.NativeModel(core_path, moc)
        except probe.ProbeError as error:
            raise PreflightFailure(f"Native header/MOC consistency preflight failed: {error}") from error
        requested_parameters = set().union(*(set(row["parameters"]) for row in poses))
        requested_parts = set().union(*(set(row["part_opacities"]) for row in poses))
        missing = [{"pose": row["name"],
                    "parameters": sorted(set(row["parameters"]) - set(native.parameter_ids)),
                    "parts": sorted(set(row["part_opacities"]) - set(native.part_ids))}
                   for row in poses if set(row["parameters"]) - set(native.parameter_ids)
                   or set(row["part_opacities"]) - set(native.part_ids)]
        all_drawables = list(native.drawable_ids)
        result["preflight"] = {
            "moc_version": native.moc_version,
            "parameter_ids": list(native.parameter_ids), "part_ids": list(native.part_ids),
            "requested_parameter_ids": sorted(requested_parameters), "requested_part_ids": sorted(requested_parts),
            "missing_requested_bindings": missing,
            "selected_after_drawables": all_drawables,
            "old_drawable_selection_reused": False,
            "vertex_samples": sum(native.vertex_counts) * len(poses),
            "topology_indices": sum(native.index_counts),
            "limits": {"vertex_samples": probe.MAX_VERTEX_SAMPLES,
                       "topology_indices": probe.MAX_TOPOLOGY_INDICES},
        }
        if missing:
            raise PreflightFailure("Export is missing requested parameter axes or parts; original pose requests were retained.")
        if not all_drawables:
            raise PreflightFailure("Editor export has no Native drawable IDs.")
        if (result["preflight"]["vertex_samples"] > probe.MAX_VERTEX_SAMPLES
                or result["preflight"]["topology_indices"] > probe.MAX_TOPOLOGY_INDICES):
            result.update(status="incomplete", reason="Full after-model geometry exceeds the preserved report budget; no IDs or poses were dropped.",
                          raw_core_report_created=False, comparison_report_created=False)
            code = 1
        else:
            del native
            phase = "probe_export"
            core_report_path = attempt / "editor-after.core.json"
            probe.run_probe(model, poses_path, core_report_path, core_path=core_path, drawables=all_drawables)
            result["raw_core_report"] = str(core_report_path)
            phase = "compare_exports"
            comparison_path = attempt / "comparison.json"
            compared = comparison.compare(baseline, core_report_path, comparison_path)
            result.update(status=compared["status"], comparison_report=str(comparison_path), summary=compared["summary"],
                          raw_core_report_created=True, comparison_report_created=True)
            code = 0 if result["status"] == "equivalent_within_tolerance" else 1
    except SelectionRequired as error:
        result.update(status="selection_required", reason=str(error), candidates=error.candidates,
                      raw_core_report_created=False, comparison_report_created=False)
        code = 2
    except PreflightFailure as error:
        result.update(status="failed", phase=phase, reason=str(error),
                      raw_core_report_created=False, comparison_report_created=False)
        code = 1
    except (comparison.ComparisonError, probe.ProbeError, OSError, ValueError, KeyError, TypeError) as error:
        budget = isinstance(error, probe.ProbeError) and ("budget" in str(error).lower() or "exceeds 64 MiB" in str(error))
        result.update(status="incomplete" if budget else "tool_error", phase=phase, reason=str(error))
        code = 1 if budget else 2
    if hashes:
        try:
            comparison.verify(hashes)
            result.update(input_hashes_verified_before_and_after=True, protected_inputs=hashes)
        except (comparison.ComparisonError, OSError) as error:
            result.update(status="tool_error", reason=str(error), input_hashes_verified_before_and_after=False)
            code = 2
    if attempt is None and "run" in result:
        try:
            attempt = new_attempt(Path(result["run"]))
            result["report_directory"] = str(attempt)
        except (OSError, ValueError) as error:
            result.update(status="tool_error", reason=f"Cannot create isolated review output: {error}")
            return 2, result
    if attempt is not None:
        try:
            result["review_report"] = str(attempt / "review.json")
            save_review(attempt, result)
        except (OSError, ValueError) as error:
            result.update(status="tool_error", reason=f"Cannot write review receipt: {error}")
            return 2, result
    return code, result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--model", type=Path)
    args = parser.parse_args(argv)
    code, result = review(args.run, args.model)
    fields = ("status", "reason", "report_directory", "review_report", "editor_export_directory", "candidates", "summary")
    print(json.dumps({key: result[key] for key in fields if key in result}, ensure_ascii=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
