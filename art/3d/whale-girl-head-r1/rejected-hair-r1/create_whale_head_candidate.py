"""Editable Q whale-girl head candidate, authored and audited in background Blender.

No painted bitmap is manufactured here. Head, eyes, lashes, ribbons and curls
are three-dimensional meshes; prototype body/actions are a temporary showcase.
Run: D:/tool/blender/blender.exe --background --factory-startup --python tools/create_whale_head_candidate.py
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("toon_author", ROOT / "tools/create_toon_prototype.py")
proto = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proto)
FACE_CENTER = (0.0, -0.012, 1.65)
FACE_RADII = (.545, .365, .455)
FACE_SEMANTICS = {}
FACE_OBJECTS = []
HEAD_OBJECTS = []


def linear_hex(value):
    channels = [int(value[i:i+2], 16) / 255 for i in (0, 2, 4)]
    return tuple(x / 12.92 if x <= .04045 else ((x+.055)/1.055)**2.4 for x in channels) + (1.,)


def mesh_object(name, vertices, faces, material, weights=None):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    proto.bind(obj, weights or {"head": 1.0}, material)
    HEAD_OBJECTS.append(obj)
    return obj


def face_surface(x, z):
    """Analytic opaque face surface used by all detailed face vertices.

    Features are a curved thin solid resting on this ellipsoid, not cards.
    A broad cheek width is retained and the lower chin is gently narrowed.
    """
    cx, cy, cz = FACE_CENTER
    rx, ry, rz = FACE_RADII
    qz = (z-cz)/rz
    taper = 1.0 - .18*max(-qz, 0.0)**1.7
    qx = (x-cx)/(rx*taper)
    inside = 1.0-qx*qx-qz*qz
    if inside <= 0:
        raise ValueError(f"Facial vertex lies beyond head shell: x={x}, z={z}")
    return cy-ry*math.sqrt(inside)


def make_face_volume():
    obj = proto.ellipsoid("FaceVolume", FACE_CENTER, FACE_RADII, "head", "WhaleSkin", 64, 40)
    # The exact same profile is consumed by conformal detail generation.
    for v in obj.data.vertices:
        qz = v.co.z / FACE_RADII[2]
        v.co.x *= 1.0-.18*max(-qz, 0.0)**1.7
    HEAD_OBJECTS.append(obj)
    return obj


def face_disk(name, cx, cz, rx, rz, depth, material, semantic,
              almond=False, tilt=0.0, rings=5, segments=48):
    """Closed thin solid with its front/back both conformal to the face shell."""
    vertices = []
    for back in (False, True):
        for ring in range(rings+1):
            radius = ring/rings
            for seg in range(segments):
                angle = math.tau*seg/segments
                x = cx + rx*radius*math.cos(angle)
                dz = rz*radius*math.sin(angle)
                if almond:
                    dz *= 1-.25*abs(math.cos(angle))
                z = cz + dz + tilt*(x-cx)
                vertices.append((x, face_surface(x,z)-depth+(.0016 if back else 0), z))
    per_side = (rings+1)*segments
    faces = []
    # Front winding faces outward toward -Y, rear winding faces the head.
    for side in range(2):
        offset = side*per_side
        for ring in range(rings):
            for seg in range(segments):
                a=offset+ring*segments+seg
                b=offset+ring*segments+(seg+1)%segments
                quad=(a,a+segments,b+segments,b)
                faces.append(tuple(reversed(quad)) if side else quad)
    for seg in range(segments):
        a=rings*segments+seg; b=rings*segments+(seg+1)%segments
        faces.append((a,b,b+per_side,a+per_side))
    obj=mesh_object(name,vertices,faces,material)
    FACE_SEMANTICS[obj.name]=semantic
    FACE_OBJECTS.append(obj)
    return obj


def face_line(name, points, radius, depth, material, semantic):
    mapped = [(x,face_surface(x,z)-depth,z) for x,z in points]
    obj = proto.tube(name,mapped,[radius]*len(mapped),[{"head":1}]*len(mapped),material,8)
    # A round cross section at a steep cheek/forehead can tunnel through skin.
    # Reproject every vertex to the analytic shell and retain only a small
    # convex depth profile. Width in the silhouette stays unchanged.
    for vertex in obj.data.vertices:
        ring=vertex.index//8
        bulge=(vertex.co.y-mapped[ring][1])*.18
        vertex.co.y=face_surface(vertex.co.x,vertex.co.z)-depth+bulge
    HEAD_OBJECTS.append(obj); FACE_OBJECTS.append(obj); FACE_SEMANTICS[obj.name]=semantic
    return obj


def catmull(points, samples=7):
    source=[Vector(x) for x in points]
    result=[]
    for idx in range(len(source)-1):
        a=source[max(idx-1,0)]; b=source[idx]; c=source[idx+1]; d=source[min(idx+2,len(source)-1)]
        for sample in range(samples):
            t=sample/samples
            result.append(.5*((2*b)+(-a+c)*t+(2*a-5*b+4*c-d)*t*t+(-a+3*b-3*c+d)*t*t*t))
    result.append(source[-1])
    return result


def sweep(name, controls, widths, depths, material="WhaleHairRoyal", hair_weight=False,
          gradient=True, sides=12, samples=7):
    points=catmull(controls,samples)
    vertices=[]; faces=[]; weights=[]
    for idx,p in enumerate(points):
        f=idx/samples
        segment=min(int(f),len(widths)-2); amount=f-segment
        width=widths[segment]*(1-amount)+widths[segment+1]*amount
        depth=depths[segment]*(1-amount)+depths[segment+1]*amount
        tangent=(points[min(idx+1,len(points)-1)]-points[max(idx-1,0)]).normalized()
        # Front/back thickness stays smaller than silhouette width.
        reference=Vector((0,1,0))
        if abs(tangent.dot(reference))>.95: reference=Vector((1,0,0))
        u=tangent.cross(reference).normalized(); v=tangent.cross(u).normalized()
        blend=max(0,min(1,(1.68-p.z)/.52)) if hair_weight else 0
        for seg in range(sides):
            angle=math.tau*seg/sides
            vertices.append(p+u*(width*math.cos(angle))+v*(depth*math.sin(angle)))
            weights.append({"head":1-blend,"hair":blend})
    for idx in range(len(points)-1):
        for seg in range(sides):
            a=idx*sides+seg; b=idx*sides+(seg+1)%sides
            faces.append((a,b,b+sides,a+sides))
    faces.extend([tuple(reversed(range(sides))),tuple(range((len(points)-1)*sides,len(points)*sides))])
    obj=mesh_object(name,vertices,faces,material)
    for group in list(obj.vertex_groups):obj.vertex_groups.remove(group)
    for bone in ("head","hair"):
        group=obj.vertex_groups.new(name=bone)
        for idx,weight in enumerate(weights):
            if weight[bone]>.000001:group.add([idx],weight[bone],"REPLACE")
    if gradient:
        for label in ("WhaleHairMid","WhaleHairCyan"):
            obj.data.materials.append(proto.MATERIALS[label])
        for poly in obj.data.polygons:
            z=sum(obj.data.vertices[i].co.z for i in poly.vertices)/len(poly.vertices)
            poly.material_index=2 if z<1.00 else (1 if z<1.27 else 0)
    return obj


def remove_proxy_head():
    prefixes=("FaceVolume","HairBackVolume","Bang","SideHair","Ahoge")
    for obj in list(bpy.context.scene.objects):
        if obj.name.startswith(prefixes):
            if obj in proto.MESHES:proto.MESHES.remove(obj)
            bpy.data.objects.remove(obj,do_unlink=True)


def build_head_geometry():
    make_face_volume()
    # Complete posterior cranium and a neck-to-waist cascade; real side/back volume.
    cap=proto.ellipsoid("WhaleHairCranium",(0,.090,1.694),(.578,.355,.494),"head","WhaleHairRoyal",48,28)
    HEAD_OBJECTS.append(cap)
    # Slightly asymmetric leaf bangs, roots connected to the cranial hair mass.
    bang_specs=[
        ([(-.02,-.205,2.119),(-.075,-.329,2.025),(-.095,-.385,1.875),(-.005,-.379,1.716)], [.12,.165,.12,.004]),
        ([(.10,-.205,2.106),(.18,-.323,2.012),(.245,-.344,1.882),(.343,-.305,1.778)], [.135,.137,.115,.005]),
        ([(-.21,-.17,2.063),(-.32,-.272,1.998),(-.4,-.256,1.83),(-.442,-.171,1.633)], [.14,.126,.095,.004]),
        ([(.32,-.145,2.037),(.422,-.19,1.912),(.46,-.19,1.681),(.423,-.229,1.52)], [.125,.112,.087,.004]),
        ([(-.43,-.09,1.95),(-.49,-.09,1.777),(-.49,-.156,1.551),(-.393,-.254,1.473)], [.103,.104,.08,.004]),
    ]
    for idx,(points,widths) in enumerate(bang_specs):
        sweep(f"WhaleBang.{idx:02}",points,widths,[.057,.065,.048,.003],gradient=False,samples=12)
    for s,label in ((1,"L"),(-1,"R")):
        sweep(f"WhaleSideCurl.{label}",[(s*.46,-.065,1.92),(s*.55,-.085,1.65),(s*.525,-.085,1.383),
              (s*.52,-.105,1.16),(s*.615,-.095,1.03),(s*.54,-.11,.935),(s*.455,-.13,1.02)],
              [.11,.15,.123,.135,.11,.075,.004],[.072,.075,.064,.061,.06,.044,.003],hair_weight=True,samples=9)
        sweep(f"WhaleOuterCascade.{label}",[(s*.45,.09,1.91),(s*.6,.14,1.55),(s*.68,.14,1.20),
              (s*.765,.09,.947),(s*.695,.06,.72),(s*.6,.065,.77),(s*.63,.04,.87)],
              [.10,.16,.14,.12,.082,.05,.003],[.07,.074,.066,.054,.046,.025,.002],hair_weight=True,samples=9)
        # Whale-fin ear ornament, thick curved blade, separate white underfrill.
        fin_controls=[(s*.48,.06,1.80),(s*.62,.04,1.675),(s*.83,.005,1.59)]
        sweep(f"WhaleEarFin.{label}",fin_controls,[.096,.123,.003],[.045,.037,.002],"WhaleHairRoyal",gradient=False,samples=10)
        sweep(f"WhaleEarFrill.{label}",[(s*.5,.07,1.74),(s*.64,.052,1.618),(s*.785,.015,1.566)],
              [.07,.083,.003],[.027,.025,.002],"WhaleLace",gradient=False,samples=10)
    for idx,x in enumerate((-.4,-.22,0,.22,.4)):
        side=-1 if x<0 else 1
        controls=[(x,.26,1.955),(x*1.30,.34,1.65),(x*1.42,.31,1.27),
                  (x*1.52+side*.065,.28,.93),(x*1.37+side*.065,.27,.59),
                  (x*1.13,.24,.54),(x*1.02,.23,.66)]
        sweep(f"WhaleBackCascade.{idx:02}",controls,[.145,.17,.155,.135,.095,.06,.003],
              [.10,.105,.098,.08,.06,.04,.002],hair_weight=True,samples=8)
    # Large arch follows the illustration without being a rigid segmented rod.
    sweep("WhaleAhoge",[(.07,.025,2.103),(.19,.035,2.292),(.12,.04,2.444),
           (-.09,.02,2.485),(-.28,.0,2.382),(-.31,-.015,2.267),(-.23,-.025,2.247)],
          [.035,.034,.031,.028,.024,.016,.002],[.024,.023,.021,.019,.016,.012,.001],
          gradient=False,samples=10)
    # A white curved hairband and a softly pleated frill, with thickness all round.
    band=[]
    for i in range(19):
        a=math.pi*i/18
        band.append((.562*math.cos(a),-.055,1.708+.475*math.sin(a)))
    sweep("WhaleMaidBand",band,[.023]*19,[.030]*19,"WhaleLace",gradient=False,samples=2)
    for i in range(15):
        a=.12+(math.pi-.24)*i/14
        location=(.571*math.cos(a),-.006,1.708+.486*math.sin(a))
        petal=proto.ellipsoid(f"WhaleMaidPleat.{i:02}",location,(.065,.043,.097),"head","WhaleLace",16,10)
        # Radial lean makes the frill read as a continuous headband from 3/4.
        petal.rotation_euler.y=math.pi/2-a
        bpy.context.view_layer.objects.active=petal
        bpy.ops.object.transform_apply(location=False,rotation=True,scale=False)
        HEAD_OBJECTS.append(petal)
    # Cyan side ribbon is a paired folded bow, not a single dangling sphere.
    for idx,x in enumerate((.49,.625)):
        bow=proto.ellipsoid(f"WhaleRibbonWing.{idx}",(x,-.047,1.785),(.085,.037,.059),"head","WhaleRibbon",20,12)
        bow.rotation_euler.y=(.25 if idx==0 else -.25)
        HEAD_OBJECTS.append(bow)
    knot=proto.ellipsoid("WhaleRibbonKnot",(.557,-.083,1.785),(.026,.025,.035),"head","WhaleRibbon",16,10)
    HEAD_OBJECTS.append(knot)


def build_face():
    for label,s in (("L",1),("R",-1)):
        semantic=f"eye_{label}"
        cx=s*.223; cz=1.642
        tilt=s*.10
        face_disk(f"WhaleEyeRim.{label}",cx,cz,.157,.153,.0038,"WhaleEyeLash",semantic,True,tilt)
        face_disk(f"WhaleEyeWhite.{label}",cx,cz-.005,.147,.138,.0058,"WhaleEyeWhite",semantic,True,tilt)
        face_disk(f"WhaleIrisOuter.{label}",cx+s*.006,cz-.015,.101,.126,.0078,"WhaleIrisDeep",semantic)
        face_disk(f"WhaleIrisBlue.{label}",cx+s*.006,cz-.02,.089,.112,.0098,"WhaleIrisBlue",semantic)
        face_disk(f"WhaleIrisGlow.{label}",cx+s*.006,cz-.065,.072,.061,.0115,"WhaleIrisGlow",semantic)
        face_disk(f"WhalePupil.{label}",cx+s*.006,cz-.006,.040,.070,.0132,"WhalePupil",semantic)
        face_disk(f"WhaleHighlightLarge.{label}",cx-.026,cz+.053,.026,.030,.0153,"WhaleHighlight",semantic,rings=3,segments=24)
        face_disk(f"WhaleHighlightSmall.{label}",cx+.036,cz-.069,.012,.013,.0153,"WhaleHighlight",semantic,rings=3,segments=16)
        # Lashes are along the actual almond contour; brow stays independent.
        points=[]
        for i in range(16):
            a=math.pi*i/15
            x=cx+.157*math.cos(a)
            z=cz+.153*math.sin(a)*(1-.25*abs(math.cos(a)))+tilt*(x-cx)
            points.append((x,z))
        face_line(f"WhaleUpperLash.{label}",points,.008,.011,"WhaleEyeLash",semantic)
        cornerx=cx+s*.145; cornerz=cz+tilt*s*.145+.035
        for idx in range(2):
            face_line(f"WhaleOuterLash.{label}.{idx}",[(cornerx-s*.018,cornerz),
                      (cornerx+s*(.012+.011*idx),cornerz+.025+.017*idx)],.0055,.012,"WhaleEyeLash",semantic)
        face_line(f"WhaleBrow.{label}",[(cx-.102,1.833),(cx-.05,1.852),(cx+.02,1.848),(cx+.09,1.824)],
                  .007,.0038,"WhaleEyeBrow",f"brow_{label}")
        face_disk(f"WhaleBlush.{label}",s*.319,1.482,.072,.021,.0024,"WhaleEyeBlush",f"blush_{label}",rings=3,segments=24)
    face_line("WhaleClosedMouth",[(-.047,1.421),(-.033,1.408),(-.014,1.405),(.006,1.412),(.027,1.41),(.046,1.42)],
              .0032,.0047,"WhaleMouth","mouth")
    # Tiny nose highlight avoids the open O mouth or realistic protruding nose.
    face_disk("WhaleNoseLight",0,1.515,.008,.006,.0035,"WhaleHighlight","nose",rings=2,segments=16)
    # Join while preserving semantic labels in vertex attributes for readback audits.
    # Material labels alone cannot distinguish a pupil on the left from the right.
    semantics={name:idx+1 for idx,name in enumerate(sorted(set(FACE_SEMANTICS.values())))}
    for obj in FACE_OBJECTS:
        attr=obj.data.attributes.new("whale_semantic","INT","POINT")
        for value in attr.data:value.value=semantics[FACE_SEMANTICS[obj.name]]
    bpy.ops.object.select_all(action="DESELECT")
    for obj in FACE_OBJECTS:obj.select_set(True)
    bpy.context.view_layer.objects.active=FACE_OBJECTS[0]
    bpy.ops.object.join()
    face=bpy.context.object; face.name="FaceFeatures"
    face.data.name="WhaleFaceConformalGeometry"
    basis=face.shape_key_add(name="Basis",from_mix=False)
    baseline=[face.matrix_world@v.co for v in face.data.vertices]
    inverse=face.matrix_world.inverted()
    attr=face.data.attributes["whale_semantic"]
    for name,semantic in (("Blink_L","eye_L"),("Blink_R","eye_R"),("Smile","mouth")):
        key=face.shape_key_add(name=name,from_mix=False);key.relative_key=basis;key.value=0
        for idx,world in enumerate(baseline):
            if attr.data[idx].value!=semantics[semantic]:continue
            new=world.copy()
            offset=face_surface(world.x,world.z)-world.y
            if name.startswith("Blink"):
                # Closed eye is a curved thin line. Reproject on the face rather than
                # only compressing Z and leaving the old spherical depth behind.
                cx=.223 if semantic=="eye_L" else -.223
                new.z=1.642-.011+.025*(1-((world.x-cx)/.157)**2)+(world.z-1.642)*.015
            else:
                new.x*=1.33
                new.z=1.417+(world.z-1.417)*1.65
            new.y=face_surface(new.x,new.z)-offset
            key.data[idx].co=inverse@new
    face.data.shape_keys.key_blocks["Smile"].value=.2
    return face,semantics


def expression_audit(face,semantics):
    keys=face.data.shape_keys.key_blocks;basis=keys["Basis"]
    attr=face.data.attributes.get("whale_semantic")
    inverse={idx:name for name,idx in semantics.items()}
    result={}
    for name,expected in (("Blink_L","eye_L"),("Blink_R","eye_R"),("Smile","mouth")):
        changed={};maximum=0
        for idx in range(len(basis.data)):
            delta=(keys[name].data[idx].co-basis.data[idx].co).length
            if delta<=1e-8:continue
            semantic=inverse[attr.data[idx].value]
            changed[semantic]=changed.get(semantic,0)+1;maximum=max(maximum,delta)
            if semantic!=expected:raise RuntimeError(f"{name} leaked into {semantic}")
        if not changed:raise RuntimeError(f"{name} has no edited vertices")
        result[name]={"changed_vertices_by_region":changed,"max_delta":maximum,"default":keys[name].value}
    return result


def neutral_bounds(objects):
    coordinates=[obj.matrix_world@v.co for obj in objects if obj.type=="MESH" for v in obj.data.vertices]
    return {"min":[min(v[i] for v in coordinates) for i in range(3)],
            "max":[max(v[i] for v in coordinates) for i in range(3)],"vertices":len(coordinates)}


def facial_depth_audit(face):
    """Numerically reject facial vertices inside skin or visibly floating far out.

    Every authored full key shape is tested, not just a few face fixtures. GPU
    edge/occlusion and interpolated morphs still require separate review.
    """
    result={}
    for key in face.data.shape_keys.key_blocks:
        offsets=[]
        for vertex in key.data:
            world=face.matrix_world@vertex.co
            offsets.append(face_surface(world.x,world.z)-world.y)
        entry={"vertices":len(offsets),"min_front_offset_m":min(offsets),
               "max_front_offset_m":max(offsets)}
        entry["passed"]=entry["min_front_offset_m"]>.0002 and entry["max_front_offset_m"]<.020
        if not entry["passed"]:raise RuntimeError(f"Face depth failure {key.name}: {entry}")
        result[key.name]=entry
    return result


def prepare_uv_and_tangents():
    """Provide real packed UV charts and an explicit triangulated tangent basis.

    Existing sphere UVs are retained. Procedural closed tubes and conformal face
    solids receive angle-aware packed surface charts, never an empty/constant UV
    placeholder. Edit-mode face triangulation preserves all vertex shape keys
    and skin weights while avoiding the tangent calculator's n-gon restriction.
    """
    records={}
    for obj in list(bpy.context.scene.objects):
        if obj.type!="MESH":continue
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True);bpy.context.view_layer.objects.active=obj
        bpy.ops.object.mode_set(mode="EDIT");bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.quads_convert_to_tris(quad_method="BEAUTY",ngon_method="BEAUTY")
        bpy.ops.object.mode_set(mode="OBJECT")
        generated=not obj.data.uv_layers
        if generated:
            obj.data.uv_layers.new(name="CandidateSurfaceUV")
            bpy.ops.object.mode_set(mode="EDIT");bpy.ops.mesh.select_all(action="SELECT")
            bpy.ops.uv.smart_project(angle_limit=math.radians(66),island_margin=.018,
                                     area_weight=.25,correct_aspect=True,scale_to_bounds=True)
            bpy.ops.object.mode_set(mode="OBJECT")
        layer=obj.data.uv_layers.active
        if layer is None or len(layer.data)!=len(obj.data.loops):
            raise RuntimeError(f"{obj.name}: missing UV loops")
        uvs=[tuple(loop.uv) for loop in layer.data]
        if not uvs or not all(math.isfinite(x) for uv in uvs for x in uv):
            raise RuntimeError(f"{obj.name}: empty/non-finite UV values")
        extent=[max(uv[i] for uv in uvs)-min(uv[i] for uv in uvs) for i in range(2)]
        if min(extent)<.001:raise RuntimeError(f"{obj.name}: UV chart has collapsed extent {extent}")
        obj.data.calc_tangents(uvmap=layer.name)
        tangents=[tuple(loop.tangent) for loop in obj.data.loops]
        if not all(math.isfinite(x) for tangent in tangents for x in tangent):
            raise RuntimeError(f"{obj.name}: non-finite tangent")
        records[obj.name]={"uv_layer":layer.name,"loops":len(uvs),"uv_extent":extent,
                           "method":"angle_aware_packed_surface_charts" if generated else "retained_primitive_spherical_uv",
                           "triangulated":all(len(poly.vertices)==3 for poly in obj.data.polygons),
                           "tangent_calculation_passed":True}
    return records


def glb_surface_attribute_audit(glb):
    """Require exported UV/normal/tangent attributes on every material primitive."""
    import struct
    raw=Path(glb).read_bytes();jlen=struct.unpack_from("<I",raw,12)[0]
    document=json.loads(raw[20:20+jlen]);offset=20+jlen
    blen,_=struct.unpack_from("<II",raw,offset);binary=raw[offset+8:offset+8+blen]
    records=[]
    for mesh in document["meshes"]:
        for index,primitive in enumerate(mesh["primitives"]):
            attributes=primitive["attributes"]
            required={"POSITION":3,"NORMAL":3,"TANGENT":4,"TEXCOORD_0":2}
            missing=sorted(set(required)-set(attributes))
            if missing:raise RuntimeError(f"{mesh.get('name')}/{index}: missing exported attributes {missing}")
            count=document["accessors"][attributes["POSITION"]]["count"]
            facts={}
            for name,components in required.items():
                accessor=document["accessors"][attributes[name]]
                if accessor["count"]!=count or accessor["componentType"]!=5126 or accessor["type"]!=f"VEC{components}":
                    raise RuntimeError(f"{mesh.get('name')}/{index}: incompatible {name} accessor")
                view=document["bufferViews"][accessor["bufferView"]]
                start=view.get("byteOffset",0)+accessor.get("byteOffset",0)
                stride=view.get("byteStride",components*4)
                values=[struct.unpack_from("<"+"f"*components,binary,start+i*stride) for i in range(count)]
                if not all(math.isfinite(x) for value in values for x in value):
                    raise RuntimeError(f"{mesh.get('name')}/{index}: non-finite exported {name}")
                extent=[max(v[i] for v in values)-min(v[i] for v in values) for i in range(components)]
                if name=="TEXCOORD_0" and min(extent)<.0001:
                    raise RuntimeError(f"{mesh.get('name')}/{index}: empty UV chart {extent}")
                facts[name]={"vertices":count,"finite":True,"extent":extent}
            records.append({"mesh":mesh.get("name"),"primitive":index,"attributes":facts})
    return {"mesh_count":len(document["meshes"]),"primitive_count":len(records),"all_primitives_passed":True,
            "required_attributes":["POSITION","NORMAL","TANGENT","TEXCOORD_0"],"primitives":records}


def exported_morph_audit(glb):
    """Read glTF accessor bytes and prove each exported morph is independent.

    Import strips custom semantic attributes; direct GLB channel geometry is
    still checked for positive/negative eye side and mouth region only.
    """
    import struct
    raw=Path(glb).read_bytes();jlen=struct.unpack_from("<I",raw,12)[0]
    document=json.loads(raw[20:20+jlen]);offset=20+jlen
    blen,btype=struct.unpack_from("<II",raw,offset);binary=raw[offset+8:offset+8+blen]
    def accessor(index):
        value=document["accessors"][index]
        if value["componentType"]!=5126 or value["type"]!="VEC3":raise RuntimeError("Expected float VEC3")
        result=[(0.,0.,0.)]*value["count"]
        if "bufferView" in value:
            view=document["bufferViews"][value["bufferView"]]
            start=view.get("byteOffset",0)+value.get("byteOffset",0)
            stride=view.get("byteStride",12)
            result=[struct.unpack_from("<fff",binary,start+i*stride) for i in range(value["count"])]
        sparse=value.get("sparse")
        if sparse:
            indices=sparse["indices"]; values=sparse["values"]
            iview=document["bufferViews"][indices["bufferView"]]
            vview=document["bufferViews"][values["bufferView"]]
            istart=iview.get("byteOffset",0)+indices.get("byteOffset",0)
            vstart=vview.get("byteOffset",0)+values.get("byteOffset",0)
            ifmt,isize={5121:("<B",1),5123:("<H",2),5125:("<I",4)}[indices["componentType"]]
            for i in range(sparse["count"]):
                vertex=struct.unpack_from(ifmt,binary,istart+i*isize)[0]
                result[vertex]=struct.unpack_from("<fff",binary,vstart+i*12)
        return result
    report={}
    for mesh in document["meshes"]:
        names=mesh.get("extras",{}).get("targetNames",[])
        if not names:continue
        for primitive in mesh["primitives"]:
            baseline=accessor(primitive["attributes"]["POSITION"])
            for name,target in zip(names,primitive["targets"]):
                delta=accessor(target["POSITION"])
                record=report.setdefault(name,{"changed_vertices":0,"opposite_eye_changed_vertices":0,"mouth_changed_vertices":0})
                for point,change in zip(baseline,delta):
                    if max(abs(x) for x in change)<=1e-8:continue
                    record["changed_vertices"]+=1
                    # Detail meshes were authored with identity transforms and
                    # global vertices. GLTF Y is original Blender Z.
                    world_x=point[0]
                    world_z=point[1]
                    if name=="Blink_L" and world_x<0:record["opposite_eye_changed_vertices"]+=1
                    if name=="Blink_R" and world_x>0:record["opposite_eye_changed_vertices"]+=1
                    if name.startswith("Blink") and world_z<1.45:record["mouth_changed_vertices"]+=1
                    if name=="Smile" and world_z>1.45:raise RuntimeError("Exported smile edited eye/nose geometry")
    for name,record in report.items():
        if not record["changed_vertices"] or record["opposite_eye_changed_vertices"] or record["mouth_changed_vertices"]:
            raise RuntimeError(f"Exported expression isolation failed {name}: {record}")
    return report


def readback(blend,glb,source,semantics):
    result={}
    for kind in ("author","glb"):
        if kind=="author":bpy.ops.wm.open_mainfile(filepath=str(blend))
        else:
            bpy.ops.object.select_all(action="SELECT");bpy.ops.object.delete(use_global=False)
            for action in list(bpy.data.actions):bpy.data.actions.remove(action)
            bpy.data.orphans_purge(do_local_ids=True,do_linked_ids=True,do_recursive=True)
            bpy.ops.import_scene.gltf(filepath=str(glb))
        arm=[o for o in bpy.context.scene.objects if o.type=="ARMATURE"]
        if len(arm)!=1:raise RuntimeError(f"{kind}: expected one armature")
        arm=arm[0];actions={a.name:a for a in bpy.data.actions}
        if not {"idle","wave","sit"}<=set(actions):raise RuntimeError(f"{kind}: actions missing")
        face=bpy.data.objects.get("FaceFeatures")
        names=[k.name for k in face.data.shape_keys.key_blocks]
        if not {"Blink_L","Blink_R","Smile"}<=set(names):raise RuntimeError(f"{kind}: expressions missing")
        expr=expression_audit(face,semantics) if kind=="author" else exported_morph_audit(glb)
        depth=facial_depth_audit(face)
        clips={}
        for clip,end in (("idle",120),("wave",75),("sit",90)):
            arm.animation_data_create();arm.animation_data.action=actions[clip];error=0
            for frame in range(1,end+1):
                bpy.context.scene.frame_set(frame);bpy.context.view_layer.update()
                for label,x in (("L",.145),("R",-.145)):
                    point=arm.matrix_world@arm.pose.bones[f"foot.{label}"].head
                    error=max(error,(point-Vector((x,0,.12))).length)
            if error>1e-5:raise RuntimeError(f"{kind}/{clip}: foot slip {error}")
            clips[clip]={"samples":end,"max_world_foot_anchor_error":error}
        uv_meshes=[]
        for obj in bpy.context.scene.objects:
            if obj.type!="MESH":continue
            if not obj.data.uv_layers:raise RuntimeError(f"{kind}/{obj.name}: UV missing on readback")
            layer=obj.data.uv_layers.active
            uvs=[tuple(loop.uv) for loop in layer.data]
            extent=[max(v[i] for v in uvs)-min(v[i] for v in uvs) for i in range(2)]
            if min(extent)<.001:raise RuntimeError(f"{kind}/{obj.name}: UV collapsed on readback")
            uv_meshes.append({"mesh":obj.name,"loops":len(uvs),"uv_extent":extent})
        result[kind]={"actions":list(actions),"bones":len(arm.data.bones),"expressions":names,
                      "expression_isolation":expr,"facial_depth":depth,"clips":clips}
        result[kind]["uv_readback"]={"mesh_count":len(uv_meshes),"all_meshes_passed":True,"meshes":uv_meshes}
    proto.write_json(source/"source-and-export-audit.json",{"schema_version":1,
        "status":"numeric_readback_pass_visual_pending","source_sha256":proto.digest(blend),
        "glb_sha256":proto.digest(glb),"records":result,"surface_attributes":glb_surface_attribute_audit(glb),
        "limitations":["No source numeric audit approves likeness or facial/hair overlaps.",
                       "Temporary proxy body/sit; no cloth or fingertip contact approval.",
                       "Actual Godot front, both 3/4, back, blinking and motion review remains necessary."]})


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--project",type=Path,default=ROOT)
    args=parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    source=args.project/"art/3d/whale-girl-head-r1";export=args.project/"assets/3d/whale-girl-head-r1"
    source.mkdir(parents=True,exist_ok=True);export.mkdir(parents=True,exist_ok=True)
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.object.select_all(action="SELECT");bpy.ops.object.delete(use_global=False)
    for action in list(bpy.data.actions):bpy.data.actions.remove(action)
    proto.MESHES.clear();proto.MATERIALS.clear();proto.REST.clear()
    scene=bpy.context.scene;scene.render.fps=30;scene.frame_start=1;scene.frame_end=120
    arm=proto.build_rig();proto.build_geometry();remove_proxy_head()
    palette={"WhaleSkin":"FFE8DE","WhaleHairRoyal":"4563A5","WhaleHairMid":"5E9AD5", "WhaleHairCyan":"70C7EF",
             "WhaleLace":"FFF9FF","WhaleRibbon":"49AFE9","WhaleEyeLash":"1D2949","WhaleEyeBrow":"5D526F",
             "WhaleEyeWhite":"FFF8FB","WhaleIrisDeep":"3C4F92","WhaleIrisBlue":"4E7EC7","WhaleIrisGlow":"7BCDF5",
             "WhalePupil":"223B71","WhaleHighlight":"FFFFFF","WhaleMouth":"8C4556","WhaleEyeBlush":"F3B6B7"}
    for name,color in palette.items():proto.material(name,linear_hex(color))
    build_head_geometry();face,semantics=build_face()
    uv_records=prepare_uv_and_tangents()
    for obj in list(bpy.context.scene.objects):
        if obj.type=="MESH":
            obj.parent=arm;modifier=obj.modifiers.new("WhaleCandidateSkin","ARMATURE");modifier.object=arm
    animations=proto.make_actions(arm);arm.animation_data.action=bpy.data.actions["idle"];scene.frame_set(1)
    for obj in bpy.context.scene.objects:obj.select_set(True)
    bpy.context.view_layer.objects.active=arm
    blend=source/"character.blend";glb=export/"character.glb"
    bounds=neutral_bounds(list(bpy.context.scene.objects))
    expressions=expression_audit(face,semantics)
    facial_depth=facial_depth_audit(face)
    bpy.data.orphans_purge(do_local_ids=True,do_linked_ids=True,do_recursive=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    options={"export_format":"GLB","export_yup":True,"export_animations":True,"export_animation_mode":"ACTIONS",
             "export_def_bones":True,"export_morph":True,"export_morph_normal":True,"export_morph_animation":False,
             "export_skins":True,"export_all_influences":False,"export_optimize_animation_size":False,
             "export_apply":False,"export_cameras":False,"export_lights":False,
             "export_texcoords":True,"export_normals":True,"export_tangents":True}
    bpy.ops.export_scene.gltf(filepath=str(glb),**options)
    weights=[];unweighted=[]
    for obj in bpy.context.scene.objects:
        if obj.type!="MESH":continue
        for vertex in obj.data.vertices:
            weights.append(abs(sum(g.weight for g in vertex.groups)-1))
            if not vertex.groups:unweighted.append([obj.name,vertex.index])
    if unweighted or max(weights)>1e-5:raise RuntimeError("Bad skin weights")
    bones=[{"name":b.name,"parent":b.parent.name if b.parent else None,"length":proto.REST[b.name]["length"],
            "head_blender":list(b.head_local),"tail_blender":list(b.tail_local),"deform":b.use_deform} for b in arm.data.bones]
    contract={"schema_version":1,"status":"candidate_art_and_runtime_review_pending","character_identity":"q_whale_girl_head_candidate",
        "purpose":"Whale-girl volumetric head candidate on a temporary rigged body; never formal production adoption.",
        "author_coordinates":{"up":"+Z","front":"-Y","units":"meters"},
        "runtime_coordinates":{"up":"+Y","front":"+Z","units":"meters","conversion":"(x,y,z)->(x,z,-y)"},
        "ground_y":0,"head_top_y":2.188,"total_height_y":bounds["max"][2],"forward_axis":[0,0,1],
        "nodes":{"armature":"Armature","face":"FaceFeatures"},"bones":bones,
        "expressions":{"Blink_L":{"range":[0,1],"default":0},"Blink_R":{"range":[0,1],"default":0},"Smile":{"range":[0,1],"default":.2}},
        "materials":list(proto.MATERIALS),"material_alpha":"all opaque geometry; no bitmap cards",
        "animations":animations,"rise":"play sit backwards; do not scale limbs",
        "sit_contact":{"stool_top_y":.35,"stool_center":[0,.175,-.18],"stool_size":[.56,.35,.36],
                       "foot_anchor_L":[.145,.12,0],"foot_anchor_R":[-.145,.12,0],"note":"Temporary proxy contact; not a final whale-girl body/clothing rig."},
        "audit":{"max_weight_sum_error":max(weights),"unweighted_vertices":unweighted,"all_deform_bones":True,
                 "structure":proto.glb_structure(glb),"expression_isolation":expressions,
                 "facial_depth":facial_depth,"neutral_bounds_blender":bounds,
                 "uv_and_tangents":uv_records,"exported_surface_attributes":glb_surface_attribute_audit(glb)},
        "source_sha256":proto.digest(blend),"glb_sha256":proto.digest(glb),"visual_status":"pending_actual_godot_review",
        "limits":["Candidate head likeness pending review","temporary prototype torso/limbs and sit animation",
                  "Hair is geometry with stepped color bands, not finished painterly shading","No hair collisions, fingers or cloth simulation"]}
    proto.write_json(source/"rig-contract.json",contract)
    proto.write_json(source/"rebuild-recipe.json",{"schema_version":1,"status":"candidate_art_and_runtime_review_pending",
        "script":"tools/create_whale_head_candidate.py","script_sha256":proto.digest(__file__),
        "prototype_dependency":"tools/create_toon_prototype.py","prototype_dependency_sha256":proto.digest(ROOT/"tools/create_toon_prototype.py"),
        "references":[{"file":"assets/idle.png","sha256":proto.digest(ROOT/"assets/idle.png"),
                       "usage":"Appearance guidance only; no image texture or billboard in candidate"}],
        "blender_version":bpy.app.version_string,"blender_build_hash":bpy.app.build_hash.decode(),
        "command":["D:/tool/blender/blender.exe","--background","--factory-startup","--python","tools/create_whale_head_candidate.py"],
        "author":str(blend.relative_to(ROOT)).replace("\\","/"),"export":str(glb.relative_to(ROOT)).replace("\\","/"),
        "export_options":options,"source_sha256":proto.digest(blend),"glb_sha256":proto.digest(glb),
        "design":{"head":"Opaque volumetric broad-cheek shell", "eyes":"Closed conformal solids, blue nested iris/glints; isolated independent blink",
                  "hair":"Tapered closed curved locks with true front/side/back volume", "details":"White maid pleats, cyan bow, curved ahoge, whale-fin ears",
                  "body":"Temporary existing 21-bone proxy, unchanged technical clips"},
        "validation":"Source and GLB readback audit; actual GPU multi-angle/art approval separate"})
    readback(blend,glb,source,semantics)
    print("WHALE_HEAD_CANDIDATE_COMPLETE "+json.dumps({"blend":str(blend),"glb":str(glb),"bounds":bounds,"structure":contract["audit"]["structure"]}))


if __name__=="__main__":main()
