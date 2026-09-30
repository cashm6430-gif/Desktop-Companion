"""Rebuild the authored model through a running local PSD2Live instance.

Run prepare_live2d_psd.py first. No credentials are printed. Generated model
files are staged under build/psd2live/whale-seam-fixed-output for visual review.
The user's existing .psd2live archive is never overwritten.
"""
import json
import sys
from itertools import product
from pathlib import Path
import numpy as np
from PIL import Image
from psd2live_client import call, initialize

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/psd2live/whale-seam-fixed-output"


def main():
    _, session = initialize()
    state = None
    ident = 2

    def invoke(name, arguments):
        nonlocal session, state, ident
        response, session = call("tools/call", {"name": name, "arguments": arguments}, ident, session)
        ident += 1
        result = response.get("result", {})
        payload = result.get("structuredContent", {})
        if response.get("error") or result.get("isError") or payload.get("error"):
            raise RuntimeError(f"{name}: {json.dumps(response, ensure_ascii=False)[:1400]}")
        state = payload.get("state", state)
        print(name, payload.get("summary", "ok"), flush=True)
        return payload

    if "--resume" in sys.argv:
        invoke("inspect", {"scope": "project"})
    else:
        invoke("asset", {"request": {"mode": "psd", "path": str(ROOT / "art/live2d/whale-girl-layered-draft.psd")}})
    (OUTPUT / "diagnostics").mkdir(parents=True, exist_ok=True)
    invoke("inspect", {"scope": "settings"})
    invoke("settings", {"state": state, "changes": {
        "atlasSize": 4096, "meshSpacing": 32, "meshOuterMargin": 3,
        "texturePadding": 8, "headStrength": 0.65, "bodyStrength": 0.6,
        "physicsEyeJelly": False, "mouthOutlineEnabled": False,
    }})
    layers = invoke("inspect", {"scope": "layers", "limit": 64})
    (OUTPUT / "diagnostics/layers.json").write_text(json.dumps(layers, ensure_ascii=False, indent=2), encoding="utf8")
    # Classification determines inherited motion. The eye backing must follow
    # the face rather than stay on the body when the head turns.
    for name, role, extra in (
        ("arm backing", "unknown", {}),
        ("irides-l", "irides", {"side": "left", "type": "preset"}),
        ("irides-r", "irides", {"side": "right", "type": "preset"}),
        ("eyewhite-l", "eyewhite", {"side": "left", "type": "preset"}),
        ("eyewhite-r", "eyewhite", {"side": "right", "type": "preset"}),
        ("eyelash-l", "eyelash", {"side": "left", "type": "preset"}),
        ("eyelash-r", "eyelash", {"side": "right", "type": "preset"}),
        ("mouth", "mouth_close", {"type": "switch", "parameter": "ParamSmileOpen", "switch_id": 0}),
        ("mouth open", "mouth_open", {"type": "switch", "parameter": "ParamSmileOpen", "switch_id": 1}),
        ("handwear right", "handwear", {"side": "right", "type": "toggle", "parameter": "ParamGrassVisible"}),
        ("handwear_r", "handwear", {"side": "right", "type": "switch", "parameter": "ParamHandRGrip", "switch_id": 0}),
        ("handwear.right", "handwear", {"side": "right", "type": "switch", "parameter": "ParamHandRGrip", "switch_id": 1}),
    ):
        # Source layer IDs are stable name#index, as returned by inspect.
        entries = layers.get("layers", layers.get("items", []))
        layer_id = next(x["id"] for x in entries if x.get("name") == name)
        invoke("layer", {"state": state, "layer_id": layer_id, "role": role, **extra})

    objects = invoke("inspect", {"scope": "objects", "limit": 64})
    (OUTPUT / "diagnostics/objects.json").write_text(json.dumps(objects, ensure_ascii=False, indent=2), encoding="utf8")
    # Drawable IDs are generated from semantic roles. Resolve the prop from
    # its source name so role naming changes cannot silently bind the wrong mesh.
    entries = objects.get("objects", objects.get("items", []))
    def mesh(name):
        return next(x["target"].split(":", 1)[1] for x in entries if x.get("name") == name)
    prop, left_arm, right_arm = mesh("handwear right"), mesh("handwear-l"), mesh("handwear-r")
    open_hand, grip = mesh("handwear_r"), mesh("handwear.right")
    for identifier, label, minimum, maximum in (
        ("ParamArmLA", "左手动作", -65, 65), ("ParamArmRA", "右手动作", -65, 65),
        ("ParamGrassReach", "向观众伸手", 0, 1), ("ParamGrassSwing", "草穗摆动", -1, 1),
        ("ParamGrassTipBend", "柔软草穗滞后", -1, 1),
        ("ParamEyeSmile", "笑眼弧度", 0, 1),
    ):
        invoke("parameter", {"request": {"mode": "create", "state": state,
            "parameter_id": identifier, "name": label, "min": minimum, "max": maximum, "default": 0}})
    source_layers = layers.get("items", [])
    def frame(name):
        bounds = next(x["bounds"] for x in source_layers if x["name"] == name)
        return [bounds[0] - 3, bounds[1] - 3, bounds[2] - bounds[0] + 6, bounds[3] - bounds[1] + 6]
    def normalized(point, bounds):
        return [(point[0] - bounds[0]) / bounds[2], (point[1] - bounds[1]) / bounds[3]]

    def bezier_controls(polynomial):
        a, b, c, d = polynomial
        return [float(a), float(a + b / 3), float(a + 2 * b / 3 + c / 3), float(a + b + c + d)]

    for side, parameter, edge_y in (("l", "ParamEyeLOpen", 472), ("r", "ParamEyeROpen", 468)):
        lash_name = "eyelash-" + side
        lash_bounds = frame(lash_name)
        # Sample the painted lash centreline once, preserving actual thickness.
        preview = next((ROOT / "build/psd2live/layer-previews").glob("*-" + lash_name + ".png"))
        alpha = np.asarray(Image.open(preview).getchannel("A"), dtype=float) / 255
        weights = alpha.sum(axis=0)
        columns = np.flatnonzero(weights > 0)
        centres = (alpha * np.arange(alpha.shape[0])[:, None]).sum(axis=0)[columns] / weights[columns]
        source_line = np.polynomial.polynomial.polyfit(
            (columns - lash_bounds[0]) / lash_bounds[2], centres, 3, w=np.sqrt(weights[columns]))
        for name, is_lash in ((lash_name, True), ("eyewhite-" + side, False)):
            target = "mesh:" + mesh(name)
            bounds = frame(name)
            neutral = {parameter: 1, "ParamEyeSmile": 0}
            invoke("form", {"state": state, "changes": [{"op": "seed", "target": target, "key": neutral}]})
            for openness, smile in product((0, 0.04, 0.25, 0.5, 0.75, 1), (0, 1)):
                key = {parameter: openness, "ParamEyeSmile": smile}
                if key == neutral: continue
                invoke("form", {"state": state, "changes": [{"op": "copy", "target": target,
                    "from": neutral, "key": key, "channels": ["geometry"]}]})
                # Closed aperture and lid share a curve in canvas coordinates.
                # Happy eyes arch upward; ordinary blinks curve downward.
                x = np.linspace(0, 1, 20)
                global_x = bounds[0] + x * bounds[2]
                u = (global_x - lash_bounds[0]) / lash_bounds[2]
                closed = edge_y + (18 - 42 * smile) * 4 * u * (1 - u)
                pivot_y = bounds[1] + bounds[3] / 2
                thickness = 0.88 if is_lash else 0.001
                scale = openness + (1 - openness) * thickness
                anchor = np.polynomial.polynomial.polyval(u, source_line) if is_lash else pivot_y
                offset = (1 - openness) * (closed - thickness * anchor - (1 - thickness) * pivot_y) / bounds[3]
                controls = bezier_controls(np.polynomial.polynomial.polyfit(x, offset, 3))
                invoke("deform", {"state": state, "changes": [{"target": target, "key": key, "operations": [
                    {"type": "scale", "pivot": [0.5, 0.5], "factors": [1, float(scale)]},
                    {"type": "curve", "axis": "y", "controls": controls},
                ]}]})
        # The fully closed aperture is narrower than the opaque lash above it.
        # Both stay opaque throughout: no alpha crossfade or second closed eye.

    for target, name, parameter, shoulder, direction, reaches, swings in (
        (left_arm, "handwear-l", "ParamArmLA", [496, 643], 1, [0], [0]),
        (right_arm, "handwear-r", "ParamArmRA", [758, 643], -1, [0, 1], [0]),
        (open_hand, "handwear_r", "ParamArmRA", [758, 643], -1, [0, 1], [0]),
        (grip, "handwear.right", "ParamArmRA", [758, 643], -1, [0, 1], [0]),
        (prop, "handwear right", "ParamArmRA", [758, 643], -1, [0, 1], [-1, 0, 1]),
    ):
        bounds = frame(name)
        neutral = {parameter: 0}
        if len(reaches) > 1: neutral["ParamGrassReach"] = 0
        if len(swings) > 1: neutral["ParamGrassSwing"] = 0
        tips = [-1, 0, 1] if target == prop else [0]
        if target == prop: neutral["ParamGrassTipBend"] = 0
        invoke("form", {"state": state, "changes": [{"op": "seed", "target": "mesh:" + target, "key": neutral}]})
        for arm in (-65, -35, 0, 35, 65):
            for reach in reaches:
                for swing, tip_bend in product(swings, tips):
                    key = dict(neutral)
                    key[parameter] = arm
                    if len(reaches) > 1: key["ParamGrassReach"] = reach
                    if len(swings) > 1: key["ParamGrassSwing"] = swing
                    if target == prop: key["ParamGrassTipBend"] = tip_bend
                    if key == neutral: continue
                    # Every form begins at the same neutral mesh. Sampling a
                    # previously modified key would accumulate deformation.
                    invoke("form", {"state": state, "changes": [{"op": "copy", "target": "mesh:" + target,
                        "from": neutral, "key": key, "channels": ["geometry"]}]})
                    operations = []
                    # Local arcs preserve length and the grip. Bend the tip
                    # first, then the whole upper stem, then move with the hand.
                    # No sway key rotates the grass as a single rigid object.
                    if tip_bend:
                        operations.append({"type": "arc", "root": normalized([884, 669], bounds),
                            "tip": normalized([806, 498], bounds), "root_pin": 0.08, "degrees": tip_bend * 32})
                    if swing:
                        operations.append({"type": "arc", "root": normalized([913, 852], bounds),
                            "tip": normalized([806, 498], bounds), "root_pin": 0.12, "degrees": swing * 65})
                    if reach and target == prop:
                        # Keep the grass beside the face as the wrist lifts;
                        # the tilt pivots inside the grip, preserving occlusion.
                        operations.append({"type": "rotate", "pivot": normalized([913, 852], bounds), "degrees": 55})
                    if target in (left_arm, right_arm):
                        cuff = [399, 802] if target == left_arm else [855, 802]
                        arm_selection = {"center": normalized(cuff, bounds), "radius": 0.8, "hardness": 0.6}
                        # Full rigid motion at the cuff matches the palm/prop;
                        # zero weight near the shoulder prevents a second puff
                        # sleeve appearing when the gesture becomes large.
                        operations.append({"type": "rotate", "pivot": normalized(shoulder, bounds),
                            "degrees": arm * direction, "selection": arm_selection})
                    else:
                        operations.append({"type": "rotate", "pivot": normalized(shoulder, bounds), "degrees": arm * direction})
                    if reach:
                        scaling = {"type": "scale", "pivot": normalized(shoulder, bounds), "factors": [1.45, 1.45]}
                        if target == right_arm: scaling["selection"] = arm_selection
                        operations.append(scaling)
                        translation = {"type": "translate", "delta": [-240 / bounds[2], -85 / bounds[3]]}
                        if target == right_arm:
                            # Move the wrist towards the viewer while pinning
                            # the shoulder, rather than translating the sleeve.
                            translation["selection"] = arm_selection
                        operations.append(translation)
                    invoke("deform", {"state": state, "changes": [{"target": "mesh:" + target, "key": key, "operations": operations}]})
    binding = invoke("inspect", {"target": "mesh:" + prop})
    if not any("ParamGrassVisible" in channel.get("axes", {}) for channel in binding.get("channels", [])):
        raise RuntimeError("Grass visibility is not bound to drawable opacity")
    for target in (open_hand, grip):
        binding = invoke("inspect", {"target": "mesh:" + target})
        if not any("ParamHandRGrip" in channel.get("axes", {}) for channel in binding.get("channels", [])):
            raise RuntimeError("Open/gripping hands must share a native opacity parameter")
    eye_bindings = {}
    for name in ("eyelash-l", "eyelash-r", "eyewhite-l", "eyewhite-r", "irides-l", "irides-r"):
        binding = invoke("inspect", {"target": "mesh:" + mesh(name)})
        if binding.get("channels"):
            raise RuntimeError("Eye material channels must stay unanimated: " + name)
        eye_bindings[name] = binding
    (OUTPUT / "diagnostics/eye-bindings.json").write_text(json.dumps(eye_bindings, ensure_ascii=False, indent=2), encoding="utf8")
    result = invoke("export", {"state": state, "output_directory": str(OUTPUT)})
    (OUTPUT / "diagnostics/export.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf8")
    # CMO3 and the layered PSD are the editable source artifacts. MCP archive
    # save needs a GUI-selected path, so do not overwrite an unrelated archive.
    print("Model staged at", OUTPUT)


if __name__ == "__main__":
    main()
