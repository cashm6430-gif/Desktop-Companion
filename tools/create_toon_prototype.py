"""Build the non-final 3D technical-gate character with Blender in background.

Run: blender --background --factory-startup --python tools/create_toon_prototype.py
All geometry, weights and animation are authored here; no downloaded assets.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

import bpy
from mathutils import Quaternion, Vector


PROJECT = Path(__file__).resolve().parents[1]
FPS = 30
REST = {}
MESHES = []
MATERIALS = {}
SIDE = ("L", "R")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def material(name, rgba):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = rgba
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = rgba
    bsdf.inputs["Roughness"].default_value = 1.0
    bsdf.inputs["Metallic"].default_value = 0.0
    MATERIALS[name] = mat
    return mat


def bind(obj, weights, mat):
    obj.data.materials.clear()
    obj.data.materials.append(MATERIALS[mat])
    for bone, amount in weights.items():
        obj.vertex_groups.new(name=bone).add(list(range(len(obj.data.vertices))), amount, "REPLACE")
    for poly in obj.data.polygons:
        poly.use_smooth = True
    MESHES.append(obj)
    return obj


def ellipsoid(name, center, scale, bone, mat, segments=24, rings=12):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, location=center)
    obj = bpy.context.object
    obj.name = name
    obj.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return bind(obj, {bone: 1.0} if isinstance(bone, str) else bone, mat)


def tube(name, points, radii, bones, mat, sides=12):
    verts, faces, ring_weights = [], [], []
    pts = [Vector(p) for p in points]
    for idx, point in enumerate(pts):
        tangent = (pts[min(idx + 1, len(pts)-1)] - pts[max(0, idx-1)]).normalized()
        axis = Vector((0, 1, 0)) if abs(tangent.y) < 0.9 else Vector((1, 0, 0))
        u = tangent.cross(axis).normalized()
        v = tangent.cross(u).normalized()
        for side in range(sides):
            angle = math.tau * side / sides
            verts.append(point + radii[idx] * (math.cos(angle)*u + math.sin(angle)*v))
            ring_weights.append(bones[idx])
    for idx in range(len(pts)-1):
        for side in range(sides):
            a = idx*sides + side
            b = idx*sides + (side+1)%sides
            faces.append((a,b,b+sides,a+sides))
    faces += [tuple(reversed(range(sides))), tuple(range((len(pts)-1)*sides, len(pts)*sides))]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(MATERIALS[mat])
    for bone in sorted({b for weight in ring_weights for b in weight}):
        group = obj.vertex_groups.new(name=bone)
        for idx, weight in enumerate(ring_weights):
            if bone in weight:
                group.add([idx], weight[bone], "REPLACE")
    for poly in mesh.polygons:
        poly.use_smooth = True
    MESHES.append(obj)
    return obj


def cone_skirt():
    # Closed underlayer. Legs are hidden inside the flared hem, never clipped away.
    rings = [(0.91, 0.22, 0.14), (0.82, 0.26, 0.18), (0.66, 0.39, 0.27)]
    verts, faces = [], []
    for z, rx, ry in rings:
        for i in range(32):
            a = math.tau*i/32
            pleat = 1.0 + 0.035*math.cos(a*12)
            verts.append((rx*math.cos(a)*pleat, ry*math.sin(a)*pleat, z))
    for ring in range(2):
        for i in range(32):
            a = ring*32+i; b = ring*32+(i+1)%32
            faces.append((a,b,b+32,a+32))
    # Inner facing closes the garment with an intentionally generous leg opening.
    for i in range(32):
        a = math.tau*i/32
        verts.append((0.25*math.cos(a), 0.18*math.sin(a), 0.68))
    for i in range(32):
        faces.append((64+i,64+(i+1)%32,96+(i+1)%32,96+i))
    mesh = bpy.data.meshes.new("SkirtClosedHem")
    mesh.from_pydata(verts, [], faces); mesh.update()
    obj = bpy.data.objects.new("SkirtClosedHem", mesh)
    bpy.context.collection.objects.link(obj)
    bind(obj, {"pelvis":1}, "ToonNavy")
    # Upper waist joins torso and hip volume instead of using empty alpha cards.
    ellipsoid("Waistband", (0,0,0.88), (0.235,0.155,0.065), "pelvis", "ToonWhite")


def build_rig():
    armdata = bpy.data.armatures.new("PrototypeSkeleton")
    arm = bpy.data.objects.new("Armature", armdata)
    bpy.context.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    entries = [
        ("root", (0,0,0), (0,0,0.15), None),
        ("pelvis", (0,0,0.72), (0,0,0.91), "root"),
        ("spine", (0,0,0.91), (0,0,1.14), "pelvis"),
        ("neck", (0,0,1.14), (0,0,1.25), "spine"),
        ("head", (0,0,1.25), (0,0,1.87), "neck"),
        ("hair", (0,0.19,1.66), (0,0.23,1.12), "head"),
        ("tail", (0,0.10,0.79), (0,0.51,0.84), "pelvis"),
    ]
    for label, s in (("L",1),("R",-1)):
        entries += [
            (f"shoulder.{label}", (s*0.19,0,1.12), (s*0.30,0,1.11), "spine"),
            (f"upper_arm.{label}", (s*0.30,0,1.11), (s*0.44,0,0.91), f"shoulder.{label}"),
            (f"forearm.{label}", (s*0.44,0,0.91), (s*0.45,-0.02,0.70), f"upper_arm.{label}"),
            (f"wrist.{label}", (s*0.45,-0.02,0.70), (s*0.45,-0.07,0.63), f"forearm.{label}"),
            (f"thigh.{label}", (s*0.145,0,0.72), (s*0.145,0,0.41), "pelvis"),
            (f"shin.{label}", (s*0.145,0,0.41), (s*0.145,0,0.12), f"thigh.{label}"),
            (f"foot.{label}", (s*0.145,0,0.12), (s*0.145,-0.20,0.08), f"shin.{label}"),
        ]
    for name, start, end, parent in entries:
        bone = armdata.edit_bones.new(name)
        bone.head, bone.tail = start, end
        bone.use_deform = True
        if parent:
            bone.parent = armdata.edit_bones[parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    arm.show_in_front = True
    for bone in armdata.bones:
        REST[bone.name] = {
            "head": bone.head_local.copy(), "tail":bone.tail_local.copy(),
            "quaternion":bone.matrix_local.to_quaternion(),
            "length":bone.length,
        }
        arm.pose.bones[bone.name].rotation_mode = "QUATERNION"
    return arm


def build_geometry():
    for name, rgba in {
        "ToonSkin": (1.0,0.80,0.72,1), "ToonHairBlue": (0.09,0.38,0.82,1),
        "ToonHairDark":(0.05,0.16,0.43,1), "ToonNavy":(0.035,0.055,0.16,1),
        "ToonWhite":(0.91,0.96,1,1), "ToonEyeWhite":(1,1,1,1),
        "ToonIris":(0.11,0.48,0.95,1), "ToonPupil":(0.015,0.035,0.09,1),
        "ToonHighlight":(1,1,1,1), "ToonMouth":(0.24,0.055,0.09,1),
    }.items():
        material(name, rgba)
    ellipsoid("Torso", (0,0,1.005), (0.255,0.15,0.235), "spine", "ToonNavy")
    ellipsoid("PelvisVolume", (0,0,0.72), (0.23,0.15,0.12), "pelvis", "ToonNavy")
    ellipsoid("NeckVolume", (0,0,1.17), (0.105,0.10,0.16), "neck", "ToonSkin")
    ellipsoid("FaceVolume", (0,-0.025,1.64), (0.535,0.41,0.52), "head", "ToonSkin",32,20)
    ellipsoid("HairBackVolume", (0,0.09,1.69), (0.57,0.40,0.535), "head", "ToonHairDark",32,16)
    # Bangs and side locks are actual opaque volumes with head-bound roots.
    for x,z,sx,sz in [(-.35,1.91,.20,.29),(-.12,2.00,.18,.25),(.11,2.01,.18,.24),(.34,1.91,.21,.29)]:
        ellipsoid("Bang", (x,-.28,z), (sx,.15,sz), "head", "ToonHairBlue")
    for s in (-1,1):
        tube("SideHair", [(s*.46,0,1.82),(s*.55,.02,1.42),(s*.47,.05,1.11),(s*.55,.02,1.00)],
             [.16,.15,.115,.015], [{"head":1},{"head":.7,"hair":.3},{"hair":1},{"hair":1}], "ToonHairBlue")
    tube("Ahoge", [(0,0,2.08),(.02,0,2.27),(.17,0,2.31),(.22,0,2.23),(.15,0,2.18)],
         [.025,.023,.02,.016,.002], [{"head":1}]*5,"ToonHairBlue")
    for label,s in (("L",1),("R",-1)):
        a,b,c = f"upper_arm.{label}",f"forearm.{label}",f"wrist.{label}"
        # One continuous skinned surface across shoulder, elbow and cuff.
        tube(f"ArmSleeve.{label}", [(s*.20,0,1.12),(s*.30,0,1.11),(s*.395,0,.965),
             (s*.44,0,.91),(s*.45,-.013,.76),(s*.45,-.02,.70)],
             [.095,.10,.09,.086,.096,.102],
             [{f"shoulder.{label}":.7,a:.3},{a:1},{a:.85,b:.15},{a:.5,b:.5},{b:1},{b:1}],"ToonNavy")
        ellipsoid(f"Cuff.{label}", (s*.45,-.02,.72), (.104,.098,.045), b, "ToonWhite")
        ellipsoid(f"Hand.{label}", (s*.45,-.035,.657), (.068,.06,.08), c, "ToonSkin")
        thigh,shin,foot = f"thigh.{label}",f"shin.{label}",f"foot.{label}"
        tube(f"Leg.{label}", [(s*.145,0,.72),(s*.145,0,.53),(s*.145,0,.41),
             (s*.145,0,.29),(s*.145,0,.12)], [.102,.094,.092,.080,.062],
             [{thigh:1},{thigh:1},{thigh:.5,shin:.5},{shin:1},{shin:1}],"ToonWhite")
        ellipsoid(f"Shoe.{label}", (s*.145,-.085,.065), (.098,.18,.065),foot,"ToonNavy")
    cone_skirt()
    # The tail remains behind the complete body and follows a separate deform bone.
    tube("TailStem", [(0,.13,.81),(0,.39,.82),(0,.54,.85)], [.10,.072,.055], [{"tail":1}]*3,"ToonHairDark")
    for s in (-1,1):
        fin = ellipsoid("TailFin", (s*.13,.55,.85),(.18,.055,.095),"tail","ToonHairBlue")
        fin.rotation_euler.y = s*.45
        bpy.context.view_layer.objects.active=fin
        bpy.ops.object.transform_apply(location=False,rotation=True,scale=False)
    ellipsoid("ShirtFront",(0,-.142,1.02),(.095,.018,.16),"spine","ToonWhite")
    ellipsoid("Collar",(0,-.07,1.19),(.13,.10,.044),"neck","ToonWhite")


def build_face():
    face_objects=[]
    for s in (-1,1):
        face_objects.append(ellipsoid("EyeWhite",(s*.20,-.397,1.67),(.132,.030,.148),"head","ToonEyeWhite"))
        face_objects.append(ellipsoid("Iris",(s*.20,-.424,1.66),(.083,.017,.110),"head","ToonIris"))
        face_objects.append(ellipsoid("Pupil",(s*.20,-.440,1.66),(.041,.012,.072),"head","ToonPupil"))
        face_objects.append(ellipsoid("Glint",(s*.20-.025,-.451,1.70),(.024,.01,.035),"head","ToonHighlight"))
    mouth_points=[(x,-.416,1.477-.014*(1-(x/.057)**2)) for x in [-.057,-.0285,0,.0285,.057]]
    mouth=tube("ClosedMouth",mouth_points,[.005]*5,[{"head":1}]*5,"ToonMouth",8)
    face_objects.append(mouth)
    bpy.ops.object.select_all(action="DESELECT")
    for obj in face_objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active=face_objects[0]
    bpy.ops.object.join()
    face=bpy.context.object; face.name="FaceFeatures"
    basis=face.shape_key_add(name="Basis",from_mix=False)
    # Coordinates are in joined mesh local space. Convert to global for semantic masks.
    baseline=[face.matrix_world@v.co for v in face.data.vertices]
    inverse=face.matrix_world.inverted()
    mouth_vertices={idx for poly in face.data.polygons
                    if face.data.materials[poly.material_index].name=="ToonMouth"
                    for idx in poly.vertices}
    for name,sign in [("Blink_L",1),("Blink_R",-1)]:
        # Blender's default from_mix=True samples the previous keys at their current
        # values; that silently baked left blink into right blink and both into Smile.
        key=face.shape_key_add(name=name,from_mix=False)
        key.relative_key=basis
        key.value=0.0
        for idx,world in enumerate(baseline):
            if idx not in mouth_vertices and world.x*sign>0:
                value=world.copy(); value.z=1.663+(world.z-1.663)*.045
                key.data[idx].co=inverse@value
    smile=face.shape_key_add(name="Smile",from_mix=False)
    smile.relative_key=basis
    smile.value=0.0
    for idx,world in enumerate(baseline):
        if idx in mouth_vertices:
            value=world.copy(); value.x*=1.35
            value.z=1.477+(world.z-1.477)*2.4
            smile.data[idx].co=inverse@value
    smile.value=.2
    return face


def expression_audit(face):
    """Prove keys modify only their semantic region, independently of key values."""
    keys=face.data.shape_keys.key_blocks
    base=keys["Basis"]
    by_material={}
    for poly in face.data.polygons:
        by_material.setdefault(face.data.materials[poly.material_index].name,set()).update(poly.vertices)
    results={}
    for name in ("Blink_L","Blink_R","Smile"):
        key=keys[name]
        details={}
        for material_name,indices in by_material.items():
            deltas=[(key.data[i].co-base.data[i].co).length for i in indices]
            details[material_name]={"changed_vertices":sum(d>1e-8 for d in deltas),"max_delta":max(deltas)}
            for idx,delta in zip(indices,deltas):
                if delta<=1e-8:continue
                world=face.matrix_world@base.data[idx].co
                if name=="Smile" and material_name!="ToonMouth":
                    raise RuntimeError(f"Smile leaks into {material_name} vertex {idx}")
                if name.startswith("Blink"):
                    sign=1 if name.endswith("_L") else -1
                    if material_name=="ToonMouth" or world.x*sign<=0:
                        raise RuntimeError(f"{name} leaks outside its eye at vertex {idx}")
        results[name]={"default":key.value,"materials":details}
    if results["Blink_L"]["default"]!=0 or results["Blink_R"]["default"]!=0:
        raise RuntimeError("Neutral face must have both blink values zero")
    return results


def desired_pose(arm, goals):
    world_quats={}
    for pb in arm.pose.bones:
        rest=REST[pb.name]["quaternion"]
        parent=pb.parent
        rest_relative=REST[parent.name]["quaternion"].inverted()@rest if parent else rest
        goal=goals.get(pb.name)
        if goal is None:
            # Preserve an unmodified local rest orientation while following parent.
            goal=world_quats[parent.name]@rest_relative if parent else rest
        local=rest_relative.inverted()@(world_quats[parent.name].inverted()@goal if parent else goal)
        pb.rotation_quaternion=local
        world_quats[pb.name]=goal


def orient(name, direction):
    rest=REST[name]
    return (rest["tail"]-rest["head"]).rotation_difference(Vector(direction))@rest["quaternion"]


def smooth(value):
    return value*value*(3-2*value)


def make_actions(arm):
    audit={}
    for clip,end in [("idle",120),("wave",75),("sit",90)]:
        action=bpy.data.actions.new(clip)
        # Actions outside the active slot must survive reopening the author source.
        action.use_fake_user=True
        arm.animation_data_create();arm.animation_data.action=action
        feet_errors=[];length_errors=[];contacts=[]
        for frame in range(1,end+1):
            time=(frame-1)/FPS
            for pb in arm.pose.bones:
                pb.location=(0,0,0);pb.scale=(1,1,1);pb.rotation_quaternion=(1,0,0,0)
            goals={}
            if clip=="idle":
                breathe=.017*math.sin(time*math.tau/4)
                goals["spine"]=Quaternion((1,0,0),breathe)@REST["spine"]["quaternion"]
                goals["head"]=Quaternion((0,0,1),.02*math.sin(time*math.tau/4))@REST["head"]["quaternion"]
                goals["tail"]=Quaternion((0,0,1),.05*math.sin(time*math.tau/4))@REST["tail"]["quaternion"]
            elif clip=="wave":
                up=smooth(min(time/.65,1))*smooth(min(max((2.45-time)/.55,0),1))
                flap=math.sin(max(time-.65,0)*math.tau*1.9)*up
                goals["shoulder.L"]=Quaternion((0,1,0),-.10*up)@REST["shoulder.L"]["quaternion"]
                goals["upper_arm.L"]=Quaternion((0,1,0),-1.85*up)@REST["upper_arm.L"]["quaternion"]
                goals["forearm.L"]=Quaternion((0,1,0),(-2.10+.22*flap)*up)@REST["forearm.L"]["quaternion"]
                goals["wrist.L"]=Quaternion((0,1,0),(-2.10+.35*flap)*up)@REST["wrist.L"]["quaternion"]
                goals["head"]=Quaternion((0,1,0),-.05*up)@REST["head"]["quaternion"]
            else:
                t=smooth(min(max((time-.25)/2.2,0),1))
                arm.pose.bones["pelvis"].location=(0,.18*t,-.25*t)
                # Hip translation is in a Z-aligned bone local frame (local Y -> world Z).
                arm.pose.bones["pelvis"].location=REST["pelvis"]["quaternion"].inverted()@Vector((0,.18*t,-.25*t))
                pitch=.13*math.sin(t*math.pi)+.035*t
                goals["spine"]=Quaternion((1,0,0),pitch)@REST["spine"]["quaternion"]
                goals["head"]=Quaternion((1,0,0),-.025*t)@REST["head"]["quaternion"]
                for label,s in (("L",1),("R",-1)):
                    thigh,shin,foot=f"thigh.{label}",f"shin.{label}",f"foot.{label}"
                    hip=REST[thigh]["head"]+Vector((0,.18*t,-.25*t))
                    ankle=REST[foot]["head"]
                    direction=ankle-hip; distance=direction.length
                    d=direction.normalized(); a=REST[thigh]["length"]; b=REST[shin]["length"]
                    along=(a*a-b*b+distance*distance)/(2*distance)
                    depth=math.sqrt(max(a*a-along*along,0))
                    pole=Vector((0,-1,0));perp=(pole-d*pole.dot(d)).normalized()
                    knee=hip+d*along+perp*depth
                    goals[thigh]=orient(thigh,knee-hip)
                    goals[shin]=orient(shin,ankle-knee)
                    goals[foot]=REST[foot]["quaternion"]
                    # Arms move towards the lap through shoulder/elbow rotations only.
                    goals[f"upper_arm.{label}"]=Quaternion((1,0,0),-.40*t)@REST[f"upper_arm.{label}"]["quaternion"]
                    goals[f"forearm.{label}"]=Quaternion((1,0,0),-.9*t)@REST[f"forearm.{label}"]["quaternion"]
            desired_pose(arm,goals)
            bpy.context.view_layer.update()
            for pb in arm.pose.bones:
                pb.keyframe_insert(data_path="location",frame=frame)
                pb.keyframe_insert(data_path="rotation_quaternion",frame=frame)
                pb.keyframe_insert(data_path="scale",frame=frame)
                length_errors.append(abs((pb.tail-pb.head).length-REST[pb.name]["length"]))
            for label in SIDE:
                pb=arm.pose.bones[f"foot.{label}"]
                feet_errors.append((pb.head-REST[pb.name]["head"]).length)
            if frame in (1,end//2,end):
                contacts.append({"frame":frame,"pelvis_blender":list(arm.pose.bones["pelvis"].head),
                                 "foot_L_blender":list(arm.pose.bones["foot.L"].head),
                                 "wrist_L_blender":list(arm.pose.bones["wrist.L"].tail)})
        # Dense keys deliberately prevent a different exporter curve interpretation.
        if action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    for slot in action.slots:
                        bag=strip.channelbag(slot)
                        if bag:
                            for curve in bag.fcurves:
                                for key in curve.keyframe_points:
                                    key.interpolation="LINEAR"
        audit[clip]={"frames":end,"duration_seconds":(end-1)/FPS,"sample_rate":FPS,
                     "max_bone_length_error":max(length_errors),"max_foot_anchor_error":max(feet_errors),
                     "contacts":contacts}
        if clip=="wave":
            audit[clip].update({"side":"L","driven_bone":"upper_arm.L","wrist_bone":"wrist.L",
                                "follower_bone":"wrist.L"})
    arm.animation_data.action=None
    return audit


def glb_structure(path):
    raw=Path(path).read_bytes()
    magic,version,length=struct.unpack_from("<III",raw)
    chunk_length,chunk_type=struct.unpack_from("<II",raw,12)
    if magic!=0x46546C67 or version!=2 or length!=len(raw) or chunk_type!=0x4E4F534A:
        raise RuntimeError("Invalid GLB container")
    data=json.loads(raw[20:20+chunk_length])
    return {"node_count":len(data.get("nodes",[])),"mesh_count":len(data.get("meshes",[])),
            "skin_joint_counts":[len(s["joints"]) for s in data.get("skins",[])],
            "animations":[{"name":a["name"],"channels":len(a["channels"])} for a in data.get("animations",[])],
            "morph_meshes":[{"name":m.get("name"),"names":m.get("extras",{}).get("targetNames",[])}
                            for m in data.get("meshes",[]) if any("targets" in p for p in m["primitives"])]}


def readback_audit(blend, glb, source):
    records={}
    for kind in ("author", "glb"):
        if kind=="author":
            bpy.ops.wm.open_mainfile(filepath=str(blend))
        else:
            bpy.ops.object.select_all(action="SELECT");bpy.ops.object.delete(use_global=False)
            for action in list(bpy.data.actions):bpy.data.actions.remove(action)
            bpy.data.orphans_purge(do_local_ids=True,do_linked_ids=True,do_recursive=True)
            bpy.ops.import_scene.gltf(filepath=str(glb))
        armatures=[o for o in bpy.context.scene.objects if o.type=="ARMATURE"]
        if len(armatures)!=1:
            raise RuntimeError(f"{kind}: expected one skeleton")
        arm=armatures[0]
        actions={a.name:a for a in bpy.data.actions}
        if not {"idle","sit","wave"} <= actions.keys():
            raise RuntimeError(f"{kind}: clips lost on readback: {list(actions)}")
        face=bpy.data.objects.get("FaceFeatures")
        expressions=[k.name for k in face.data.shape_keys.key_blocks]
        if not {"Blink_L","Blink_R","Smile"}<=set(expressions):
            raise RuntimeError(f"{kind}: expressions lost on readback")
        semantic_audit=expression_audit(face)
        per_clip={}
        for clip,end in (("idle",120),("wave",75),("sit",90)):
            arm.animation_data_create();arm.animation_data.action=actions[clip]
            error=0.0
            for frame in range(1,end+1):
                bpy.context.scene.frame_set(frame)
                bpy.context.view_layer.update()
                for label in SIDE:
                    point=arm.matrix_world@arm.pose.bones[f"foot.{label}"].head
                    target=Vector((.145 if label=="L" else -.145,0,.12))
                    error=max(error,(point-target).length)
            per_clip[clip]={"samples":end,"max_world_foot_anchor_error":error}
            if error>1e-5:
                raise RuntimeError(f"{kind}/{clip}: exported foot slipped by {error}")
        records[kind]={"actions":list(actions),"bone_count":len(arm.data.bones),
                       "shape_keys":expressions,"expression_isolation":semantic_audit,"per_clip":per_clip}
    write_json(source/"source-and-export-audit.json",{
        "schema_version":1,"status":"numeric_readback_pass_visual_pending",
        "source_sha256":digest(blend),"glb_sha256":digest(glb),"blender_version":bpy.app.version_string,
        "records":records,"limits":"Feet and animation/shape presence do not prove silhouette or cloth/contact quality."})


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--project",type=Path,default=PROJECT)
    args=parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    source=args.project/"art/3d/prototype"; export=args.project/"assets/3d/prototype"
    source.mkdir(parents=True,exist_ok=True);export.mkdir(parents=True,exist_ok=True)
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.object.select_all(action="SELECT");bpy.ops.object.delete(use_global=False)
    for action in list(bpy.data.actions):bpy.data.actions.remove(action)
    scene=bpy.context.scene;scene.render.fps=FPS;scene.frame_start=1;scene.frame_end=120
    arm=build_rig(); build_geometry();face=build_face()
    for obj in list(bpy.context.scene.objects):
        if obj.type=="MESH":
            obj.parent=arm
            mod=obj.modifiers.new("PrototypeSkin","ARMATURE");mod.object=arm
    animations=make_actions(arm)
    arm.animation_data.action=bpy.data.actions["idle"]
    scene.frame_set(1)
    for obj in bpy.context.scene.objects:obj.select_set(True)
    bpy.context.view_layer.objects.active=arm
    blend=source/"character.blend";glb=export/"character.glb"
    bpy.data.orphans_purge(do_local_ids=True,do_linked_ids=True,do_recursive=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    options={"export_format":"GLB","export_yup":True,"export_animations":True,
             "export_animation_mode":"ACTIONS","export_def_bones":True,"export_morph":True,
             "export_morph_normal":True,"export_morph_animation":False,"export_skins":True,
             "export_all_influences":False,"export_optimize_animation_size":False,
             "export_apply":False,"export_cameras":False,"export_lights":False}
    bpy.ops.export_scene.gltf(filepath=str(glb),**options)
    weights=[]; unweighted=[]
    for obj in bpy.context.scene.objects:
        if obj.type!="MESH":continue
        for vertex in obj.data.vertices:
            total=sum(g.weight for g in vertex.groups)
            weights.append(abs(total-1))
            if not vertex.groups:unweighted.append([obj.name,vertex.index])
    bones=[{"name":b.name,"parent":b.parent.name if b.parent else None,
            "length":REST[b.name]["length"],"head_blender":list(b.head_local),"tail_blender":list(b.tail_local),
            "deform":b.use_deform} for b in arm.data.bones]
    contract={"schema_version":1,"status":"technical_prototype_non_final","character_identity":"generic_q_toon_proxy",
              "purpose":"Technical gate A: real volumes, skinned joints, portable clips, expressions; no whale-girl art approval.",
              "author_coordinates":{"up":"+Z","front":"-Y","units":"meters"},
              "runtime_coordinates":{"up":"+Y","front":"+Z","units":"meters","conversion":"(x,y,z)->(x,z,-y)"},
              "ground_y":0,"head_top_y":2.16,"total_height_y":2.335,"forward_axis":[0,0,1],
              "nodes":{"armature":"Armature","face":"FaceFeatures"},"bones":bones,
              "expressions":{"Blink_L":{"range":[0,1],"default":0},"Blink_R":{"range":[0,1],"default":0},
                             "Smile":{"range":[0,1],"default":.2}},
              "materials":list(MATERIALS),"material_alpha":"all opaque geometry",
              "animations":animations,"rise":"play sit backwards; do not scale limbs",
              "sit_contact":{"stool_top_y":.35,"stool_center":[0,.175,-.18],"stool_size":[.56,.35,.36],
                             "foot_anchor_L":[.145,.12,0],"foot_anchor_R":[-.145,.12,0],
                             "note":"Temporary broad stool; not a final cloth/contact rig. Requires actual runtime visual check."},
              "audit":{"max_weight_sum_error":max(weights),"unweighted_vertices":unweighted,
                       "all_deform_bones":all(b["deform"] for b in bones),"structure":glb_structure(glb),
                       "expression_isolation":expression_audit(face)},
              "source_sha256":digest(blend),"glb_sha256":digest(glb),
              "visual_status":"pending_actual_godot_review","limits":["temporary primitive-based art","no fingers/cloth simulation", "no production character likeness"]}
    if unweighted or max(weights)>1e-5 or any(a["max_bone_length_error"]>1e-5 or a["max_foot_anchor_error"]>1e-5 for a in animations.values()):
        raise RuntimeError("Rig numerical audit failed: "+json.dumps(contract["audit"]))
    write_json(source/"rig-contract.json",contract)
    write_json(source/"rebuild-recipe.json",{"schema_version":1,"status":"technical_prototype_non_final",
               "script":"tools/create_toon_prototype.py","script_sha256":digest(__file__),
               "blender_version":bpy.app.version_string,"blender_build_hash":bpy.app.build_hash.decode(),
               "command":["D:/tool/blender/blender.exe","--background","--factory-startup","--python","tools/create_toon_prototype.py"],
               "author":"art/3d/prototype/character.blend","export":"assets/3d/prototype/character.glb",
               "export_options":options,"source_sha256":digest(blend),"glb_sha256":digest(glb),
               "validation":"rig-contract.json numeric audit; runtime visual approval is separate and pending"})
    readback_audit(blend,glb,source)
    print("PROTOTYPE_COMPLETE "+json.dumps({"blend":str(blend),"glb":str(glb),"animations":animations,"glb_structure":contract["audit"]["structure"]}))


if __name__=="__main__":main()
