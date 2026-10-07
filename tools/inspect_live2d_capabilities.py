"""Export conservative, model-bound Live2D capabilities for text-only handoffs.

Native ranges prove limits, not visible motion. Policy describes authoring intent;
only a hashed probe against the complete current model can establish a measured
effect. This tool never approves a motion or edits the model.
"""
import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
from uuid import uuid4

from review_session import deployment_snapshot


ROOT = Path(__file__).resolve().parents[1]
MODEL_REL = Path("assets/live2d/whale-girl")
MODEL_NAME = "whale-girl-layered-draft.model3.json"
POLICY_REL = Path("art/live2d/workflow/parameter-policy.json")


def sha256(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def relative_name(root, path):
    try:
        return Path(path).resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(Path(path).resolve())


def model_snapshot(root):
    directory = root / MODEL_REL
    records = {relative_name(root, path): sha256(path)
               for path in sorted(directory.rglob("*")) if path.is_file()}
    if not records:
        raise ValueError("CAP_MODEL_MISSING: no model files found")
    return records


def renderer_snapshot(root):
    return {relative_name(root, root / "build" / name): sha256(root / "build" / name)
            for name in ("DesktopCompanion.exe", "Live2DCubismCore.dll")}


def capture_ranges(root, ranges_path):
    """Capture ranges with explicit input binding, without overwriting on failure."""
    # The shared deployment gate also catches stale extra motion files.
    deployed_before = deployment_snapshot()
    model_before = model_snapshot(root)
    renderer_before = renderer_snapshot(root)
    ranges_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = ranges_path.with_name(f".{ranges_path.name}.{uuid4().hex}.native.json")
    options = {}
    if sys.platform == "win32":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        options["startupinfo"] = startup
    try:
        subprocess.run([str(root / "build/DesktopCompanion.exe"), "--dump-parameters", str(temporary)],
                       cwd=root / "build", check=True, timeout=120, **options)
        data = read_json(temporary)
        parse_ranges(data)
        if deployment_snapshot() != deployed_before or model_snapshot(root) != model_before:
            raise ValueError("CAP_CAPTURE_INPUT_CHANGED: assets changed during Native capture")
        if renderer_snapshot(root) != renderer_before:
            raise ValueError("CAP_CAPTURE_RENDERER_CHANGED: renderer/Core changed during Native capture")
        settings = read_json(root / MODEL_REL / MODEL_NAME)
        moc_name = relative_name(root, root / MODEL_REL / settings["FileReferences"]["Moc"])
        data.update(model_moc_sha256=model_before[moc_name], model_inputs=model_before,
                    renderer_sha256=renderer_before["build/DesktopCompanion.exe"],
                    cubism_core_sha256=renderer_before["build/Live2DCubismCore.dll"],
                    capture_provenance={"schema_version": 1,
                                        "kind": "native_parameter_ranges",
                                        "captured_utc": datetime.now(timezone.utc).isoformat(),
                                        "source_and_deployment_match": True,
                                        "inputs_unchanged_during_capture": True,
                                        "command": "DesktopCompanion.exe --dump-parameters",
                                        "scope": "range_limits_only; no geometry or visual approval"})
        temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
        temporary.replace(ranges_path)
    finally:
        if temporary.is_file():
            temporary.unlink()
    return data


def parse_ranges(data):
    if data.get("render_backend") != "cubism_native":
        raise ValueError("CAP_RANGE_BACKEND: ranges must come from cubism_native")
    parameters = data.get("parameters")
    if not isinstance(parameters, dict) or not parameters:
        raise ValueError("CAP_RANGE_EMPTY: Native parameter ranges are missing")
    if data.get("parameter_count", len(parameters)) != len(parameters):
        raise ValueError("CAP_RANGE_COUNT: parameter_count does not match parameters")
    for identifier, item in parameters.items():
        if not isinstance(item, dict):
            raise ValueError(f"CAP_RANGE_INVALID: {identifier}")
        values = [item.get(key) for key in ("min", "max", "default")]
        if any(isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value)
               for value in values) or not values[0] <= values[2] <= values[1]:
            raise ValueError(f"CAP_RANGE_INVALID: {identifier} needs finite min <= default <= max")
    return parameters


def validator_rules(root):
    """Read current literal policy sets without executing the asset validator."""
    path = root / "tools/validate_live2d_assets.py"
    rules = {"INERT_PARAMETERS": set(), "GROUNDED_PARAMETERS": set()}
    if path.is_file():
        tree = ast.parse(path.read_text(encoding="utf8"))
        for statement in tree.body:
            if isinstance(statement, ast.Assign):
                for target in statement.targets:
                    if isinstance(target, ast.Name) and target.id in rules:
                        rules[target.id] = set(ast.literal_eval(statement.value))
    return rules


def reaction_ownership(root):
    """Extract the two explicit short-reaction lists, including C++ ID aliases."""
    path = root / "src/ParameterMotion.cpp"
    if not path.is_file():
        return {"status": "source_missing", "source": relative_name(root, path),
                "source_sha256": None, "allowed": [], "reserved": {}}
    source = path.read_text(encoding="utf8")
    # The canonical parameter-id aliases live in the shared motion header since
    # the behavior-module refactor; the coordinator file keeps using them.
    aliases = dict(re.findall(r'const QString (\w+)\s*=\s*QStringLiteral\("([^"]+)"\)', source))
    ids_header = root / "src/motion/MotionParameterIds.h"
    if ids_header.is_file():
        aliases.update(re.findall(
            r'inline const QString (\w+)\s*=\s*QStringLiteral\("([^"]+)"\)',
            ids_header.read_text(encoding="utf8")))
    allowed_block = re.search(r'reactionParameters\(\)\s*\{(.*?)return ids;', source, re.S)
    reserved_block = re.search(r'reactionReservedParameters\(\)\s*\{(.*?)return reserved;', source, re.S)
    if not allowed_block or not reserved_block:
        return {"status": "unparsed", "source": relative_name(root, path),
                "source_sha256": sha256(path), "allowed": [], "reserved": {}}
    body = allowed_block.group(1)
    allowed = set(re.findall(r'QStringLiteral\("(Param[^\"]+)"\)', body))
    allowed.update(aliases[name] for name in re.findall(r'\b\w+\b', body) if name in aliases)
    reserved = {}
    for literal, alias, reason in re.findall(
            r'\{\s*(?:QStringLiteral\("([^"]+)"\)|(\w+))\s*,\s*QStringLiteral\("([^"]+)"\)\s*\}',
            reserved_block.group(1)):
        identifier = literal or aliases.get(alias)
        if identifier:
            reserved[identifier] = reason
    return {"status": "extracted_explicit_lists", "source": relative_name(root, path),
            "source_sha256": sha256(path), "allowed": sorted(allowed), "reserved": reserved,
            "scope": "current short-reaction ownership only; not visible-effect evidence"}


def motion_references(root):
    references = {}
    motions = []
    for path in sorted((root / "assets/motions").glob("*.motion.json")):
        data = read_json(path)
        used = {key for frame in data.get("keyframes", []) for key in frame.get("parameters", {})}
        for field in ("tracks", "constants", "channels", "pulses"):
            used.update(data.get(field, {}))
        record = {"id": path.name.removesuffix(".motion.json"), "path": relative_name(root, path),
                  "sha256": sha256(path), "approval": data.get("approval", "unknown"),
                  "revision": data.get("revision"), "mode": data.get("mode", "absolute"),
                  "usage_scope": "authored_reference; not proof of visible parameter effect"}
        motions.append(record)
        for identifier in used:
            references.setdefault(identifier, []).append(record)
    return references, motions


def resolve_evidence(root, evidence, inputs):
    """An old result cannot silently become current just because an ID persists."""
    result = dict(evidence)
    sources = evidence.get("sources", [])
    checks = []
    for source in sources:
        path = root / source["path"]
        actual = sha256(path) if path.is_file() else None
        checks.append({**source, "current_sha256": actual,
                       "matches_recorded_source": actual is not None and actual == source.get("sha256")})
    expected = evidence.get("model_inputs")
    expected_runtime = dict(evidence.get("runtime_inputs", {}))
    renderer_hash = evidence.get("renderer_sha256")
    core_hash = evidence.get("cubism_core_sha256")
    if renderer_hash:
        expected_runtime["build/DesktopCompanion.exe"] = renderer_hash
    if core_hash:
        expected_runtime["build/Live2DCubismCore.dll"] = core_hash
    runtime_checks = [{"path": name, "expected_sha256": expected_hash,
                       "current_sha256": sha256(root / name) if (root / name).is_file() else None}
                      for name, expected_hash in expected_runtime.items()]
    result["source_checks"] = checks
    result["runtime_checks"] = runtime_checks
    if not expected:
        result["binding_status"] = "historical_unbound"
    elif expected != inputs:
        result["binding_status"] = "stale_model"
    elif not checks or not all(item["matches_recorded_source"] for item in checks):
        result["binding_status"] = "missing_or_changed_evidence"
    elif evidence.get("kind") == "native_parameter_probe" and not (renderer_hash and core_hash):
        result["binding_status"] = "historical_unbound"
    elif evidence.get("kind") == "native_parameter_probe" and not all(
            item["current_sha256"] == item["expected_sha256"] for item in runtime_checks):
        result["binding_status"] = "stale_runtime"
    else:
        result["binding_status"] = "bound_to_current_model"
    result["eligible_as_current_measurement"] = (
        result["binding_status"] == "bound_to_current_model"
        and evidence.get("kind") == "native_parameter_probe")
    return result


def inspect_capabilities(root, ranges_path, policy_path):
    inputs = model_snapshot(root)
    settings = read_json(root / MODEL_REL / MODEL_NAME)
    refs = settings["FileReferences"]
    display = read_json(root / MODEL_REL / refs["DisplayInfo"])
    metadata_path = root / MODEL_REL / "whale-girl-layered-draft.psd2live.json"
    metadata = read_json(metadata_path) if metadata_path.is_file() else {}
    policy = read_json(policy_path)
    data = read_json(ranges_path)
    native = parse_ranges(data)
    moc_name = relative_name(root, root / MODEL_REL / refs["Moc"])
    range_bound = (data.get("model_inputs") == inputs
                   and data.get("model_moc_sha256") == inputs.get(moc_name)
                   and data.get("capture_provenance", {}).get("kind") == "native_parameter_ranges")
    range_status = "bound_native_ranges" if range_bound else "unverified_model_binding"
    names = {item["Id"]: item for item in display.get("Parameters", [])}
    references, motions = motion_references(root)
    rules = validator_rules(root)
    reaction = reaction_ownership(root)
    evidence = {key: resolve_evidence(root, value, inputs)
                for key, value in policy.get("evidence", {}).items()}
    combined = display.get("CombinedParameters", [])
    results = {}
    for identifier in sorted(set(native) | set(names) | set(references) | set(policy.get("parameters", {}))):
        spec = policy.get("parameters", {}).get(identifier, {})
        channel = spec.get("recommended_channel", "unknown")
        channel_policy = policy.get("channels", {}).get(channel, {})
        selected = [evidence[key] for key in spec.get("evidence", []) if key in evidence]
        current = [item for item in selected if item["eligible_as_current_measurement"]
                   and identifier in native and range_bound]
        blocked = list(spec.get("prohibited_reasons", []))
        if identifier in rules["INERT_PARAMETERS"]:
            blocked.append("Asset validator forbids this declared inert axis; name/range does not establish visible effect.")
        if identifier in rules["GROUNDED_PARAMETERS"]:
            blocked.append("Asset validator forbids vertical whole-model travel under the current grounded renderer.")
        present = identifier in native
        if reaction["status"] != "extracted_explicit_lists":
            reaction_status = "unknown"
        else:
            reaction_status = "allowed" if identifier in reaction["allowed"] else "reserved" if identifier in reaction["reserved"] else "not_allowed"
        status = "blocked" if blocked else "not_in_native_dump" if not present else "measured_in_probe" if current else "unmeasured"
        bindings = [{key: layer.get(key) for key in ("source", "drawable", "type", "switchId")}
                    for layer in metadata.get("layers", []) if layer.get("parameter") == identifier]
        results[identifier] = {
            "name": names.get(identifier, {}).get("Name", identifier),
            "group": names.get(identifier, {}).get("GroupId", "unknown"),
            "purpose": spec.get("purpose", "unknown; obtain an axis probe before authoring"),
            "native_present": present, "native_range": native.get(identifier),
            "range_binding_status": range_status if present else "not_in_dump",
            "range_scope": "limits only; does not prove visible motion, contact or naturalness",
            "measured_status": "measured" if current else "unmeasured",
            "authoring_status": status,
            "measured_scopes": [item.get("scope", "unknown") for item in current],
            "evidence": selected,
            "recommended_channel": channel,
            "current_writers": spec.get("current_writers", channel_policy.get("current_writers", [])),
            "writer_scope": "documented code responsibilities; not automatically traced execution",
            "reaction_ownership": reaction_status,
            "reaction_reserved_reason": reaction["reserved"].get(identifier),
            "reaction_ownership_source": {"path": reaction["source"], "sha256": reaction["source_sha256"]},
            "runtime_input_domain": spec.get("runtime_input_domain", {
                "status": "not_explicitly_documented", "do_not_infer_from_native_range": True}),
            "resolution_contract": spec.get("resolution_contract", {"kind": "not_explicitly_documented"}),
            "posture_constraints": channel_policy.get("posture_constraints", []) + spec.get("posture_constraints", []),
            "prohibited_reasons": sorted(set(blocked)),
            "declared_layer_bindings": bindings,
            "declared_combined_parameters": [pair for pair in combined if identifier in pair],
            "allowed_combinations": {"status": "unmeasured", "reason": "CDI pairs and shared clips do not prove arbitrary safe combinations"},
            "motion_references": references.get(identifier, []),
        }
    if model_snapshot(root) != inputs:
        raise ValueError("CAP_INSPECT_INPUT_CHANGED: model changed while inspecting")
    validator_path = root / "tools/validate_live2d_assets.py"
    return {"schema_version": 1, "scope": "parameter_limits_and_conservative_authoring_policy",
            "model_inputs": inputs, "model_moc_sha256": inputs.get(moc_name),
            "policy": {"path": relative_name(root, policy_path), "sha256": sha256(policy_path)},
            "validator_sha256": sha256(validator_path) if validator_path.is_file() else None,
            "reaction_policy": reaction,
            "runtime_contract_sources": {name: sha256(root / name)
                                         for name in ("src/ParameterMotion.cpp", "src/CubismCanvas.cpp", "src/CubismPostureTransition.h")
                                         if (root / name).is_file()},
            "range_evidence": {"path": relative_name(root, ranges_path), "sha256": sha256(ranges_path),
                               "binding_status": range_status,
                               "capture_provenance": data.get("capture_provenance"),
                               "renderer_sha256": data.get("renderer_sha256"),
                               "cubism_core_sha256": data.get("cubism_core_sha256")},
            "parameters": results, "motions": motions,
            "limitations": policy.get("limitations", []) + [
                "No parameter is made usable by its existence, name, declared layer, range, or an approved motion alone.",
                "No new visual or user adoption approval is issued by this report.",
                "Probe observations are valid only for the listed scope and exact bound model inputs."]}


def text_summary(report):
    lines = [f"Native ranges: {report['range_evidence']['binding_status']}",
             "Range != visible capability; combinations remain unmeasured unless separately proved."]
    for identifier, item in report["parameters"].items():
        limits = item["native_range"]
        span = f"{limits['min']}..{limits['max']} default={limits['default']}" if limits else "not in dump"
        lines.append(f"{identifier} | {item['name']} | {span} | {item['authoring_status']} | "
                     f"{item['recommended_channel']} | {item['purpose']}")
        domain = item["runtime_input_domain"]
        if "min" in domain:
            lines.append(f"  runtime input: {domain['min']}..{domain['max']}; Native range is a separate output constraint")
        lines.append(f"  short reaction: {item['reaction_ownership']}")
        for reason in item["prohibited_reasons"]:
            lines.append(f"  blocked: {reason}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ranges", type=Path, required=True, help="Native --dump-parameters JSON")
    parser.add_argument("--output", type=Path, required=True, help="machine-readable capability JSON")
    parser.add_argument("--policy", type=Path, default=ROOT / POLICY_REL)
    parser.add_argument("--capture-ranges", action="store_true", help="capture a fresh Native range dump with input hashes")
    parser.add_argument("--text", action="store_true", help="also print a readable summary")
    args = parser.parse_args(argv)
    ranges = args.ranges if args.ranges.is_absolute() else ROOT / args.ranges
    output = args.output if args.output.is_absolute() else ROOT / args.output
    policy = args.policy if args.policy.is_absolute() else ROOT / args.policy
    try:
        if args.capture_ranges:
            capture_ranges(ROOT, ranges)
        report = inspect_capabilities(ROOT, ranges, policy)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
    if args.text:
        print(text_summary(report))
    else:
        print(f"Capability report: {relative_name(ROOT, output)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
