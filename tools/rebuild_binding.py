#!/usr/bin/env python3
"""Rebuild the whale-girl Live2D binding on top of the cleaned layers, via the
PSD2Live 2.0.2 MCP. One-shot + idempotent-ish: create the project, add the arm
and expression parameters, rig the arms (shoulder/elbow/wrist) and the hand-grip
switch, then export the model family.

Run with the uv cpython-3.12 interpreter while PSD2Live is open with an EMPTY
workspace:  python tools/rebuild_binding.py

The cleaned, trimmed standing layers are read from
.local/authoring/layers-trim-20261014/trim-manifest.json.
"""
import sys, os, json

sys.path.insert(0, "tools")
import psd2live_client as c

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRIM = os.path.join(ROOT, ".local", "authoring", "layers-trim-20261014")
OUT = os.path.join(ROOT, ".local", "authoring", "rebuild-full-export")

S = None


def call(name, args):
    global S
    r, S = c.call("tools/call", {"name": name, "arguments": args}, 7, S)
    return r


def state():
    r, _ = c.call("tools/call", {"name": "inspect", "arguments": {"scope": "project"}}, 9, S)
    return json.loads(r["result"]["content"][0]["text"])["state"]


def txt(r):
    try:
        return json.loads(r["result"]["content"][0]["text"])
    except Exception:
        return r


def main():
    global S
    r, S = c.initialize()
    # 1. create project from the cleaned standing layers
    recs = json.load(open(os.path.join(TRIM, "trim-manifest.json"), encoding="utf-8"))
    layers = [{"path": os.path.join(TRIM, x["file"]), "name": x["name"], "x": x["x"], "y": x["y"]}
              for x in recs if not any(k in x["name"].lower() for k in ("busy", "laptop", "workstation", "desk"))]
    t = txt(call("asset", {"request": {"mode": "create", "width": 1254, "height": 1254, "layers": layers}}))
    print("create:", t if "state" not in t else "ok layers=" + str(len(layers)))

    # 3. arm rig via the SKELETON: PSD2Live infers a bone armature (UPPER_ARM /
    #    FOREARM / HAND with joints + drawable bindings), we put it and the bake
    #    generates the arm parameters (ParamArmRA/RB/HandR, ...LA/LB/HandL).
    #    (The old rig+deform approach is abandoned: rotating a big Warp pivoted
    #    in place instead of lifting the arm.)
    spec = txt(call("skeleton", {"request": {"mode": "propose"}}))["spec"]
    call("skeleton", {"request": {"mode": "put", "state": state(), "spec": spec}})
    print("skeleton bones:", [b["id"] for b in spec["bones"]])

    # 4. expression parameters (ranges mirror R19); the arm params come from the
    #    skeleton bake.
    P = [("ParamEyeSmile", "眼笑", 0, 1), ("ParamCheek", "腮红", 0, 1),
         ("ParamSmileOpen", "笑口", 0, 1), ("ParamMouthGape", "张嘴", 0, 1)]
    for pid, nm, mn, mx in P:
        call("parameter", {"request": {"mode": "create", "parameter_id": pid, "state": state(),
                                       "name": nm, "min": mn, "max": mx, "default": 0}})
    print("expression params added:", len(P))

    # 5. hide the alternate right hand (grip) and the grass so only the plain
    #    hand shows and nothing overlaps. NOTE: appearance ids are BARE (no
    #    "mesh:" prefix), and "handwear right" (grass-prop) is the fist+grass.
    call("appearance", {"state": state(), "edits": [
        {"action": "visibility", "kind": "mesh", "id": "ArtMeshHandwearR", "visible": False},
        {"action": "visibility", "kind": "mesh", "id": "ArtMeshLayer", "visible": False},
    ]})
    print("hid grip hand + grass")

    # 6. export. Pad the atlas tiles so adjacent-tile bleed does not leave thin
    #    cracks/dots at part edges.
    call("settings", {"state": state(), "changes": {"texturePadding": 4}})
    os.makedirs(OUT, exist_ok=True)
    r, _ = c.call("tools/call", {"name": "export", "arguments": {"state": state(), "output_directory": OUT}}, 11, S)
    print("export ok:", os.path.exists(os.path.join(OUT, "Generated_artwork.moc3")), OUT)


if __name__ == "__main__":
    main()
