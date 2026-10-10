"""Build a curved card from a painted hair layer.

Why this exists
---------------
Route B spent a deformation budget trying to make cloned locks add up to a
silhouette that a painting already specifies exactly. The painting is the
specification. If the geometry is built *from* the layer's alpha, the front
silhouette is correct by construction -- there is nothing to fit and nothing to
guess.

What a painting cannot carry is the one thing a card still needs: how the
surface wraps around the head. That is a single declared ellipsoid, with its
radii on the command line, instead of a 20-parameter search chasing a profile
nobody ever drew.

Output framing
--------------
The layer is a full 1254x1254 canvas, and the model space anchor is known from
the round-2 head fit: the eye-line midpoint sits at px(585,445) and the scale is
618.46 px per model unit. So a pixel maps to

    X = (px - ANCHOR_PX) / SS          Z = ANCHOR_Z + (ANCHOR_PY - py) / SS

and the card inherits the head's alignment for free -- it can be dropped next to
the head mesh without another registration step.

What it showed, and why it was not adopted
-----------------------------------------
Run on `10-front-hair.png` it does exactly what it says: 45k verts / 43k quads,
and the front render IS the painting. Registered on the single eye-line anchor
it lands on the head with face and eyes aligned -- dropping the layer's own
bounding box in favour of the painted canvas frame is what bought that, for
free.

The side view then shows what a card is: a sheet. That alone is forgivable at
3/4, which is the desk-pet viewing angle. It was rejected for a measured
reason instead: the model's own front silhouette is already close to the
painting (front IoU 0.7736), so one shell adds a rind and a few strands and
nothing else -- it cannot replace what the painting does with overlapping
layers -- and it re-covers the face that the T7 constraint had just been added
to protect.

Two readings were corrected while measuring it, both recorded because they
were wrong in a way that is easy to repeat: the dark plates in the render are
the dress and the ribbon, not hair (the model's hair is blue,
`WhaleHairGradient`), and `character-round3.glb` is the whole character -- one
`WhaleGirl` mesh, 11 materials -- not a head.

The outcome that does stand is a redirection: the gap is the hair TEXTURE, not
its geometry. The face reads right because it carries projected paint
(`WhaleFaceTex`); the hair is a flat gradient, so the strands, highlights and
layering of the painting are absent. Projecting `10-front-hair` /
`20-back-hair` onto the existing hair geometry is the unclaimed next step, and
it is cheap in the way this was not: no new geometry, so it cannot re-cover the
face, and it cannot disturb the fitted silhouette.

Run (Blender 4.x/5.x, headless):
  blender --background --factory-startup --python tools/build_hair_card.py -- \
      art/live2d/layers/10-front-hair.png .local/tmp/haircard
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

# --------------------------------------------------------------- registration
# From the round-2 head fit (see the project notes): the painted canvas is
# 1254x1254 and its eye-line midpoint corresponds to model (0, 1.66).
ANCHOR_PX = 585.0          # eye-line midpoint, in canvas pixels
ANCHOR_PY = 445.0
ANCHOR_Z = 1.66
SS = 618.46                # canvas pixels per model unit

# The declared assumption. The skull is not a sphere: measured from the head
# mesh it is far deeper front-to-back than it is wide. These are the radii the
# card wraps over, and the only thing in this tool that is not read off the
# painting -- hence they are arguments, printed in the report, and never
# adjusted per candidate behind the reader's back.
ELL_CENTRE = (0.0, 0.10, 1.95)     # head centre (x, y, z)
ELL_RADII = (0.55, 0.30, 0.50)     # (Rx, Ry, Rz)


def argv_after_ddash() -> list[str]:
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def parse_args(args: list[str]) -> tuple[list[str], dict[str, str]]:
    """Positional arguments plus `--key value` options, order-independent."""
    pos: list[str] = []
    opts: dict[str, str] = {}
    i = 0
    while i < len(args):
        if args[i].startswith("--"):
            key = args[i][2:]
            if i + 1 < len(args) and not args[i + 1].startswith("--"):
                opts[key] = args[i + 1]
                i += 2
            else:
                opts[key] = "1"          # bare flag
                i += 1
        else:
            pos.append(args[i])
            i += 1
    return pos, opts


def bounds(objects) -> tuple[Vector, float]:
    """World-space centre and height of everything that will be framed.

    The glTF importer parks a helper (a unit icosphere) in `glTF_not_exported`.
    It is not part of the asset, and feeding it to the framing zooms the camera
    out until the head is a speck -- the same trap the silhouette renderer hit.
    """
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for obj in objects:
        if obj.type != "MESH":
            continue
        if any(c.name == "glTF_not_exported" for c in obj.users_collection):
            continue
        for corner in obj.bound_box:
            world = obj.matrix_world @ Vector(corner)
            for axis in range(3):
                lo[axis] = min(lo[axis], world[axis])
                hi[axis] = max(hi[axis], world[axis])
    return (lo + hi) / 2.0, float(hi.z - lo.z)


def dress_imported(card) -> None:
    """Give an untextured import a neutral surface so its form is readable.

    The round-2 head carries a face texture but its hair shows up black here,
    which makes the comparison against the card unreadable for the wrong
    reason. Only the material-less parts are touched, so the painted face
    survives.
    """
    grey = bpy.data.materials.new("ImportedNeutral")
    grey.use_nodes = True
    bsdf = grey.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (0.62, 0.65, 0.72, 1.0)
        if "Roughness" in bsdf.inputs:
            bsdf.inputs["Roughness"].default_value = 0.75
    for obj in bpy.context.scene.objects:
        if obj is card or obj.type != "MESH":
            continue
        if obj.data.materials and len(obj.data.materials):
            continue
        obj.data.materials.append(grey)


def load_alpha(path: Path) -> tuple[np.ndarray, int, int]:
    """Alpha as a top-down float array plus the canvas size."""
    img = bpy.data.images.load(str(path.resolve()), check_existing=False)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
    bpy.data.images.remove(img)
    return px[::-1, :, 3].copy(), w, h      # Blender stores bottom-up


def wrap_y(x: float, z: float) -> float:
    """Depth of the card surface, from the declared ellipsoid.

    Below the centre, the vertical term is clamped: a lock hanging past the
    jaw should stay at the side of the head, not slide back to its centre.
    """
    cx, cy, cz = ELL_CENTRE
    rx, ry, rz = ELL_RADII
    dz = (min(z, cz) - cz) / rz
    t = 1.0 - (x - cx) ** 2 / rx ** 2 - dz ** 2
    return cy - ry * math.sqrt(t) if t > 0.0 else cy


def build_mesh(alpha: np.ndarray, w: int, h: int, step: int,
               cut: float) -> tuple[list, list, list]:
    ys, xs = np.where(alpha > cut)
    if not len(ys):
        raise RuntimeError("the layer has no pixels above the cut")
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()) + 1, int(ys.min()), int(ys.max()) + 1

    index: dict[tuple[int, int], int] = {}
    verts: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []

    def vertex(gx: int, gy: int) -> int:
        key = (gx, gy)
        hit = index.get(key)
        if hit is not None:
            return hit
        x = (gx - ANCHOR_PX) / SS
        z = ANCHOR_Z + (ANCHOR_PY - gy) / SS
        index[key] = len(verts)
        verts.append((x, wrap_y(x, z), z))
        uvs.append((gx / w, 1.0 - gy / h))
        return index[key]

    faces = []
    for gy in range(y0, y1, step):
        for gx in range(x0, x1, step):
            block = alpha[gy:gy + step, gx:gx + step]
            if block.size == 0 or float(block.mean()) <= cut:
                continue
            a = vertex(gx, gy)
            b = vertex(gx + step, gy)
            c = vertex(gx + step, gy + step)          # noqa: E741
            d = vertex(gx, gy + step)
            faces.append((a, b, c, d))
    return verts, faces, uvs


def make_object(name: str, verts, faces, uvs, texture: Path):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    uv_layer = mesh.uv_layers.new(name="UVMap")
    # from_pydata keeps vertex order, so the per-vertex uv list lines up.
    for loop in mesh.loops:
        uv_layer.data[loop.index].uv = uvs[loop.vertex_index]
    mesh.update()
    # Cards are seen from both sides; a single-sided sheet would vanish.
    for poly in mesh.polygons:
        poly.use_smooth = False

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)

    mat = bpy.data.materials.new(name + "-mat")
    mat.use_nodes = True
    nt = mat.node_tree
    for node in list(nt.nodes):
        nt.nodes.remove(node)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    out.location = (620, 0)
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (300, 0)
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.location = (-80, 0)
    uvn = nt.nodes.new("ShaderNodeUVMap")
    uvn.location = (-320, 0)
    uvn.uv_map = "UVMap"
    tex.image = bpy.data.images.load(str(texture.resolve()), check_existing=True)
    tex.interpolation = "Closest"
    nt.links.new(uvn.outputs["UV"], tex.inputs["Vector"])
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = 0.0
    if "Roughness" in bsdf.inputs:
        bsdf.inputs["Roughness"].default_value = 0.9
    if hasattr(mat, "blend_method"):
        # Renamed/removed across 4.2 -> 5.x for EEVEE Next; the alpha link above
        # is what actually carries the cutout, so this is best-effort only.
        try:
            mat.blend_method = "BLEND"
        except Exception:
            pass
    obj.data.materials.append(mat)
    return obj


def add_solidify(obj, thickness: float) -> None:
    if thickness <= 0.0:
        return
    mod = obj.modifiers.new("thickness", "SOLIDIFY")
    mod.thickness = thickness
    mod.offset = 0.0
    mod.use_rim = True
    mod.use_rim_only = False
    # Wrapping already decided which way is "out"; the shell only needs depth so
    # a grazing view does not read as paper.
    mod.use_flip_normals = True


# ------------------------------------------------------------------ rendering
VIEWS = (("front", (0.0, -4.0)), ("side", (4.0, 0.0)), ("turn", (2.8, -2.8)))


def setup_scene(centre: Vector, height: float) -> None:
    scene = bpy.context.scene
    for engine in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE", "CYCLES"):
        try:
            scene.render.engine = engine
            break
        except Exception:
            continue
    scene.render.resolution_x = 900
    scene.render.resolution_y = 1100
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    world = bpy.data.worlds.new("W") if scene.world is None else scene.world
    scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg is not None:
        bg.inputs[0].default_value = (1.0, 1.0, 1.0, 1.0)
        bg.inputs[1].default_value = 2.4

    cam_data = bpy.data.cameras.new("Cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = max(height * 1.15, 1.2)
    cam = bpy.data.objects.new("Cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam


def aim(cam, target: Vector, direction: tuple[float, float]) -> None:
    dx, dy = direction
    dist = 6.0
    cam.location = target + Vector((dx, dy, 0.0)).normalized() * dist
    look = target - Vector(cam.location)
    cam.rotation_euler = look.to_track_quat("-Z", "Y").to_euler()


def render_views(out_dir: Path, centre: Vector, height: float,
                 tag: str) -> list[str]:
    cam = bpy.context.scene.camera
    written = []
    for name, direction in VIEWS:
        aim(cam, centre, direction)
        path = (out_dir / f"{tag}-{name}.png").resolve()
        bpy.context.scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        written.append(str(path))
    return written


def main() -> int:
    pos, opts = parse_args(argv_after_ddash())
    if len(pos) < 2:
        print("usage: -- <layer.png> <out_dir> [step] [cut] [thickness] "
              "[--head head.glb] [--tag name] [--skip-card]")
        return 2
    png, out_dir = Path(pos[0]).resolve(), Path(pos[1]).resolve()
    step = int(pos[2]) if len(pos) > 2 else 2
    cut = float(pos[3]) if len(pos) > 3 else 0.35
    thickness = float(pos[4]) if len(pos) > 4 else 0.012
    tag = opts.get("tag", "card")
    out_dir.mkdir(parents=True, exist_ok=True)

    # --factory-startup hands us the default Cube, whose bounds reach z[-1,1]
    # and silently zoom the framing out until the asset is a speck.
    bpy.ops.wm.read_factory_settings(use_empty=True)

    obj = None
    if "skip-card" not in opts:
        alpha, w, h = load_alpha(png)
        verts, faces, uvs = build_mesh(alpha, w, h, step, cut)
        obj = make_object("HairCard", verts, faces, uvs, png)
        add_solidify(obj, thickness)

    head = opts.get("head")
    if head:
        # Same registration as the head fit, so the card lands on the head
        # without a second alignment step -- that is the point of using the
        # painted canvas frame instead of the layer's own bounding box.
        bpy.ops.import_scene.gltf(filepath=str(Path(head).resolve()))
        bpy.context.view_layer.update()
        dress_imported(obj)
        print(f"CARD head={head}")

    if obj is not None:
        zs = [v[2] for v in verts]
        xs = [v[0] for v in verts]
        print(f"CARD verts={len(verts)} quads={len(faces)}")
        print(f"CARD model x[{min(xs):+.3f},{max(xs):+.3f}] "
              f"z[{min(zs):.3f},{max(zs):.3f}] "
              f"y[{min(v[1] for v in verts):+.3f},{max(v[1] for v in verts):+.3f}]")
    centre, height = bounds(bpy.context.scene.objects)
    print(f"CARD layer={png.name} step={step} cut={cut} thickness={thickness}")
    print(f"CARD anchor px=({ANCHOR_PX:.0f},{ANCHOR_PY:.0f}) -> z={ANCHOR_Z} "
          f"ss={SS}  ellipsoid centre={ELL_CENTRE} radii={ELL_RADII}")
    print(f"CARD framing centre=({centre.x:+.3f},{centre.y:+.3f},{centre.z:+.3f}) "
          f"height={height:.3f}")

    setup_scene(centre, height)
    written = render_views(out_dir, centre, height, tag)
    for path in written:
        print(f"CARD wrote {path}")

    blend = out_dir / f"{tag}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    if obj is not None:
        glb = out_dir / f"{tag}.glb"
        for o in bpy.context.scene.objects:
            o.select_set(o is obj)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.export_scene.gltf(filepath=str(glb), export_format="GLB",
                                  use_selection=True, export_apply=True)
        print(f"CARD wrote {glb}")
    print(f"CARD wrote {blend}")
    print("CARD_RUN DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
