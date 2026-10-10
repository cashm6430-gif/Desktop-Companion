"""T3 runner: deform the r2/r3 authoring blend with a parameter set, verify,
then export a candidate GLB through the *same* join + glTF path r2 uses.

Run:
  blender --background --factory-startup \
      art/3d/whale-girl-head-r2/character-round3.blend \
      --python tools/fit_silhouette_template.py -- \
      [--params <params.json>] [--out <candidate.glb>] [--probe <param>] \
      [--report <report.json>]

Gates (printed as V3a / V3b / V3c):
  V3a identity -- with the neutral parameter set every vertex of every part must
      come back bit-identical (max |delta| == 0), i.e. the fitting space is
      anchored on the shipped mesh and cannot drift on its own
  V3b monotonicity -- raising a length/flare parameter must not shrink the
      measured extent in the axis it acts on (probe mode)
  V3c orientation -- no triangle may flip its normal; a sign flip would mean
      the deformation inverted the surface locally
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import whale_fit_deform as deform  # noqa: E402


def load_module(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def argv_after_ddash() -> list[str]:
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def parse_args(argv: list[str]) -> dict:
    opts: dict[str, str | None] = {"params": None, "out": None, "probe": None, "report": None}
    i = 0
    while i < len(argv):
        key = argv[i].lstrip("-")
        if key in opts and i + 1 < len(argv):
            opts[key] = argv[i + 1]
            i += 2
        else:
            i += 1
    return opts


def snapshot() -> dict[str, list[tuple[float, float, float]]]:
    snap = {}
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        mw = obj.matrix_world
        snap[obj.name] = [tuple(mw @ v.co) for v in obj.data.vertices]
    return snap


def max_delta(a, b) -> float:
    worst = 0.0
    for name, pts in a.items():
        for p, q in zip(pts, b.get(name, [])):
            for i in range(3):
                worst = max(worst, abs(p[i] - q[i]))
    return worst


def newell(pts, idx) -> tuple[float, float, float]:
    nx = ny = nz = 0.0
    n = len(idx)
    for i in range(n):
        x0, y0, z0 = pts[idx[i]]
        x1, y1, z1 = pts[idx[(i + 1) % n]]
        nx += (y0 - y1) * (z0 + z1)
        ny += (z0 - z1) * (x0 + x1)
        nz += (x0 - x1) * (y0 + y1)
    return nx, ny, nz


def face_normals(snap, cap=40000) -> dict[str, list[tuple[float, float, float]]]:
    """Newell normals computed from a position snapshot (cache-free)."""
    out = {}
    budget = cap
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or obj.name not in snap:
            continue
        pts = snap[obj.name]
        polys = obj.data.polygons
        step = max(1, len(polys) // max(1, budget))
        normals = []
        for poly in list(polys)[::step]:
            normals.append(newell(pts, list(poly.vertices)))
        out[obj.name] = normals
        budget -= len(normals)
    return out


def count_flips(before, after) -> int:
    flips = 0
    for name, na in after.items():
        nb = before.get(name, [])
        for a, b in zip(na, nb):
            dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
            mag_a = (a[0] ** 2 + a[1] ** 2 + a[2] ** 2) ** 0.5
            mag_b = (b[0] ** 2 + b[1] ** 2 + b[2] ** 2) ** 0.5
            if mag_a < 1e-12 or mag_b < 1e-12:
                continue
            if dot / (mag_a * mag_b) < 0.0:
                flips += 1
    return flips


def main() -> int:
    opts = parse_args(argv_after_ddash())
    failures: list[str] = []

    # ---------------------------------------------------------------- probe
    if opts["probe"]:
        params = list(deform.PARAM_SPEC) if opts["probe"] == "all" else [str(opts["probe"])]
        probes = {p: deform.probe(p) for p in params}
        weak = []
        for name, res in probes.items():
            rows = " | ".join(f"{r['factor']:.1f}:{r[res['key']]:.4f}" for r in res["rows"])
            good = res["monotone"] and res["responds"]
            print(f"  {name:<18} {res['key']:<8} dir={res['direction']:+d} "
                  f"delta={res['delta']:+.4f}  {rows}  -> {'OK' if good else 'WEAK'}")
            if not good:
                weak.append(name)
            # probe() restores from its own snapshot before returning
        ok = not weak
        print(f"V3b monotonicity: {len(params) - len(weak)}/{len(params)} parameters "
              f"respond monotonically -> {'PASS' if ok else 'FAIL ' + ','.join(weak)}")
        if opts["report"]:
            Path(opts["report"]).write_text(json.dumps(probes, indent=1))
        print("T3_PROBE " + ("PASS (V3b)" if ok else "FAIL (V3b)"))
        return 0 if ok else 1

    params = json.loads(Path(opts["params"]).read_text()) if opts["params"] else {}
    p = deform.neutral(params)
    print("PARAMS " + json.dumps(p))

    before = snapshot()
    before_normals = face_normals(before)

    # ------------------------------------------------------------- V3a identity
    report = deform.apply(params)
    after = snapshot()
    delta = max_delta(before, after)
    added = [n for n in after if n not in before]
    removed = [n for n in before if n not in after]
    identity_expected = deform.is_identity(params)
    if identity_expected:
        ok = delta == 0.0 and not added and not removed
        print(f"V3a identity: neutral params -> max vertex delta = {delta}, "
              f"added={len(added)} removed={len(removed)} ({len(before)} parts) -> "
              f"{'PASS' if ok else 'FAIL'}")
        if not ok:
            failures.append("V3a")
    else:
        moved = sum(1 for n in after
                    if n in before
                    and any(abs(a[i] - b[i]) > 1e-9
                            for a, b in zip(after[n], before[n]) for i in range(3)))
        print(f"V3a identity: params are non-neutral, {moved}/{len(before)} parts moved, "
              f"+{len(added)} created, max delta = {delta:.6f} (informational)")

    # ---------------------------------------------------------- V3d creation
    created = report.get("created", [])
    print(f"V3d creation: neutral={identity_expected} created={len(created)} "
          f"{created[:3]}{'...' if len(created) > 3 else ''} -> "
          f"{'PASS' if (not identity_expected or not created) else 'FAIL'}")
    if identity_expected and created:
        failures.append("V3d")

    # ---------------------------------------------------- V3e skin integrity
    # Added parts inherit the template's vertex groups, but that is exactly the
    # kind of thing worth asserting rather than assuming: an unweighted vertex
    # would tear away from the rig at the first animation frame.
    unweighted = 0
    worst_weight = 0.0
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        for vertex in obj.data.vertices:
            total = sum(g.weight for g in vertex.groups)
            if not vertex.groups:
                unweighted += 1
            worst_weight = max(worst_weight, abs(total - 1.0))
    ok = unweighted == 0 and worst_weight <= 1e-5
    print(f"V3e skin integrity: unweighted={unweighted} "
          f"max|sum(w)-1|={worst_weight:.2e} -> {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append("V3e")

    # ------------------------------------------------------- V3f ground guard
    # Every silhouette is normalised to the same total height, so any geometry
    # that reaches below the sole silently rescales the whole comparison -- a
    # candidate could "improve" by growing taller rather than by fitting. The
    # ground contact is therefore frozen at the shipped value.
    base_min_z = min(p[2] for pts in before.values() for p in pts)
    after_min_z = min(p[2] for pts in after.values() for p in pts)
    ok = after_min_z >= base_min_z - 1e-4
    print(f"V3f ground guard: shipped min z={base_min_z:.4f} -> deformed "
          f"{after_min_z:.4f} (delta {after_min_z - base_min_z:+.4f}) -> "
          f"{'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append("V3f")

    print(f"deform report: touched={len(report['touched'])} "
          f"created={len(report.get('created', []))} "
          f"skipped_neutral={len(report['skipped_neutral'])} verts={report['verts']}")

    # ---------------------------------------------------------- V3c orientation
    flips = count_flips(before_normals, face_normals(after))
    ok = flips == 0
    print(f"V3c orientation: flipped faces = {flips} -> {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append("V3c")

    # ------------------------------------------------- V5 tail integrity
    # A regression guard for the defect that started this line of work: the
    # generated previews grew a fluke on each side of the body. The authoring
    # template has exactly one tail, and every fitted export must keep it that
    # way. Two notes on the measurement, both learned by getting it wrong first:
    #   * the tail ROOT legitimately crosses the centre line (TailStem starts at
    #     x=-0.062, where it meets the pelvis), so only the FLUKES are required
    #     to be single-sided;
    #   * the stem and flukes are separate meshes that overlap in space without
    #     sharing vertices, so "connected" has to be tested by bounding-box
    #     overlap along X, not by vertex proximity.
    def tail_pts(prefix):
        objs = [o for o in bpy.context.scene.objects
                if o.type == "MESH" and o.name.startswith(prefix)]
        return [(o.matrix_world @ v.co) for o in objs for v in o.data.vertices]

    fpts = tail_pts("TailFluke")
    spts = tail_pts("TailStem")
    if not fpts or not spts:
        print(f"V5a tail presence: stem={len(spts)} fluke={len(fpts)} verts -> FAIL")
        failures.append("V5a")
    else:
        fx = [q.x for q in fpts]
        ok = min(fx) > 0.0
        print(f"V5a flukes single-sided: {len(fpts)} verts x[{min(fx):+.3f},{max(fx):+.3f}] "
              f"-> {'PASS' if ok else 'FAIL (a fluke on -X too)'}")
        if not ok:
            failures.append("V5a")

        stem_max = max(q.x for q in spts)
        ok = stem_max >= min(fx)
        print(f"V5b flukes attached: stem reaches x={stem_max:+.3f}, flukes start at "
              f"{min(fx):+.3f} -> {'PASS' if ok else 'FAIL (flukes float free)'}")
        if not ok:
            failures.append("V5b")

    # V5c: the created hair layers must match what the parameters asked for.
    asked = (int(round(p.get("side_lock_extra", 0))) + int(round(p.get("front_lock_extra", 0)))
             + int(round(p.get("back_lock_extra", 0))))
    made = len([o for o in bpy.context.scene.objects
                if o.name.startswith(deform.ADD_PREFIX)])
    ok = (asked == 0 and made == 0) or made == asked * 2      # two templates per layer
    print(f"V5c created layers: asked={asked} objects={made} -> {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append("V5c")

    # --------------------------------------------------------------- export
    if opts["out"]:
        r2 = load_module("whale_candidate_r2", "tools/create_whale_head_candidate_r2.py")
        out = Path(opts["out"])
        out.parent.mkdir(parents=True, exist_ok=True)
        joined = r2.join_for_export()
        stats = {"objects": 0, "tris": 0, "verts": 0}
        if joined is not None:
            joined.data.calc_loop_triangles()
            stats = {"objects": 1, "tris": len(joined.data.loop_triangles),
                     "verts": len(joined.data.vertices)}
        # Mirrors tools/create_whale_head_candidate_r2.py:main() so the fitted
        # candidate is byte-comparable with the shipped character-round3.glb.
        options = {"export_format": "GLB", "export_yup": True, "export_animations": True,
                   "export_animation_mode": "ACTIONS", "export_def_bones": True,
                   "export_morph": False, "export_skins": True, "export_all_influences": False,
                   "export_optimize_animation_size": False, "export_apply": False,
                   "export_cameras": False, "export_lights": False,
                   "export_texcoords": True, "export_normals": True, "export_tangents": True,
                   "export_vertex_color": "ACTIVE", "export_all_vertex_colors": False}
        bpy.ops.export_scene.gltf(filepath=str(out), **options)
        print(f"EXPORT {out} size={out.stat().st_size} mesh={stats}")

    payload = {"params": p, "identity": identity_expected, "max_vertex_delta": delta,
               "flipped_faces": flips, "deform": {k: v for k, v in report.items() if k != "params"},
               "glb": opts["out"], "failures": failures}
    if opts["report"]:
        Path(opts["report"]).write_text(json.dumps(payload, indent=1))
    print("T3_RUNNER " + ("FAIL " + ",".join(failures) if failures else "PASS (V3a,V3c..V3f,V5a..V5c)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
