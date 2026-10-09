"""Whale-girl head candidate r2 (texture-reuse route, round 2).

Depth-correct SINGLE-SURFACE head (round 1 read as "hair helmet over a
recessed face ball" - rejected): the painted face IS the front of the skull.
One closed surface carries the projected PSD face texture on its front cap
and the hair gradient everywhere else; bangs/side locks are closed sweep
tubes lying proud of that same surface and overlapping the face rim, so the
depth relation is hair-over-face as in the R19 art. Fold audits inherited
from the r1 sweep machinery keep long hair hole-free.

Run: D:/tool/blender/blender.exe --background --factory-startup --python tools/create_whale_head_candidate_r2.py
"""
from __future__ import annotations

import importlib.util
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


proto = load_module("toon_author", "tools/create_toon_prototype.py")
r1 = load_module("whale_head_r1", "tools/create_whale_head_candidate.py")
# r1's module-level import created a second proto instance; rebind all r1
# machinery onto this script's proto so MATERIALS/MESHES registries are shared.
r1.proto = proto

MAP = json.loads((ROOT / "art/3d/whale-girl-head-r2/face-map.json").read_text())
SS = 618.46                      # px per model unit (head crop 804px <-> 1.30 units)
# Visual calibration (grid overlay): painted eye-line midpoint px <-> model point.
ANCHOR_PX = (585.0, 445.0)       # eye-line midpoint in PSD px
EYE_Z = 1.66                     # model z of the eye line
FZC = 1.63                       # face ellipsoid center z (slightly below eye line)
FACE_RX = 0.26                   # painted skin oval half-width ~142px -> 0.230 + margin
FACE_RZ = 0.20
FACE_CENTER = (0.0, -0.02, FZC)
FACE_RADII = (FACE_RX, 0.20, FACE_RZ)
# The skull: hair-colored surface around the face cap. Front is flattened so
# the hair rim sits only a hair's width proud of the face plane.
SKULL_CENTER = (0.0, 0.05, 1.72)
SKULL_RADII = (0.55, 0.46, 0.48)

HEAD_OBJECTS = []


def clamp(v, lo=0.0, hi=1.0):
    return max(lo, min(hi, v))


def mesh_object_r2(name, vertices, faces, material, gradient=False):
    obj = r1.mesh_object(name, vertices, faces, material)
    if gradient:
        attribute = obj.data.color_attributes.new(name="WhaleHairGradient", type="FLOAT_COLOR", domain="POINT")
        obj.data.color_attributes.active_color = attribute
        obj.data.color_attributes.render_color_index = 0
        for idx, vertex in enumerate(obj.data.vertices):
            attribute.data[idx].color = r1.hair_gradient(vertex.co.z)
    HEAD_OBJECTS.append(obj)
    return obj


def skull_point(d):
    """Skull ellipsoid; front is flattened so hair sits proud of the face,
    and the front-bottom (jaw) recedes behind the face ball."""
    dx, dy, dz = d
    front = clamp((-dy - 0.2) / 0.6)
    lower = clamp((-dz + 0.1) / 0.5)
    ry = SKULL_RADII[1] - 0.20 * front - 0.16 * front * lower
    return Vector((SKULL_CENTER[0] + SKULL_RADII[0] * dx,
                   SKULL_CENTER[1] + ry * dy,
                   SKULL_CENTER[2] + SKULL_RADII[2] * dz))


def face_point(d):
    # Lower-face taper (V chin), matching round 1's calibrated face volume.
    taper = 1.0 - 0.18 * max(-d[2], 0.0) ** 1.7
    return Vector((FACE_CENTER[0] + FACE_RADII[0] * taper * d[0],
                   FACE_CENTER[1] + FACE_RADII[1] * d[1],
                   FACE_CENTER[2] + FACE_RADII[2] * d[2]))


def face_zone_weight(pf):
    """1 on the painted-face cap, fading to 0 across the hairline rim.
    Bottom extends to the face-ball pole (painted chin z~1.545 must stay skin)."""
    ix = clamp((0.250 - abs(pf.x)) / 0.045)
    iz = clamp((pf.z - 1.44) / 0.05) * clamp((1.79 - pf.z) / 0.05)
    iy = clamp(-pf.y / 0.12)
    return ix * iz * iy


def head_surface_point(d):
    """One surface for the whole head: face cap blends into the skull across
    the rim, so hair and skin are the same continuous skin (depth correct)."""
    pf = face_point(d)
    return skull_point(d).lerp(pf, face_zone_weight(pf))


def _ell_front_y(center, radii, ry, x, z):
    k = 1.0 - (x / radii[0]) ** 2 - ((z - center[2]) / radii[2]) ** 2
    if k <= 1e-4:
        return None
    return center[1] - ry * math.sqrt(k)


def surface_front_y(x, z):
    """Front-surface y of the blended head at planar (x, z)."""
    ys = _ell_front_y(SKULL_CENTER, SKULL_RADII, SKULL_RADII[1] - 0.20, x, z)
    yf = _ell_front_y(FACE_CENTER, FACE_RADII, FACE_RADII[1], x, z)
    if yf is not None:
        w = face_zone_weight(Vector((x, yf, z)))
        if ys is None:
            return yf
        return ys * (1.0 - w) + yf * w
    return ys


def surface_back_y(x, z):
    k = 1.0 - (x / SKULL_RADII[0]) ** 2 - ((z - SKULL_CENTER[2]) / SKULL_RADII[2]) ** 2
    if k <= 1e-4:
        return None
    return SKULL_CENTER[1] + SKULL_RADII[1] * math.sqrt(k)


def seat_on_skull(x, z, offset=0.022):
    """Point sitting `offset` proud of the skull surface (worn items)."""
    y = surface_front_y(x, z)
    if y is None:
        return Vector((x, 0.0, z))
    p = Vector((x, y, z)) - Vector(SKULL_CENTER)
    return Vector(SKULL_CENTER) + p + p.normalized() * offset


def make_head(segments=72, rings=48):
    vertices = []
    for ring in range(rings + 1):
        phi = math.pi * ring / rings
        for seg in range(segments):
            theta = math.tau * seg / segments
            d = (math.sin(phi) * math.cos(theta),
                 math.sin(phi) * math.sin(theta),
                 math.cos(phi))
            p = head_surface_point(d)
            vertices.append((p.x, p.y, p.z))
    faces = []
    for ring in range(rings):
        for seg in range(segments):
            a = ring * segments + seg
            b = ring * segments + (seg + 1) % segments
            faces.append((a, b, b + segments, a + segments))
    # Slot 0 = hair gradient, slot 1 = projected face texture. One mesh, no
    # recessed inner face: the face material paints the front of the skull.
    obj = mesh_object_r2("Head", vertices, faces, "WhaleHairGradient", gradient=True)
    obj.data.materials.append(proto.MATERIALS["WhaleFaceTex"])
    uv = obj.data.uv_layers.new(name="FaceProject")
    uv.active_render = True
    for poly in obj.data.polygons:
        if face_zone_weight(poly.center) > 0.5:
            poly.material_index = 1
        for li in poly.loop_indices:
            co = obj.data.vertices[obj.data.loops[li].vertex_index].co
            u = (ANCHOR_PX[0] + co.x * SS) / 1254.0
            v = (1254.0 - ANCHOR_PX[1] + (co.z - EYE_Z) * SS) / 1254.0
            uv.data[li].uv = (u, v)
    return obj


def lock(name, controls, widths, depths):
    return r1.sweep(name, controls, widths, depths, hair_weight=False, gradient=True)


def make_locks():
    waves = []
    # Back mass: five long closed locks STARTING on the skull's back surface
    # (round 1 buried them inside the helmet shell), then falling free.
    for idx, x0 in enumerate((-0.38, -0.20, 0.0, 0.20, 0.38)):
        x1 = x0 + (0.10 if idx % 2 else -0.10)
        x2 = x0 - (0.06 if idx % 2 else -0.06)
        y0 = surface_back_y(x0 * 0.6, 2.00)
        y1 = surface_back_y(x0, 1.72)
        y2 = surface_back_y(x1, 1.45)
        waves.append(lock(f"HairBack{idx}",
                          [(x0 * 0.6, (y0 if y0 is not None else 0.34) + 0.015, 2.00),
                           (x0, (y1 if y1 is not None else 0.48) + 0.02, 1.72),
                           (x1, (y2 if y2 is not None else 0.40) + 0.035, 1.45),
                           (x2, 0.30, 1.15), (x1 * 1.1, 0.26, 0.92)],
                          [0.09, 0.15, 0.15, 0.11, 0.05],
                          [0.10, 0.18, 0.18, 0.13, 0.06]))
    # Outer side locks framing the face, hugging the skull on the way down.
    for side in (-1, 1):
        y_top = surface_front_y(side * 0.42, 1.95)
        waves.append(lock(f"HairSideOuter{'L' if side < 0 else 'R'}",
                          [(side * 0.42, (y_top if y_top is not None else -0.06) - 0.015, 1.95),
                           (side * 0.56, -0.06, 1.70),
                           (side * 0.52, 0.08, 1.42), (side * 0.58, 0.02, 1.12),
                           (side * 0.50, 0.08, 0.98)],
                          [0.11, 0.16, 0.16, 0.12, 0.06],
                          [0.13, 0.19, 0.19, 0.14, 0.07]))
        y_in = surface_front_y(side * 0.30, 1.90)
        waves.append(lock(f"HairSideInner{'L' if side < 0 else 'R'}",
                          [(side * 0.30, (y_in if y_in is not None else -0.14) - 0.016, 1.90),
                           (side * 0.40, -0.10, 1.66),
                           (side * 0.38, -0.02, 1.44), (side * 0.41, 0.02, 1.30)],
                          [0.08, 0.12, 0.11, 0.05],
                          [0.10, 0.15, 0.14, 0.06]))
    # Two lower back locks, the longest (cyan tips).
    for side in (-1, 1):
        y_low = surface_back_y(side * 0.30, 1.70)
        waves.append(lock(f"HairBackLow{'L' if side < 0 else 'R'}",
                          [(side * 0.30, (y_low if y_low is not None else 0.42) + 0.02, 1.70),
                           (side * 0.38, 0.38, 1.40),
                           (side * 0.30, 0.32, 1.10), (side * 0.36, 0.30, 0.84)],
                          [0.10, 0.14, 0.12, 0.05],
                          [0.12, 0.17, 0.15, 0.06]))
    return waves


def make_bangs():
    bangs = []
    # Eleven tapered strands lying ON the head surface (16 mm proud: hair
    # OVERLAPS the face as in the art), tips following the painted bang
    # silhouette: center 1.72 between the eyes, side pieces down to 1.60.
    strands = (-0.30, -0.24, -0.18, -0.12, -0.06, 0.0, 0.06, 0.12, 0.18, 0.24, 0.30)
    for idx, x0 in enumerate(strands):
        tip_x = x0 + (0.025 if idx % 2 else -0.025)
        zig = 0.02 if idx % 2 else -0.01
        tip_z = 1.735 if x0 == 0.0 else (1.725 if abs(x0) <= 0.12 else
                                         (1.695 if abs(x0) <= 0.24 else 1.645))
        tip_z += zig
        pts = []
        for x, z in ((x0 * 0.85, 2.02), (x0 * 0.97, 1.88), (x0, 1.80),
                     (x0 * 1.02, tip_z + 0.06), (tip_x, tip_z)):
            y = surface_front_y(x, z)
            pts.append((x, (y if y is not None else -0.10) - 0.014, z))
        bangs.append(lock(f"Bang{idx}", pts,
                          [0.048, 0.056, 0.050, 0.026, 0.008],
                          [0.024, 0.030, 0.028, 0.016, 0.008]))
    # Side pieces hugging the face rim down to mouth height.
    for side in (-1, 1):
        pts = []
        for x, z in ((side * 0.24, 1.92), (side * 0.29, 1.78),
                     (side * 0.31, 1.62), (side * 0.30, 1.48)):
            y = surface_front_y(x, z)
            pts.append((x, (y if y is not None else -0.08) - 0.016, z))
        bangs.append(lock(f"BangSide{'L' if side < 0 else 'R'}", pts,
                          [0.040, 0.048, 0.044, 0.018],
                          [0.026, 0.032, 0.030, 0.014]))
    return bangs


def make_ahoge():
    return lock("Ahoge",
                [(0.0, 0.10, 2.20), (0.02, 0.16, 2.36), (0.08, 0.12, 2.46)],
                [0.045, 0.03, 0.008],
                [0.05, 0.032, 0.008])


def band_arc():
    pts = [(-0.50, 1.90), (-0.28, 2.06), (0.0, 2.16), (0.28, 2.06), (0.50, 1.90)]
    # Seat the band ON the skull surface along its normal, slightly proud.
    return [tuple(seat_on_skull(x, z)) for x, z in pts]


def make_headband():
    r1.sweep("MaidBand", band_arc(), [0.075] * 4 + [0.06], [0.030] * 5,
             material="WhaleLace", gradient=False)
    # Pleat scallops along the band: quadratic Bezier through the arc points.
    p0, p1, p2 = Vector(band_arc()[0]), Vector(band_arc()[1]), Vector(band_arc()[2])
    for idx in range(9):
        s = idx / 8.0
        if s <= 0.5:
            u = s * 2
            pos = (1 - u) ** 2 * p0 + 2 * (1 - u) * u * p1
        else:
            u = (s - 0.5) * 2
            pos = (1 - u) ** 2 * p1 + 2 * (1 - u) * u * p2
        pos.z -= 0.075
        verts, faces = [], []
        rings, segs = 6, 14
        for ring in range(rings + 1):
            phi = math.pi * ring / rings
            for seg in range(segs):
                th = math.tau * seg / segs
                verts.append((pos.x + 0.042 * math.sin(phi) * math.cos(th),
                              pos.y + 0.020 * math.sin(phi) * math.sin(th),
                              pos.z + 0.030 * math.cos(phi)))
        for ring in range(rings):
            for seg in range(segs):
                a = ring * segs + seg
                b = ring * segs + (seg + 1) % segs
                faces.append((a, b, b + segs, a + segs))
        mesh_object_r2(f"MaidPleat{idx}", verts, faces, "WhaleLace")


def make_bow():
    # Viewer-right side of the R19 art = model -X.
    base = seat_on_skull(-0.50, 1.92, offset=0.03)
    r1.sweep("BowLoopL", [tuple(base + Vector((-0.02, 0.0, 0.02))),
                          tuple(base + Vector((-0.13, 0.05, 0.10))),
                          tuple(base + Vector((-0.20, 0.02, 0.02))),
                          tuple(base + Vector((-0.13, 0.0, -0.06)))],
             [0.05, 0.055, 0.05], [0.025, 0.03, 0.025], material="WhaleRibbon", gradient=False)
    r1.sweep("BowLoopR", [tuple(base + Vector((0.0, 0.0, 0.10))),
                          tuple(base + Vector((-0.05, 0.06, 0.18))),
                          tuple(base + Vector((-0.08, 0.02, 0.10)))],
             [0.045, 0.05, 0.02], [0.025, 0.028, 0.015], material="WhaleRibbon", gradient=False)
    verts, faces = [], []
    rings, segs = 6, 14
    for ring in range(rings + 1):
        phi = math.pi * ring / rings
        for seg in range(segs):
            th = math.tau * seg / segs
            verts.append((base.x + 0.045 * math.sin(phi) * math.cos(th),
                          base.y + 0.035 * math.sin(phi) * math.sin(th),
                          base.z + 0.045 * math.cos(phi)))
    for ring in range(rings):
        for seg in range(segs):
            a = ring * segs + seg
            b = ring * segs + (seg + 1) % segs
            faces.append((a, b, b + segs, a + segs))
    mesh_object_r2("BowKnot", verts, faces, "WhaleRibbon")


def make_fin_ears():
    # R19: fin ears sit at EYE height, swept back and down, half-buried.
    for side in (-1, 1):
        obj = proto.ellipsoid(f"FinEar{'L' if side < 0 else 'R'}",
                              (side * 0.50, 0.06, 1.68), (0.17, 0.045, 0.085),
                              "head", "WhaleFinOuter", 28, 12)
        obj.rotation_euler = (0.10, -side * 0.45, side * -0.10)
        HEAD_OBJECTS.append(obj)
        inner = proto.ellipsoid(f"FinEarInner{'L' if side < 0 else 'R'}",
                                (side * 0.52, 0.07, 1.676), (0.115, 0.024, 0.054),
                                "head", "WhaleFinInner", 24, 10)
        inner.rotation_euler = (0.10, -side * 0.45, side * -0.10)
        HEAD_OBJECTS.append(inner)


def build_r2_head():
    make_head()
    make_locks()
    make_bangs()
    make_ahoge()
    make_headband()
    make_bow()
    make_fin_ears()


def setup_render(scene):
    scene.render.resolution_x = 640
    scene.render.resolution_y = 640
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except Exception:
        scene.render.engine = "BLENDER_EEVEE"
    key = bpy.data.lights.new("Key", "SUN"); key.energy = 6.0
    key_obj = bpy.data.objects.new("Key", key); bpy.context.collection.objects.link(key_obj)
    key_obj.rotation_euler = (Vector((0, -0.8, -0.6))).to_track_quat("-Z", "Y").to_euler()
    fill = bpy.data.lights.new("Fill", "SUN"); fill.energy = 1.2
    fill_obj = bpy.data.objects.new("Fill", fill); bpy.context.collection.objects.link(fill_obj)
    fill_obj.rotation_euler = (Vector((-0.9, -0.4, -0.2))).to_track_quat("-Z", "Y").to_euler()
    rim = bpy.data.lights.new("Rim", "SUN"); rim.energy = 2.0
    rim_obj = bpy.data.objects.new("Rim", rim); bpy.context.collection.objects.link(rim_obj)
    rim_obj.rotation_euler = (Vector((0.2, 0.8, 0.5))).to_track_quat("-Z", "Y").to_euler()
    world = bpy.data.worlds.get("World") or bpy.data.worlds.new("World")
    bpy.context.scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (0.85, 0.88, 0.95, 1.0)
    bg.inputs["Strength"].default_value = 0.35
    cam = bpy.data.cameras.new("Cam"); cam.type = "ORTHO"; cam.ortho_scale = 1.55
    cam_obj = bpy.data.objects.new("Cam", cam); bpy.context.collection.objects.link(cam_obj)
    scene.camera = cam_obj
    return cam_obj


def render_views(cam_obj, out_dir):
    target = Vector((0.0, 0.0, 1.66))
    views = {"front": 0.0, "left35": math.radians(35), "right35": math.radians(-35)}
    outputs = {}
    engine_used = bpy.context.scene.render.engine
    for name, angle in views.items():
        pos = target + Vector((math.sin(angle) * 3.0, -math.cos(angle) * 3.0, 0.0))
        cam_obj.location = pos
        cam_obj.rotation_euler = (target - pos).to_track_quat("-Z", "Y").to_euler()
        bpy.context.scene.render.filepath = str(out_dir / f"round1-{name}.png")
        try:
            bpy.ops.render.render(write_still=True)
        except Exception as exc:
            if "CYCLES" not in engine_used:
                print(f"EEVEE render failed ({exc}); falling back to CYCLES CPU")
                bpy.context.scene.render.engine = "CYCLES"
                bpy.context.scene.cycles.device = "CPU"
                bpy.context.scene.cycles.samples = 32
                engine_used = "CYCLES"
                bpy.ops.render.render(write_still=True)
            else:
                raise
        outputs[name] = out_dir / f"round1-{name}.png"
    return outputs


def main():
    source = ROOT / "art/3d/whale-girl-head-r2"
    export = ROOT / "assets/3d/whale-girl-head-r2"
    source.mkdir(parents=True, exist_ok=True); export.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.object.select_all(action="SELECT"); bpy.ops.object.delete(use_global=False)
    for action in list(bpy.data.actions): bpy.data.actions.remove(action)
    proto.MESHES.clear(); proto.MATERIALS.clear(); proto.REST.clear(); r1.SWEEP_AUDITS.clear()
    scene = bpy.context.scene
    scene.render.fps = 30; scene.frame_start = 1; scene.frame_end = 60
    arm = proto.build_rig(); proto.build_geometry(); r1.remove_proxy_head()
    palette = {"WhaleSkin": "FFE8DE", "WhaleHairRoyal": "4563A5", "WhaleHairMid": "5E9AD5",
               "WhaleHairCyan": "70C7EF", "WhaleLace": "FFF9FF", "WhaleRibbon": "49AFE9",
               "WhaleFinOuter": "E8F4FB", "WhaleFinInner": "A8D8F0", "WhaleHairGradient": "FFFFFF",
               "WhaleEyeLash": "1D2949", "WhaleEyeBrow": "5D526F", "WhaleEyeWhite": "FFF8FB",
               "WhaleIrisDeep": "3C4F92", "WhaleIrisBlue": "4E7EC7", "WhaleIrisGlow": "7BCDF5",
               "WhalePupil": "223B71", "WhaleHighlight": "FFFFFF", "WhaleMouth": "8C4556",
               "WhaleEyeBlush": "F3B6B7", "WhaleFaceTex": "FFE8DE"}
    for name, color in palette.items():
        proto.material(name, r1.linear_hex(color))
    face_mat = proto.MATERIALS["WhaleFaceTex"]
    image = bpy.data.images.load(str(ROOT / "art/3d/whale-girl-head-r2/face-project.png"))
    tex = face_mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = image
    bsdf = face_mat.node_tree.nodes["Principled BSDF"]
    face_mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    for spec_name in ("Specular IOR Level", "Specular"):
        if spec_name in bsdf.inputs:
            bsdf.inputs[spec_name].default_value = 0.0
            break
    bsdf.inputs["Roughness"].default_value = 1.0
    gradient_material = proto.MATERIALS["WhaleHairGradient"]
    color_node = gradient_material.node_tree.nodes.new("ShaderNodeVertexColor")
    color_node.layer_name = "WhaleHairGradient"
    gradient_material.node_tree.links.new(color_node.outputs["Color"],
                                          gradient_material.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
    build_r2_head()
    for obj in list(bpy.context.scene.objects):
        if obj.type == "MESH":
            obj.parent = arm
            modifier = obj.modifiers.new("WhaleCandidateSkin", "ARMATURE"); modifier.object = arm
    animations = proto.make_actions(arm)
    arm.animation_data.action = bpy.data.actions["idle"]; scene.frame_set(1)
    weights = []; unweighted = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH": continue
        for vertex in obj.data.vertices:
            weights.append(abs(sum(g.weight for g in vertex.groups) - 1))
            if not vertex.groups: unweighted.append([obj.name, vertex.index])
    if unweighted or max(weights) > 1e-5: raise RuntimeError("Bad skin weights")
    blend = source / "character.blend"; glb = export / "character.glb"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    options = {"export_format": "GLB", "export_yup": True, "export_animations": True,
               "export_animation_mode": "ACTIONS", "export_def_bones": True,
               "export_morph": False, "export_skins": True, "export_all_influences": False,
               "export_optimize_animation_size": False, "export_apply": False,
               "export_cameras": False, "export_lights": False,
               "export_texcoords": True, "export_normals": True, "export_tangents": True,
               "export_vertex_color": "ACTIVE", "export_all_vertex_colors": False}
    bpy.ops.export_scene.gltf(filepath=str(glb), **options)
    cam_obj = setup_render(scene)
    outputs = render_views(cam_obj, source)
    report = {"blend": str(blend), "glb": str(glb),
              "renders": {k: str(v) for k, v in outputs.items()},
              "sweep_audits": r1.SWEEP_AUDITS,
              "max_weight_sum_error": max(weights),
              "face_anchor": {"ss_px_per_unit": SS, "anchor_px": list(ANCHOR_PX),
                              "eye_z": EYE_Z, "face_radii": list(FACE_RADII),
                              "face_center_model": list(FACE_CENTER)},
              "animations": animations}
    (source / "round1-report.json").write_text(json.dumps(report, indent=1))
    print("WHALE_HEAD_R2_ROUND1_COMPLETE " + json.dumps(
        {"blend": str(blend), "glb": str(glb), "renders": len(outputs),
         "sweeps": len(r1.SWEEP_AUDITS)}))


if __name__ == "__main__":
    main()
