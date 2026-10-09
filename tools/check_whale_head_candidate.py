"""Verify a real six-view head-style capture without approving the character art.

Reads the staged GLB independently, including morph positions and facial material
ownership. A pass covers the recorded engineering evidence; likeness, hair shape,
expression quality and desktop compositing still require visual review.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTRACT = ROOT / "art/3d/whale-girl-head-r1/rig-contract.json"
POSES = {"front": (1, 0), "left35": (2, 35), "right35": (3, -35),
         "back": (4, 180), "blink": (5, 0), "smile": (6, 0)}
EXPRESSIONS = ("Blink_L", "Blink_R", "Smile", "LidDepth_L", "LidDepth_R")
TOLERANCES = {"bone_scale_absolute_error": 0.0001, "fixed_pose_component_error": 0.000001,
              "yaw_error_degrees": 0.1, "expression_channel_error": 0.01,
              "hair_height_palette_error": 0.006, "source_color_audit_error": 0.0000001}
LIMITATIONS = [
    "Engineering pass is separate from human character, silhouette and expression approval.",
    "Six frozen head views do not validate full-body animation, fingers, cloth or hair collisions.",
    "Morph ownership and finite 3D extents do not prove that all visible intersections are correct.",
    "GPU alpha and observed window flags do not approve Windows desktop edge compositing or real mouse input.",
    "Recorded focus probes cannot exclude an unobserved focus change between samples.",
]


def read_json(path):
    return json.loads(path.read_text(encoding="utf8"))


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def expected_hair_color(height):
    # Independent readback of this candidate's linear RGB height palette.
    # Source audit records are compared with the decoded values below as well.
    def linear_rgb(hex_color):
        channels = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
        return tuple(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in channels) + (1.0,)
    royal, middle, cyan = map(linear_rgb, ("4563A5", "5E9AD5", "70C7EF"))
    amount = max(0, min(1, (1.58 - height) / 0.95))
    amount = amount * amount * (3 - 2 * amount)
    low, high, blend = (royal, middle, amount * 2) if amount < 0.5 else (middle, cyan, (amount - 0.5) * 2)
    return tuple(a * (1 - blend) + b * blend for a, b in zip(low, high))


class Glb:
    """Minimal bounded GLB 2 accessor decoder, supporting strided and sparse data."""
    COMPONENTS = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
                  5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
    WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

    def __init__(self, path):
        data = path.read_bytes()
        if len(data) < 20 or struct.unpack_from("<III", data) != (0x46546C67, 2, len(data)):
            raise ValueError("Invalid GLB 2 header")
        cursor, chunks = 12, {}
        while cursor < len(data):
            if cursor + 8 > len(data):
                raise ValueError("Truncated GLB chunk")
            length, kind = struct.unpack_from("<II", data, cursor)
            cursor += 8
            if length % 4 or cursor + length > len(data) or kind in chunks:
                raise ValueError("Invalid or duplicated GLB chunk")
            chunks[kind] = data[cursor:cursor + length]
            cursor += length
        self.document = json.loads(chunks[0x4E4F534A].decode("utf8"))
        self.binary = chunks[0x004E4942]
        buffers = self.document.get("buffers", [])
        if len(buffers) != 1 or "uri" in buffers[0] or not 0 <= len(self.binary) - buffers[0]["byteLength"] <= 3:
            raise ValueError("Only one embedded GLB buffer is supported")
        self.cache = {}

    def _decode(self, view_index, offset, count, component, width, use_stride=True):
        view = self.document["bufferViews"][view_index]
        if view.get("buffer", 0) != 0:
            raise ValueError("Accessor uses an external buffer")
        code, byte_width = self.COMPONENTS[component]
        item_size = byte_width * width
        stride = view.get("byteStride", item_size) if use_stride else item_size
        if stride < item_size or offset < 0 or count < 0:
            raise ValueError("Invalid accessor layout")
        extent = (count - 1) * stride + item_size if count else 0
        view_start = view.get("byteOffset", 0)
        if offset + extent > view["byteLength"] or view_start + view["byteLength"] > len(self.binary):
            raise ValueError("Accessor exceeds its buffer view")
        return [struct.unpack_from("<" + code * width, self.binary, view_start + offset + row * stride)
                for row in range(count)]

    def accessor(self, index):
        if index in self.cache:
            return self.cache[index]
        accessor = self.document["accessors"][index]
        width = self.WIDTHS[accessor["type"]]
        count, component = accessor["count"], accessor["componentType"]
        values = (self._decode(accessor["bufferView"], accessor.get("byteOffset", 0), count, component, width)
                  if "bufferView" in accessor else [(0,) * width for _ in range(count)])
        if "sparse" in accessor:
            sparse = accessor["sparse"]
            indices = sparse["indices"]
            sparse_values = sparse["values"]
            decoded_indices = self._decode(indices["bufferView"], indices.get("byteOffset", 0), sparse["count"], indices["componentType"], 1, False)
            replacements = self._decode(sparse_values["bufferView"], sparse_values.get("byteOffset", 0), sparse["count"], component, width, False)
            previous = -1
            for (row,), replacement in zip(decoded_indices, replacements):
                if not previous < row < count:
                    raise ValueError("Invalid sparse accessor index")
                values[row], previous = replacement, row
        if accessor.get("normalized") and component != 5126:
            divisor = {5120: 127, 5121: 255, 5122: 32767, 5123: 65535, 5125: 4294967295}[component]
            values = [tuple(max(-1, item / divisor) for item in row) for row in values]
        if not all(math.isfinite(item) for row in values for item in row):
            raise ValueError("GLB accessor contains non-finite values")
        self.cache[index] = values
        return values


def inspect_glb(path):
    glb = Glb(path)
    doc = glb.document
    surface_rows, expression_rows, morph_meshes, gradient_rows, correction_defaults = [], {name: [] for name in EXPRESSIONS}, [], [], []
    failures = []
    for mesh in doc.get("meshes", []):
        mesh_name = mesh.get("name", "")
        shape_names = mesh.get("extras", {}).get("targetNames", [])
        if shape_names:
            morph_meshes.append({"name": mesh_name, "names": shape_names})
            for name in ("LidDepth_L", "LidDepth_R"):
                if name in shape_names:
                    index = shape_names.index(name)
                    weight = mesh.get("weights", [0] * len(shape_names))[index]
                    correction_defaults.append({"mesh": mesh_name, "channel": name, "weight": weight, "passed": weight == 0})
        for surface_index, primitive in enumerate(mesh.get("primitives", [])):
            material = doc["materials"][primitive["material"]]
            material_name = material.get("name", "")
            attributes = primitive["attributes"]
            positions = glb.accessor(attributes["POSITION"])
            indices = [row[0] for row in glb.accessor(primitive["indices"])] if "indices" in primitive else list(range(len(positions)))
            if primitive.get("mode", 4) != 4 or len(indices) % 3 or any(not 0 <= i < len(positions) for i in indices):
                failures.append(f"invalid_triangles:{mesh_name}:{surface_index}")
            if material_name.startswith("Whale"):
                required = {"POSITION": 3, "NORMAL": 3, "TEXCOORD_0": 2, "TANGENT": 4}
                valid = all(name in attributes and len(glb.accessor(attributes[name])) == len(positions)
                            and all(len(row) == width for row in glb.accessor(attributes[name]))
                            for name, width in required.items())
                extent = [max(row[i] for row in positions) - min(row[i] for row in positions) for i in range(3)]
                alpha = material.get("pbrMetallicRoughness", {}).get("baseColorFactor", [1, 1, 1, 1])[3]
                opaque = material.get("alphaMode", "OPAQUE") == "OPAQUE" and alpha >= 0.999
                colors = glb.accessor(attributes["COLOR_0"]) if "COLOR_0" in attributes else []
                colors_valid = not colors or (len(colors) == len(positions) and all(len(row) in (3, 4) and all(0 <= v <= 1 for v in row) for row in colors))
                if mesh_name.startswith(("WhaleBackCascade", "WhaleOuterCascade", "WhaleSideCurl")):
                    gradient_valid = colors_valid and bool(colors) and all(len(row) == 4 for row in colors)
                    color_row = {"mesh": mesh_name, "primitive": surface_index, "vertices": len(positions), "passed": False}
                    if gradient_valid:
                        minimum = [min(row[i] for row in colors) for i in range(4)]
                        maximum = [max(row[i] for row in colors) for i in range(4)]
                        span = [high - low for low, high in zip(minimum, maximum)]
                        palette_error = max(abs(value - expected_hair_color(position[1])[i])
                                            for color, position in zip(colors, positions) for i, value in enumerate(color))
                        top = max(range(len(positions)), key=lambda i: positions[i][1])
                        bottom = min(range(len(positions)), key=lambda i: positions[i][1])
                        gradient_valid = palette_error <= TOLERANCES["hair_height_palette_error"] and span[1] >= 0.12 and maximum[0] <= 0.5 and minimum[3] >= 0.999
                        color_row.update(normalized_rgba_min=minimum, normalized_rgba_max=maximum, channel_span=span,
                                         max_height_palette_error=palette_error,
                                         top_endpoint={"height_m": positions[top][1], "rgba": list(colors[top])},
                                         bottom_endpoint={"height_m": positions[bottom][1], "rgba": list(colors[bottom])}, passed=gradient_valid)
                    gradient_rows.append(color_row)
                    if not gradient_valid:
                        failures.append("wrong_or_placeholder_hair_COLOR_0:" + mesh_name)
                    colors_valid = colors_valid and gradient_valid
                weights = glb.accessor(attributes["WEIGHTS_0"]) if "WEIGHTS_0" in attributes else []
                joints = glb.accessor(attributes["JOINTS_0"]) if "JOINTS_0" in attributes else []
                max_weight_error = max((abs(sum(row) - 1) for row in weights), default=1)
                joint_count = len(doc.get("skins", [{}])[0].get("joints", []))
                skin_valid = (len(weights) == len(joints) == len(positions) and max_weight_error <= 1e-5
                              and all(len(row) == 4 and all(v >= 0 for v in row) for row in weights)
                              and all(len(row) == 4 and all(0 <= joint < joint_count for joint, weight in zip(row, weights[i]) if weight > 0) for i, row in enumerate(joints)))
                valid = valid and opaque and min(extent) > 1e-6 and colors_valid and skin_valid
                surface_rows.append({"mesh": mesh_name, "surface": surface_index, "material": material_name,
                                     "vertices": len(positions), "extent_m": extent, "opaque": opaque,
                                     "has_vertex_colors": "COLOR_0" in attributes, "maximum_weight_sum_error": max_weight_error,
                                     "skin_valid": skin_valid, "passed": valid})
                if not valid:
                    failures.append(f"head_surface_attributes_or_volume:{mesh_name}:{surface_index}")
            for name, target in zip(shape_names, primitive.get("targets", [])):
                if name not in expression_rows or "POSITION" not in target:
                    continue
                deltas = glb.accessor(target["POSITION"])
                if len(deltas) != len(positions):
                    raise ValueError("Morph target/base accessor count mismatch")
                changed = [i for i in set(indices) if sum(value * value for value in deltas[i]) > 1e-14]
                if not changed:
                    continue
                eye_material = (material_name.startswith("WhaleEye") or material_name.startswith("WhaleIris")
                                or material_name in ("WhalePupil", "WhaleHighlight"))
                correction = name.startswith("LidDepth")
                allowed = material_name == "WhaleMouth" if name == "Smile" else eye_material
                if correction:
                    allowed = allowed and material_name not in ("WhaleEyeLash", "WhaleEyeBrow", "WhaleEyeBlush")
                wrong_side = 0 if name == "Smile" else sum(positions[i][0] * (1 if name.endswith("_L") else -1) <= 0 for i in changed)
                non_depth_delta = max((abs(deltas[i][axis]) for i in changed for axis in (0, 1)), default=0) if correction else 0
                expression_rows[name].append({"mesh": mesh_name, "material": material_name,
                                              "changed_vertices": len(changed), "wrong_eye_side_vertices": wrong_side,
                                              "maximum_non_depth_delta_m": non_depth_delta,
                                              "max_delta_m": max(math.sqrt(sum(v * v for v in deltas[i])) for i in changed),
                                              "passed": allowed and not wrong_side and non_depth_delta <= 1e-7})
    for name, rows in expression_rows.items():
        if not rows or not all(row["passed"] for row in rows):
            failures.append("morph_ownership:" + name)
    return {"mesh_count": len(doc.get("meshes", [])), "node_count": len(doc.get("nodes", [])),
            "skin_joint_counts": [len(skin.get("joints", [])) for skin in doc.get("skins", [])],
            "animations": sorted(item.get("name") for item in doc.get("animations", [])),
            "images": len(doc.get("images", [])), "textures": len(doc.get("textures", [])),
            "morph_meshes": morph_meshes,
            "gradient_colors": gradient_rows,
            "correction_defaults": correction_defaults,
            "head_surfaces": surface_rows, "expression_ownership": expression_rows, "failures": failures}


def check_review(review, contract_path):
    from PIL import Image, ImageChops
    receipt_path, runtime_path, trace_path = review / "review-receipt.json", review / "captures/report.json", review / "captures/trace.json"
    receipt, runtime, trace, contract = map(read_json, (receipt_path, runtime_path, trace_path, contract_path))
    checks = {}

    def check(name, passed, **facts):
        checks[name] = {"passed": bool(passed), **facts}

    profile = runtime.get("review_profile", {})
    check("recorded_runtime_identity", receipt.get("runtime") == runtime and receipt.get("review_kind") == runtime.get("review_kind") == "head-style"
          and receipt.get("review_profile") == profile and read_json(review / "review-profile.json") == profile
          and profile.get("schema_version") == 1 and runtime.get("host_mode", {}).get("enabled") is False)
    check("capture_completed", receipt.get("status") == "technical_capture_passed" and receipt.get("exit_code") == 0
          and runtime.get("visual_capture_completed") is True and not runtime.get("errors") and not receipt.get("errors"),
          receipt_status=receipt.get("status"), runtime_status=runtime.get("status"), errors=runtime.get("errors"))
    gpu = runtime.get("headless") is False and runtime.get("display_server") != "headless" and bool(runtime.get("native_handle")) and bool(runtime.get("adapter")) and bool(runtime.get("rendering_driver"))
    check("actual_gpu", gpu, display_server=runtime.get("display_server"), adapter=runtime.get("adapter"), rendering_driver=runtime.get("rendering_driver"))
    staged_glb = review / "project/assets/character.glb"
    model_hash = fingerprint(staged_glb)["sha256"]
    check("model_identity", model_hash == contract.get("glb_sha256") == runtime.get("model_sha256") == receipt.get("model_fingerprint", {}).get("sha256"),
          staged=model_hash, contract=contract.get("glb_sha256"), runtime=runtime.get("model_sha256"))
    if receipt.get("frozen_model_contract") is not None:
        frozen_path = review / "rig-contract.json"
        check("frozen_model_contract_receipt", frozen_path.is_file() and fingerprint(frozen_path) == receipt["frozen_model_contract"],
              frozen_contract=fingerprint(frozen_path) if frozen_path.is_file() else None)
    copied_sources = []
    for name, expected in receipt.get("sources", {}).items():
        if name.startswith("godot/toon-prototype/"):
            relative = Path(name.removeprefix("godot/toon-prototype/"))
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("Invalid staged source path")
            path = review / "project" / relative
            copied_sources.append({"file": name, "passed": path.is_file() and fingerprint(path) == expected})
    check("frozen_renderer_source_identity", bool(copied_sources) and all(row["passed"] for row in copied_sources), files=copied_sources)
    log_errors = []
    for name in ("import.log", "runtime.log"):
        log_errors.extend(re.findall(r"^.*(?:SCRIPT ERROR|ERROR:).*$", (review / name).read_text(encoding="utf8"), re.MULTILINE))
    check("no_import_or_runtime_errors", not log_errors, errors=log_errors)

    glb = inspect_glb(staged_glb)
    structure = contract.get("audit", {}).get("structure", {})
    check("glb_structure_and_surface_readback", not glb["failures"] and glb["images"] == glb["textures"] == 0
          and glb["mesh_count"] == structure.get("mesh_count") and glb["skin_joint_counts"] == structure.get("skin_joint_counts")
          and glb["node_count"] == structure.get("node_count") and glb["morph_meshes"] == structure.get("morph_meshes")
          and runtime.get("bone_count") == len(contract["bones"])
          and sorted(name.split("/")[-1] for name in runtime.get("animation_names", [])) == glb["animations"],
          mesh_count=glb["mesh_count"], skin_joint_counts=glb["skin_joint_counts"], failures=glb["failures"])
    check("glb_expression_material_and_eye_isolation", all(glb["expression_ownership"].values())
          and all(row["passed"] for rows in glb["expression_ownership"].values() for row in rows), ownership=glb["expression_ownership"])
    check("eyelid_correction_is_depth_only_and_neutral_by_default", len(glb["correction_defaults"]) == 2
          and all(item["passed"] for item in glb["correction_defaults"])
          and contract.get("expression_drivers") == {"LidDepth_L": "4*Blink_L*(1-Blink_L)", "LidDepth_R": "4*Blink_R*(1-Blink_R)"},
          defaults=glb["correction_defaults"], expression_drivers=contract.get("expression_drivers"),
          endpoint_weights={"blink_0": 0, "blink_1": 0, "blink_half": 1})
    source_colors = contract.get("audit", {}).get("exported_surface_attributes", {})
    expected_gradients = source_colors.get("continuous_gradient_COLOR_0_meshes", [])
    source_color_rows = {(item["mesh"], item["primitive"]): item["attributes"]["COLOR_0"]
                         for item in source_colors.get("primitives", []) if "COLOR_0" in item.get("attributes", {})}
    color_audit_matches = []
    for item in glb["gradient_colors"]:
        source = source_color_rows.get((item["mesh"], item["primitive"]), {})
        error = 0.0
        valid = item["passed"] and source.get("vertices") == item["vertices"] and source.get("passed") is True
        for field in ("normalized_rgba_min", "normalized_rgba_max", "channel_span"):
            left, right = item.get(field, []), source.get(field, [])
            valid = valid and len(left) == len(right) == 4
            if len(left) == len(right) == 4:
                error = max(error, *(abs(a - b) for a, b in zip(left, right)))
        error = max(error, abs(item.get("max_height_palette_error", 999) - source.get("max_height_palette_error", -999)))
        color_audit_matches.append({"mesh": item["mesh"], "maximum_source_audit_component_error": error,
                                    "passed": valid and error <= TOLERANCES["source_color_audit_error"]})
    check("actual_hair_COLOR_0_matches_height_palette_and_source_audit", len(expected_gradients) == 9
          and {item["mesh"] for item in glb["gradient_colors"]} == set(expected_gradients)
          and len(color_audit_matches) == 9 and all(item["passed"] for item in color_audit_matches), colors=glb["gradient_colors"], source_audit_matches=color_audit_matches)
    material_rows = runtime.get("materials", [])
    by_mesh = {}
    for item in material_rows:
        by_mesh.setdefault(item["mesh"], []).append(item["source_material"])
    expected_hidden = {name for name, materials in by_mesh.items() if not any(item.startswith("Whale") for item in materials)}
    hidden = runtime.get("hidden_review_meshes", [])
    check("head_scene_visibility_scope", bool(expected_hidden) and set(hidden) == expected_hidden
          and len(hidden) == len(set(hidden)) and profile.get("hide_proxy_body") is True
          and runtime.get("review_visibility_scope") == "head_geometry_only_other_materials_hidden_in_scene", hidden_meshes=hidden)
    runtime_gradient_names = {name.replace(".", "_") for name in expected_gradients}
    actual_runtime_gradient_names = {row["mesh"] for row in material_rows if row.get("uses_vertex_colors") is True}
    check("runtime_uses_all_nine_actual_hair_gradients", bool(runtime_gradient_names) and actual_runtime_gradient_names == runtime_gradient_names,
          expected=sorted(runtime_gradient_names), actual=sorted(actual_runtime_gradient_names))

    captures = runtime.get("captures", [])
    captured = {item.get("file"): item for item in captures}
    expected_files = {name + ".png" for name in POSES}
    actual_files = {path.name for path in (review / "captures").glob("*.png")}
    receipt_images = {item["file"]: item for item in receipt.get("images", [])}
    check("six_gpu_pose_inventory", len(captures) == len(captured) == 6 and set(captured) == expected_files
          and actual_files == expected_files | {"shutdown-transparent.png"}
          and set(receipt_images) == actual_files, actual_files=sorted(actual_files))
    images, loaded, pose_facts = [], {}, []
    viewport_size = tuple(int(x) for x in profile.get("window_size", runtime.get("window", {}).get("size", [])))
    yaw_base = float(profile.get("model_yaw_degrees", 0))
    for name, (scheduled, yaw) in POSES.items():
        filename, path = name + ".png", review / "captures" / (name + ".png")
        capture = captured.get(filename, {})
        pose = capture.get("pose", {})
        with Image.open(path) as image:
            loaded[name] = image.convert("RGBA")
        rgba = loaded[name]
        alpha = rgba.getchannel("A")
        bbox = alpha.getbbox()
        margin = bool(bbox and bbox[0] > 0 and bbox[1] > 0 and bbox[2] < rgba.width and bbox[3] < rgba.height)
        actual_fingerprint = fingerprint(path)
        row = {"file": filename, "pixels": list(rgba.size), "bbox": list(bbox) if bbox else None,
               **actual_fingerprint, "passed": alpha.getextrema() == (0, 255) and margin and rgba.size == viewport_size
               and all(receipt_images.get(filename, {}).get(key) == value for key, value in actual_fingerprint.items())
               and capture.get("source") == "gpu_viewport_after_frame_post_draw"
               and capture.get("alpha", {}).get("clipped_at_viewport_edge") is False}
        images.append(row)
        pose_facts.append({"file": filename, "actual_time": capture.get("actual_time"), "yaw": pose.get("character_yaw_degrees"),
                           "passed": capture.get("scheduled_time") == scheduled and scheduled <= capture.get("actual_time", -1) <= scheduled + 2 / 60
                           and pose.get("state") == "head-style"
                           and abs((pose.get("character_yaw_degrees", 999) - yaw - yaw_base + 180) % 360 - 180) <= TOLERANCES["yaw_error_degrees"]})
    check("gpu_image_integrity_and_margins", all(item["passed"] for item in images), images=images)
    check("frozen_six_view_angles_and_capture_clock", all(item["passed"] for item in pose_facts), poses=pose_facts)

    control_rows = []
    probe = profile.get("expression_probe", {})
    blink_peak = max(0, min(1, float(probe.get("blink_peak", 1))))
    smile_peak = max(0, min(1, float(probe.get("smile_peak", 1))))
    for name in POSES:
        values = captured[name + ".png"]["pose"].get("blend_shapes", {})
        for expression in EXPRESSIONS:
            matches = [value for key, value in values.items() if key.split("/")[-1] == expression]
            target = 0.0
            if name == "blink" and expression.startswith("Blink"):
                target = blink_peak
            elif name == "blink" and expression.startswith("LidDepth"):
                target = 4 * blink_peak * (1 - blink_peak)
            elif name == "smile" and expression == "Smile":
                target = smile_peak
            control_rows.append({"pose": name, "channel": expression, "values": matches, "expected": target,
                                 "passed": len(matches) == 1 and isinstance(matches[0], (float, int)) and abs(matches[0] - target) <= TOLERANCES["expression_channel_error"]})
    check("isolated_runtime_expression_channels", all(item["passed"] for item in control_rows), channels=control_rows)
    changes = {}
    for name in ("left35", "right35", "back", "blink", "smile"):
        difference = ImageChops.difference(loaded["front"].convert("RGB"), loaded[name].convert("RGB"))
        red, green, blue = difference.split()
        maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
        changes[name] = maximum.width * maximum.height - maximum.histogram()[0]
    check("actual_pixels_change_for_angle_and_expression", all(value > 0 for value in changes.values()), changed_pixels=changes,
          limitation="Observed change supports rendered channels, not the quality of the expression")

    samples = trace.get("samples", [])
    expected_bones = {bone["name"] for bone in contract["bones"]}
    consistent = bool(samples) and len(samples) == runtime.get("trace_samples")
    baseline = samples[0]["bones"] if samples else {}
    max_scale, max_pose = 0.0, 0.0
    maximum_correction_driver_error = 0.0
    correction_trace_valid = bool(samples)
    for previous, sample in zip([None] + samples[:-1], samples):
        consistent = consistent and sample.get("state") == "head-style" and set(sample.get("bones", {})) == expected_bones
        consistent = consistent and math.isfinite(sample["time"]) and (previous is None or sample["time"] > previous["time"])
        for name, bone in sample.get("bones", {}).items():
            for field, width in (("scale", 3), ("position", 3), ("global_position", 3), ("rotation_xyzw", 4)):
                values = bone.get(field, [])
                if len(values) != width or not all(isinstance(v, (float, int)) and math.isfinite(v) for v in values):
                    raise ValueError("Invalid pose vector")
                max_pose = max(max_pose, *(abs(v - baseline[name][field][i]) for i, v in enumerate(values)))
            max_scale = max(max_scale, *(abs(v - 1) for v in bone["scale"]))
        shapes = sample.get("blend_shapes", {})
        for side in ("L", "R"):
            base = [value for key, value in shapes.items() if key.split("/")[-1] == "Blink_" + side]
            correction = [value for key, value in shapes.items() if key.split("/")[-1] == "LidDepth_" + side]
            valid = len(base) == len(correction) == 1 and all(isinstance(value, (float, int)) and math.isfinite(value) and -1e-6 <= value <= 1.000001 for value in base + correction)
            correction_trace_valid = correction_trace_valid and valid
            if valid:
                maximum_correction_driver_error = max(maximum_correction_driver_error, abs(correction[0] - 4 * base[0] * (1 - base[0])))
    check("body_pose_stays_fixed_while_head_views_and_morphs_change", consistent and max_pose <= TOLERANCES["fixed_pose_component_error"] and max_scale <= TOLERANCES["bone_scale_absolute_error"],
          trace_samples=len(samples), maximum_pose_component_change=max_pose, maximum_scale_error=max_scale)
    check("eyelid_depth_driver_matches_independent_blink_channels_each_sample", correction_trace_valid and maximum_correction_driver_error <= 1e-5,
          maximum_driver_error=maximum_correction_driver_error, trace_samples=len(samples),
          note="Correction weight is zero at blink endpoints and reaches one at blink=0.5")

    shutdown_path = review / "captures/shutdown-transparent.png"
    with Image.open(shutdown_path) as image:
        shutdown_alpha = image.convert("RGBA").getchannel("A").getextrema()
    shutdown_hash = fingerprint(shutdown_path)
    check("atomic_transparent_shutdown", shutdown_alpha == (0, 0) and runtime.get("shutdown_gpu_frame_presented") is True
          and runtime.get("shutdown_visible_alpha_pixels") == 0 and runtime.get("shutdown_strategy") == "hide_all_visual_content_then_frame_post_draw_then_quit"
          and all(receipt_images.get("shutdown-transparent.png", {}).get(key) == value for key, value in shutdown_hash.items()))
    audit = receipt.get("window_audit") or {}
    observed = receipt.get("window_audit_samples", [])
    check("native_window_observation", bool(observed) and audit.get("status") == "passed" and audit.get("pid") == runtime.get("pid")
          and audit.get("native_handle") == runtime.get("native_handle") and all(item.get("status") == "passed" and item.get("owns_foreground") is False for item in observed)
          and all(audit.get(field) is True for field in ("visible", "caption_absent", "topmost", "no_activate"))
          and audit.get("outside_corner_hits_own_window") is False, observed_samples=len(observed), audit=audit)
    guard = runtime.get("focus_guard", {})
    probes = runtime.get("fg_probes", [])
    check("focus_observations_and_guard", bool(probes) and all(item.get("fg_is_self") is False for item in probes)
          and not any(item.get("has_focus") is True for item in runtime.get("focus_events", []))
          and runtime.get("startup_window_flags", {}).get("has_focus_before_configuration") is False
          and runtime.get("window", {}).get("has_focus") is False and guard.get("configured") is True
          and guard.get("cbt_hook_installed") is True and guard.get("forced_assignments") == 0 and guard.get("restores_failed") == 0,
          probe_count=len(probes), forced_assignments=guard.get("forced_assignments"), restores_failed=guard.get("restores_failed"))
    failed = [name for name, item in checks.items() if not item["passed"]]
    return {"schema_version": 1, "kind": "whale_head_candidate_engineering_check", "passed": not failed,
            "status": "engineering_checks_passed_art_approval_pending" if not failed else "failed",
            "review": str(review), "contract": str(contract_path), "checks": checks, "failed_checks": failed,
            "glb_readback": glb, "images": images,
            "input_fingerprints": {"review-receipt.json": fingerprint(receipt_path), "captures/report.json": fingerprint(runtime_path),
                                   "captures/trace.json": fingerprint(trace_path), "contract": fingerprint(contract_path), "staged_glb": fingerprint(staged_glb)},
            "character_art_approved": False, "visual_approval": "pending", "formal_model_replaced": False,
            "review_scope": "standard_head_endpoints" if blink_peak == smile_peak == 1 else "explicit_expression_probe",
            "tolerances": TOLERANCES,
            "limitations": LIMITATIONS}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review", required=True, type=Path)
    parser.add_argument("--contract", type=Path, help="Defaults to the frozen REVIEW/rig-contract.json, then the current candidate contract")
    parser.add_argument("--output", type=Path, help="Defaults to REVIEW/head-check.json")
    args = parser.parse_args()
    review = args.review.resolve()
    output = (args.output or review / "head-check.json").resolve()
    try:
        contract_path = args.contract.resolve() if args.contract else (review / "rig-contract.json" if (review / "rig-contract.json").is_file() else DEFAULT_CONTRACT)
        result = check_review(review, contract_path.resolve())
    except (OSError, ValueError, KeyError, TypeError, IndexError, struct.error) as error:
        result = {"schema_version": 1, "kind": "whale_head_candidate_engineering_check", "status": "insufficient_evidence",
                  "passed": False, "error": str(error), "review": str(review), "character_art_approved": False,
                  "visual_approval": "pending", "formal_model_replaced": False, "limitations": LIMITATIONS}
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf8")
    print(json.dumps({"status": result["status"], "output": str(output), "failed_checks": result.get("failed_checks", []),
                      "error": result.get("error")}, ensure_ascii=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
