"""Copy audited source layers into a new, pixel-exact handoff directory.

This extracts decoded PSD pixels only. It never recreates the PSD, edits a rig,
imports into an editor, or replaces the production layer manifest.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re

from live2d_paths import enable_authoring_dependencies
enable_authoring_dependencies()
from PIL import Image
from psd_tools import PSDImage


ROOT = Path(__file__).resolve().parents[1]
SHA_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
RESERVED = {"con", "prn", "aux", "nul"} | {
    f"{prefix}{number}" for prefix in ("com", "lpt") for number in range(1, 10)
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha(path: Path) -> str:
    return sha(path.read_bytes())


def checked_sha(value: str, label: str) -> str:
    if not isinstance(value, str) or not SHA_PATTERN.fullmatch(value):
        raise ValueError(f"Missing or invalid expected SHA256: {label}")
    return value


def safe_filename(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]+\.png", value):
        raise ValueError(f"Layer output must be a plain PNG filename: {value!r}")
    if value[:-4].lower() in RESERVED:
        raise ValueError(f"Reserved Windows filename: {value}")
    return value


def source_canvas(crop: Image.Image, bounds: tuple[int, ...], size: tuple[int, ...]) -> Image.Image:
    """Paste without an alpha mask: low-alpha pixels must remain byte exact."""
    left, top, right, bottom = bounds
    if not (0 <= left < right <= size[0] and 0 <= top < bottom <= size[1]):
        raise ValueError(f"Layer bounds outside canvas: {bounds}")
    if crop.mode != "RGBA" or crop.size != (right - left, bottom - top):
        raise ValueError("Decoded source raster mode or dimensions do not match bounds")
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    canvas.paste(crop, (left, top))
    return canvas


def check_new_output(output: Path, sources: list[Path]) -> Path:
    if output.exists() or output.is_symlink():
        raise ValueError("Output must be a new directory; existing outputs are never overwritten")
    resolved = output.resolve()
    for source in sources:
        if source.is_relative_to(resolved) or resolved.is_relative_to(source.parent):
            raise ValueError(f"Output overlaps source inputs: {source}")
    return resolved


def prepare_pack(audit_path: Path, psd_path: Path, output: Path) -> dict:
    audit_path = audit_path.resolve(strict=True)
    psd_path = psd_path.resolve(strict=True)
    audit_bytes = audit_path.read_bytes()
    audit = json.loads(audit_bytes)
    source_section = audit["PSD_vs_original_HEAD"]
    mapping = audit["layer_manifest_audit"]["source_layer_mapping"]
    manifest = audit["layer_manifest_audit"]["proposed_manifest"]
    expected_psd = [value for key, value in audit["fixed_input_hashes"].items()
                    if key.lower().endswith(".psd")]
    if len(expected_psd) != 1:
        raise ValueError("Audit must identify exactly one source PSD hash")
    expected_psd_sha = checked_sha(expected_psd[0], "source PSD")
    if file_sha(psd_path) != expected_psd_sha:
        raise ValueError("Source PSD hash changed since audit")
    if not isinstance(manifest, dict) or len(manifest) != len(mapping):
        raise ValueError("Manifest must have exactly one entry per audited PSD layer")

    inputs = {str(audit_path): sha(audit_bytes), str(psd_path): expected_psd_sha}
    plans = []
    names, filenames, ids = set(), set(), set()
    psd = PSDImage.open(psd_path)
    if list(psd.size) != source_section["canvas"] or len(psd) != len(mapping) \
            or len(psd) != source_section["PSD_layer_count"]:
        raise ValueError("Source PSD canvas or layer count differs from audit")
    for index, (layer, entry) in enumerate(zip(psd, mapping)):
        source_id = f"{layer.name}#{index}"
        name = entry["manifest_key"]
        filename = safe_filename(entry["recommended_manifest_value"])
        if (layer.name != entry["source_name"] or name != layer.name
                or source_id != entry["source_layer_id"] or layer.is_group()):
            raise ValueError(f"Source PSD layer identity changed at index {index}")
        if name in names or filename.lower() in filenames or source_id in ids:
            raise ValueError("Layer names, IDs and output filenames must be unique")
        if manifest.get(name) != filename:
            raise ValueError(f"Audited manifest mapping differs for {name}")
        names.add(name)
        filenames.add(filename.lower())
        ids.add(source_id)
        bounds = tuple(layer.bbox)
        if list(bounds) != entry["PSD_bbox"] or list(psd.size) != entry["expected_canvas"]:
            raise ValueError(f"Source PSD layer bounds changed: {source_id}")
        raster = layer.topil()
        if raster is None:
            raise ValueError(f"No raw decoded source raster: {source_id}")
        crop = raster.convert("RGBA")
        if sha(crop.tobytes()) != checked_sha(entry["RGBA_crop_sha256"], source_id):
            raise ValueError(f"Source PSD pixels differ from audit: {source_id}")
        canvas = source_canvas(crop, bounds, psd.size)
        full_sha = sha(canvas.tobytes())
        source = None
        content = None
        if entry["existing_PNG"]:
            source = Path(entry["PNG_path"]).resolve(strict=True)
            content = source.read_bytes()
            expected = checked_sha(entry["existing_PNG_sha256"], str(source))
            if sha(content) != expected:
                raise ValueError(f"Source PNG hash changed since audit: {source}")
            inputs[str(source)] = expected
            with Image.open(source) as existing:
                decoded = existing.convert("RGBA")
                if decoded.size != psd.size or decoded.tobytes() != canvas.tobytes():
                    raise ValueError(f"Full source PNG RGBA differs from PSD: {source_id}")
        plans.append({"entry": entry, "filename": filename, "source": source,
                      "content": content, "crop": crop if source is None else None,
                      "full_sha": full_sha,
                      "alpha_2_pixels": crop.getchannel("A").histogram()[2]})

    destination = check_new_output(output, [Path(path) for path in inputs])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.mkdir(exist_ok=False)
    output_layers = []
    for plan in plans:
        entry = plan["entry"]
        target = destination / plan["filename"]
        if plan["source"] is not None:
            target.write_bytes(plan["content"])
            action = "copied_source_PNG_bytes"
        else:
            canvas = source_canvas(plan["crop"], tuple(entry["PSD_bbox"]), psd.size)
            canvas.save(target, format="PNG")
            action = "extracted_raw_PSD_RGBA_at_original_coordinates"
        with Image.open(target) as saved:
            saved = saved.convert("RGBA")
            if saved.size != psd.size or sha(saved.tobytes()) != plan["full_sha"]:
                raise RuntimeError(f"Written PNG failed exact decoded RGBA verification: {target}")
        if plan["content"] is not None and target.read_bytes() != plan["content"]:
            raise RuntimeError(f"Written copy differs from source bytes: {target}")
        output_layers.append({
            "source_layer_id": entry["source_layer_id"], "source_name": entry["source_name"],
            "PSD_bottom_to_top_index": len(output_layers), "PSD_bbox": entry["PSD_bbox"],
            "file": plan["filename"], "action": action, "source_PNG": str(plan["source"]) if plan["source"] else None,
            "file_sha256": file_sha(target), "decoded_full_canvas_RGBA_sha256": plan["full_sha"],
            "decoded_crop_RGBA_sha256": entry["RGBA_crop_sha256"],
            "source_crop_alpha_2_pixels_preserved": plan["alpha_2_pixels"],
            "exact_full_canvas_RGBA_matches_PSD": True,
        })
    for path, expected in inputs.items():
        if file_sha(Path(path)) != expected:
            raise RuntimeError(f"Source input changed while packing: {path}")
    manifest_path = destination / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    receipt = {
        "schema_version": 1, "kind": "isolated_pixel_exact_source_layer_pack",
        "created_utc": datetime.now(timezone.utc).isoformat(), "canvas": list(psd.size),
        "layer_count": len(plans), "copied_PNG_count": sum(p["source"] is not None for p in plans),
        "extracted_PSD_layer_count": sum(p["source"] is None for p in plans),
        "output_directory": str(destination), "manifest_sha256": file_sha(manifest_path),
        "all_decoded_full_canvas_RGBA_equal_PSD": True, "source_inputs_unchanged": True,
        "source_inputs": [{"path": p, "sha256": s} for p, s in inputs.items()],
        "layers_bottom_to_top": output_layers,
        "source_contract": "Flat source rasters only; no compositing, trimming, resizing, alpha masking, rig generation or editor imports.",
        "consumer_contract": {
            "manifest_format": "Flat source name to sibling PNG filename dictionary, matching the existing production manifest format.",
            "layer_order": "PSD order is explicit in receipt.layers_bottom_to_top; filename prefixes are not ordering authority.",
            "prepare_live2d_psd": "Does not read the manifest. Recreates rasters and overwrites fixed production PSD and layer outputs; do not use it to roundtrip this source pack.",
            "build_live2d_model": "Imports the fixed production PSD, reads only eyelash-l and eyelash-r from fixed art/live2d/layers manifest. Has no pack input argument and will not adopt this isolated pack automatically.",
            "binding_equivalence": "Not established: matching source pixels does not prove author HEAD, CMO3 or Native rig equivalence.",
            "formal_inputs_replaced": False,
        },
        "tool": {"path": str(Path(__file__).resolve()), "sha256": file_sha(Path(__file__))},
    }
    (destination / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    return receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--psd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    receipt = prepare_pack(args.audit, args.psd, args.output)
    print(json.dumps({key: receipt[key] for key in (
        "output_directory", "layer_count", "copied_PNG_count", "extracted_PSD_layer_count",
        "all_decoded_full_canvas_RGBA_equal_PSD", "source_inputs_unchanged")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
