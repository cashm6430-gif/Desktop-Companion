"""Render the three fit views plus a head close-up of one GLB, in one EEVEE setup.

Used for the level-B approval sheet (`tools/draw_fit_approval.py` composes it).
The lighting is deliberately flat and identical for every model, so a difference
on the sheet is geometry, not shading -- orthographic camera, two suns, a plain
backdrop, no shadows beyond what the suns give.

The head close-up exists because the face is ~9% of a full-body frame: the
question the fitting has to answer (does anything new hang in front of the face)
is invisible at full-body scale.

Run:
  blender --background --factory-startup --python tools/render_fit_views.py -- \
      <glb> <out_prefix>
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

RES = (460, 620)
HEAD_RES = (460, 620)
HEAD_FRAC = 0.55            # of the character's height, seen in the close-up
HEAD_CENTRE = 0.70          # of the character's height, where the close-up looks


def argv_after_ddash() -> list[str]:
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def asset_bbox() -> tuple[Vector, Vector]:
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"
              and not any(c.name.startswith("glTF_not_exported")
                          for c in o.users_collection)]
    mins, maxs = Vector((1e9,) * 3), Vector((-1e9,) * 3)
    for obj in meshes:
        for corner in obj.bound_box:
            w = obj.matrix_world @ Vector(corner)
            mins = Vector(map(min, mins, w))
            maxs = Vector(map(max, maxs, w))
    return mins, maxs


def main() -> int:
    args = argv_after_ddash()
    if len(args) < 2:
        print("usage: -- <glb> <out_prefix>")
        return 2
    glb, prefix = Path(args[0]), args[1]
    # Absolute: Blender resolves a relative render filepath against the blend's
    # own directory and then fails *silently* -- no file, no error.
    prefix = str(Path(prefix).resolve())

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(glb))
    mins, maxs = asset_bbox()
    center = (mins + maxs) / 2.0
    size = (maxs - mins).length

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.film_transparent = False
    world = bpy.data.worlds.new("W")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.93, 0.94, 0.96, 1.0)
    world.node_tree.nodes["Background"].inputs[1].default_value = 1.0

    for name, rot, energy in (("key", (math.radians(52), 0, math.radians(38)), 3.0),
                              ("fill", (math.radians(70), 0, math.radians(-125)), 1.2)):
        light = bpy.data.lights.new(name, type="SUN")
        light.energy = energy
        obj = bpy.data.objects.new(name, light)
        obj.rotation_euler = rot
        scene.collection.objects.link(obj)

    cam_data = bpy.data.cameras.new("C")
    cam_data.type = "ORTHO"
    cam = bpy.data.objects.new("C", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    far = size

    front = ((center.x, center.y - far, center.z), (math.radians(90), 0, 0))
    left = ((center.x + far, center.y, center.z),
            (math.radians(90), 0, math.radians(90)))
    back = ((center.x, center.y + far, center.z),
            (math.radians(90), 0, math.pi))

    scene.render.resolution_x, scene.render.resolution_y = RES
    cam_data.ortho_scale = size * 1.06
    for label, (loc, rot) in (("front", front), ("left", left), ("back", back)):
        cam.location, cam.rotation_euler = loc, rot
        scene.render.filepath = f"{prefix}-{label}.png"
        bpy.ops.render.render(write_still=True)

    # Head close-up: keep the model's x centre, look at the head band only.
    scene.render.resolution_x, scene.render.resolution_y = HEAD_RES
    height = maxs.z - mins.z
    cam_data.ortho_scale = height * HEAD_FRAC
    cam.location = (center.x, center.y - far, mins.z + HEAD_CENTRE * height)
    cam.rotation_euler = front[1]
    scene.render.filepath = f"{prefix}-head.png"
    bpy.ops.render.render(write_still=True)

    print(f"FIT_VIEWS wrote {prefix}-front/-left/-back/-head.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
