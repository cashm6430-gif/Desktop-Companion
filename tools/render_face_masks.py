"""T7: how much of the face does the hair hide, seen from the front?

The silhouette objective cannot see this at all. Hair falling *in front of* the
face adds exactly the same outline as hair falling beside it, so the first
fitted candidate bought its side-view gain by hanging the new locks over the
face and the score never noticed. This tool measures the missing axis directly.

It runs on the *authoring blend*, not on an exported GLB: `join_for_export()`
merges the ~75 named parts into a single mesh, so an exported file has no `Head`
to isolate and no hair to hide. The blend is the last point where the parts are
still separable, and `join` does not alter geometry, so what is measured here is
what the silhouette renderer sees.

Method: per-pixel part identity, read straight out of the depth buffer.

  * every part gets a unique flat colour via `Object.color`, rendered with
    Workbench `color_type='OBJECT'` and `light='FLAT'` -- so the pixel value is
    the identity of the *front-most* surface at that pixel, no shading;
  * the pass is written as 32-bit float EXR, which bypasses the display
    transform and the 8-bit quantisation that would otherwise make adjacent
    codes collide;
  * `face_pixels` = pixels whose front-most part is `Head` in the pass with all
    hair hidden -- i.e. the face as it would look with no hair at all;
  * occluded(group) = share of `face_pixels` whose front-most part is that group;
  * `occlusion_added_new` = share of `face_pixels` that is hair in the pass
    *with* the fitting layers and face in the pass *without* them. That is the
    number the constraint uses: a duplicated lock landing squarely on top of the
    lock it was copied from changes the front-most part without hiding one extra
    pixel of face, and only the increment can tell the two apart.

The first attempt used silhouette set arithmetic (`base AND NOT base+group`)
and returned 0 for every group, because a union silhouette can only ever grow
when geometry is added. Depth is the missing ingredient; V7c below is the
positive control that keeps that class of bug from shipping again.

`Head` is a single closed mesh (face cap + skull), so its front-facing pixels
are the face. Its skull top is hair-covered by design, in the shipped model as
much as in a candidate, which is why the shipped model is measured first and the
gate is expressed relative to it rather than as an absolute number.

Run:
  blender --background --factory-startup --python tools/render_face_masks.py -- \
      <blend> <out_dir> [<params.json>]
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import bpy
from mathutils import Vector
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import render_silhouette as rs  # noqa: E402
import whale_fit_deform as deform  # noqa: E402
import face_overlay as fo  # noqa: E402

VIEW = "neg_y"                      # the rig faces -Y, so neg_y is the front
RES = rs.RES
CODE_SCALE = 256.0                  # codes are k/256 for k = 1..len(parts)

GROUP_RULES: dict[str, tuple[str, ...]] = {
    "bangs": ("Bang",),
    "locks": ("HairBack", "HairSide"),
    "ahoge": ("Ahoge",),
    "added_front": ("FitAdd_front",),
    "added_side": ("FitAdd_side",),
    "added_back": ("FitAdd_back",),
}
ADDED_GROUPS = ("added_front", "added_side", "added_back")


def argv_after_ddash() -> list[str]:
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def prune_preview_shells() -> int:
    """Drop the outline shells, exactly as join_for_export() does.

    The authoring scene carries a `WhaleOutlineShell` modifier that inflates a
    copy of every part for the preview outline. The export removes it (the
    engine's toon shader draws the outline), and leaving it in would put an
    inflated copy of the *head* in front of the face, inventing occlusion the
    exported model does not have.
    """
    removed = 0
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        for mod in list(obj.modifiers):
            if mod.name == "WhaleOutlineShell":
                obj.modifiers.remove(mod)
                removed += 1
    return removed


def render_ids(cam, center, dist, visible: set[str], meshes, out_exr: Path) -> None:
    for obj in meshes:
        obj.hide_render = obj.name not in visible
    dirx, diry, _, rot = rs.VIEWS[VIEW]
    cam.location = (center.x + dirx * dist, center.y + diry * dist, center.z)
    cam.rotation_euler = rot
    scene = bpy.context.scene
    scene.render.resolution_x = RES
    scene.render.resolution_y = RES
    scene.render.film_transparent = True
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    # Absolute: Blender resolves a relative render filepath against the blend's
    # own directory and fails *silently* (no file, no error) -- a trap this
    # project has already paid for once.
    scene.render.filepath = str(Path(out_exr).resolve())
    bpy.ops.render.render(write_still=True)


def load_ids(path: Path, count: int) -> np.ndarray:
    """Per-pixel part code (1..count), -1 where the background shows.

    The code is read straight back out of the float R channel: codes are k/256
    for k in 1..count, so the value * 256 is the code. 1-based on purpose --
    the background renders as an exact 0, which would otherwise collide with
    the first part.
    """
    img = bpy.data.images.load(str(Path(path).resolve()), check_existing=False)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
    bpy.data.images.remove(img)
    px = px[::-1]                                   # Blender stores bottom-up
    raw = px[..., 0] * CODE_SCALE
    code = np.rint(raw)
    ok = ((px[..., 3] > 0.5) & (code >= 1.0) & (code <= count)
          & (np.abs(raw - code) < 0.10))
    return np.where(ok, code, -1.0).astype(np.int16)


def main() -> int:
    args = argv_after_ddash()
    debug = "--debug" in args
    args = [a for a in args if not a.startswith("--")]
    if len(args) < 2:
        print("usage: -- <blend> <out_dir> [<params.json>] [--debug]")
        return 2
    blend, out_dir = Path(args[0]), Path(args[1])
    params_path = Path(args[2]) if len(args) > 2 else None

    if blend.suffix.lower() != ".blend":
        print(f"V7a FAIL: {blend.name} is not an authoring blend; the parts cannot "
              f"be isolated in an exported GLB (join_for_export makes one mesh)")
        return 1
    bpy.ops.wm.open_mainfile(filepath=str(blend.resolve()))

    scene = bpy.context.scene
    if hasattr(scene, "use_nodes"):
        scene.use_nodes = False                 # no compositor on a measuring pass
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "FLAT"
    scene.display.shading.color_type = "OBJECT"
    scene.display.shading.show_object_outline = False
    scene.display.shading.show_shadows = False
    scene.display.shading.show_cavity = False
    scene.display.render_aa = "OFF"             # no blended colours at the edges
    scene.view_settings.view_transform = "Standard"

    shells = prune_preview_shells()
    params = json.loads(params_path.read_text()) if params_path else {}
    report = deform.apply(params)
    print(f"FACE shells_removed={shells} touched={len(report['touched'])} "
          f"created={len(report['created'])}")

    meshes = [o for o in scene.objects if o.type == "MESH"]
    names = sorted(o.name for o in meshes)
    if "Head" not in names:
        print(f"V7a FAIL: no `Head` mesh among {len(names)} parts")
        return 1

    # Codes must be unique and order-stable, and 1-based so that "no object"
    # (background, all channels 0) can never be mistaken for part #0.
    codes = {name: i + 1 for i, name in enumerate(names)}
    for obj in meshes:
        obj.color = (codes[obj.name] / CODE_SCALE, 0.0, 0.0, 1.0)
    count = len(names)

    mins, maxs = rs.scene_bbox()
    cam, center, dist = rs.setup_camera(mins, maxs)
    print(f"FACE parts={count} bbox x[{mins.x:.3f},{maxs.x:.3f}] "
          f"y[{mins.y:.3f},{maxs.y:.3f}] z[{mins.z:.3f},{maxs.z:.3f}]")

    groups = {g: [n for n in names if n.startswith(prefs)]
              for g, prefs in GROUP_RULES.items()}
    hair = sorted({n for g in groups.values() for n in g})
    non_hair = [n for n in names if n not in set(hair)]
    is_hair = np.zeros(count + 1, dtype=bool)
    for n in hair:
        is_hair[codes[n]] = True
    print(f"FACE hair={len(hair)} "
          f"groups={json.dumps({g: len(v) for g, v in groups.items()})}")

    face_dir = out_dir / "face"
    face_dir.mkdir(parents=True, exist_ok=True)

    def pass_ids(visible: list[str], tag: str) -> np.ndarray:
        exr = face_dir / f"id-{tag}.exr"
        render_ids(cam, center, dist, set(visible), meshes, exr)
        if not exr.exists():
            raise RuntimeError(f"render wrote no {exr}")
        idmap = load_ids(exr, count)
        np.save(face_dir / f"id-{tag}.npy", idmap.astype(np.int16))
        return idmap
    id_nohair = pass_ids(non_hair, "nohair")
    id_full = pass_ids(names, "full")
    # every non-added hair object, i.e. the model as it would look without the
    # layers the fitting deformation instantiates
    non_added = [n for n in hair if not n.startswith(deform.ADD_PREFIX)]
    id_no_added = pass_ids(non_hair + non_added, "no-added")
    if debug:
        by_code = {v: k for k, v in codes.items()}
        vals, cnts = np.unique(id_nohair, return_counts=True)
        order = np.argsort(-cnts)[:12]
        print("FACE debug: top codes in the no-hair pass")
        for v, c in zip(vals[order].tolist(), cnts[order].tolist()):
            nm = by_code.get(v, "?")
            obj = next((o for o in meshes if o.name == nm), None)
            bb = ""
            if obj is not None:
                pts = [obj.matrix_world @ Vector(cor) for cor in obj.bound_box]
                bb = (f"x[{min(p.x for p in pts):+.2f},{max(p.x for p in pts):+.2f}] "
                      f"z[{min(p.z for p in pts):+.2f},{max(p.z for p in pts):+.2f}]")
            print(f"    code {v:3d} {nm:<16} {c:8d}px  {bb}")
    group_ids = {g: (pass_ids(non_hair + members, g) if members else None)
                 for g, members in groups.items()}
    id_added = pass_ids(non_hair + [n for n in hair if n.startswith(deform.ADD_PREFIX)],
                        "added")

    head = codes["Head"]
    face = id_nohair == head
    area = int(face.sum())
    coverage = float(area) / float(id_nohair.size)
    print(f"FACE face_region={area}px ({100.0 * coverage:.2f}% of frame), "
          f"head visible without hair, hair parts={len(hair)}")
    if area <= 0:
        print("V7a FAIL: the face region is empty "
              "(no pixel shows the Head when the hair is hidden)")
        return 1

    def front_is_hair(idmap: np.ndarray) -> np.ndarray:
        return is_hair[np.clip(idmap, 0, count)]

    def occ(idmap: np.ndarray) -> float:
        return round(float((face & front_is_hair(idmap)).sum()) / area, 5)

    occlusion = {g: (occ(group_ids[g]) if group_ids[g] is not None else 0.0)
                 for g in groups}
    added = occ(id_added)
    total = occ(id_full)
    # The number the constraint actually needs. A duplicated lock that lands on
    # top of the lock it was copied from changes the front-most part without
    # hiding one extra pixel of face -- and the naive count above cannot tell
    # the two apart. So the increment is measured against the same model with
    # the fitting layers removed: pixels that were face there, and are hair now.
    exposed_before = face & ~front_is_hair(id_no_added)
    added_new = round(float((exposed_before & front_is_hair(id_added)).sum()) / area, 5)

    # The class map the approval sheet draws. Written as an array rather than a
    # PNG because Blender would put the pixels through a colour space on the way
    # out, and then the figure's colours would no longer match the palette the
    # legend is drawn from.
    classes = fo.classify(face, face & front_is_hair(id_full),
                          face & front_is_hair(id_added) & ~front_is_hair(id_no_added),
                          id_full >= 0)
    np.save(face_dir / "sem-classes.npy", classes)

    # V7c positive control: the shipped model's own fringe must cover part of the
    # face. A measurement that structurally returns 0 (which the silhouette-set
    # version did) fails here instead of silently gating every candidate at 0.
    control_ok = occlusion["bangs"] > 0.01
    print(f"V7c positive control: bangs occlude {occlusion['bangs']:.4f} of the face "
          f"(gate > 0.01) -> {'PASS' if control_ok else 'FAIL (the depth pass is not '
          'resolving parts)'}")
    # V7b: the group passes must explain the full pass. Summing the groups over
    # -counts where two hair groups overlap, so the sum is an upper bound and the
    # check is one-sided.
    residual = round(sum(occlusion.values()) - total, 5)
    partition_ok = residual >= -1e-4
    print(f"V7b partition: sum(groups)={sum(occlusion.values()):.4f} >= "
          f"total={total:.4f} (residual {residual:+.4f}) -> "
          f"{'PASS' if partition_ok else 'FAIL'}")

    failures = ([f for f, ok in (("V7b", partition_ok), ("V7c", control_ok)) if not ok])
    payload = {
        "blend": str(blend),
        "params": params,
        "view": VIEW,
        "face_region_px": area,
        "face_region_frac_of_frame": round(coverage, 5),
        "occlusion": occlusion,
        "occlusion_added": added,           # front-most part is a fitting layer
        "occlusion_added_new": added_new,   # ... and it was face before
        "occlusion_total": total,           # all hair
        "hair_parts": len(hair),
        "added_parts": len([n for n in hair if n.startswith(deform.ADD_PREFIX)]),
        "failures": failures,
    }
    (out_dir / "face-occlusion.json").write_text(json.dumps(payload, indent=1))
    print("FACE occlusion (fraction of the front-visible head covered by hair):")
    for g, v in occlusion.items():
        print(f"  {g:<12} {v:.4f}")
    print(f"  {'ADDED':<12} {added:.4f}   (front-most part is a fitting layer)")
    print(f"  {'ADDED_NEW':<12} {added_new:.4f}   <- face taken away by the fit, "
          f"must stay ~0")
    print(f"  {'TOTAL':<12} {total:.4f}")
    print("T7_FACE " + ("PASS (V7a..V7c)" if not failures
                        else "FAIL " + ",".join(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
