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

    # 2. parameters (ranges mirror R19)
    P = [("ParamArmRA", "手臂 R", -65, 65), ("ParamArmLA", "手臂 L", -65, 65),
         ("ParamElbowRA", "肘 R", -35, 55), ("ParamElbowLA", "肘 L", -35, 55),
         ("ParamWristRA", "手腕 R", -25, 25),
         ("ParamEyeSmile", "眼笑", 0, 1), ("ParamCheek", "腮红", 0, 1),
         ("ParamSmileOpen", "笑口", 0, 1), ("ParamMouthGape", "张嘴", 0, 1),
         ("ParamHandRGrip", "握持手", 0, 1), ("ParamGrassVisible", "草显示", 0, 1)]
    for pid, nm, mn, mx in P:
        call("parameter", {"request": {"mode": "create", "parameter_id": pid, "state": state(),
                                       "name": nm, "min": mn, "max": mx, "default": 0}})
    print("params added:", len(P))

    # 3. rigs: upper arms (shoulder) + hands (wrist) + forearms (elbow) via rig
    def rig(name, meshes):
        t = txt(call("rig", {"state": state(), "name": name, "targets": meshes}))
        return t.get("target")

    up = {"R": rig("RigUpperarmR", ["mesh:ArtMeshLayerR"]), "L": rig("RigUpperarmL", ["mesh:ArtMeshLayerL"])}
    fore = {"R": rig("RigForearmR", ["mesh:ArtMeshHandwearR3", "mesh:ArtMeshHandwearR2", "mesh:ArtMeshHandwearR"]),
            "L": rig("RigForearmL", ["mesh:ArtMeshHandwearL"])}
    hand = {"R": rig("RigHandR", ["mesh:ArtMeshHandwearR", "mesh:ArtMeshHandwearR2"])}
    print("rigs:", up, fore, hand)

    # 4. keyforms
    def rot(target, key, deg, pivot):
        return {"target": target, "key": key, "operations": [{"type": "rotate", "degrees": deg, "pivot": pivot}]}

    ch = []
    for side, pid, piv, sgn in [("R", "ParamArmRA", [0.68, 0.38], -1), ("L", "ParamArmLA", [0.32, 0.38], 1)]:
        for v, deg in [(65.0, 35.0 * sgn), (-65.0, -35.0 * sgn)]:
            ch.append(rot(up[side], {pid: v}, deg, piv))                       # upper arm about shoulder
            ch.append(rot(fore[side], {pid: v, ("ParamElbowR" if side == "R" else "ParamElbowL") + "A": 0.0}, deg, piv))
    for v, deg in [(55.0, -45.0), (-35.0, 30.0)]:                              # right elbow
        ch.append(rot(fore["R"], {"ParamElbowRA": v}, deg, [0.68, 0.42]))
    for v, deg in [(55.0, 45.0), (-35.0, -30.0)]:                              # left elbow
        ch.append(rot(fore["L"], {"ParamElbowLA": v}, deg, [0.32, 0.42]))
    for v, deg in [(25.0, -25.0), (-25.0, 25.0)]:                              # right wrist
        ch.append(rot(hand["R"], {"ParamWristRA": v}, deg, [0.70, 0.55]))
    call("deform", {"state": state(), "changes": ch})
    print("deform keys:", len(ch))

    # 5. hide the alternate right hand (grip) and the grass so only the plain
    #    hand shows and nothing overlaps. NOTE: appearance ids are BARE (no
    #    "mesh:" prefix), and "handwear right" (grass-prop) is the fist+grass.
    call("appearance", {"state": state(), "edits": [
        {"action": "visibility", "kind": "mesh", "id": "ArtMeshHandwearR", "visible": False},
        {"action": "visibility", "kind": "mesh", "id": "ArtMeshLayer", "visible": False},
    ]})
    print("hid grip hand + grass")

    # 6. export
    os.makedirs(OUT, exist_ok=True)
    r, _ = c.call("tools/call", {"name": "export", "arguments": {"state": state(), "output_directory": OUT}}, 11, S)
    print("export ok:", os.path.exists(os.path.join(OUT, "Generated_artwork.moc3")), OUT)


if __name__ == "__main__":
    main()
