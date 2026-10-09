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
SWEEP_AUDITS = {}
BLINK_TARGETS = {}


def linear_hex(value):
    channels = [int(value[i:i+2], 16) / 255 for i in (0, 2, 4)]
    return tuple(x / 12.92 if x <= .04045 else ((x+.055)/1.055)**2.4 for x in channels) + (1.,)


def hair_gradient(z):
    royal=linear_hex("4563A5");mid=linear_hex("5E9AD5");cyan=linear_hex("70C7EF")
    t=max(0,min(1,(1.58-z)/.95))
    t=t*t*(3-2*t)
    if t<.5:a,b,k=royal,mid,t*2
    else:a,b,k=mid,cyan,(t-.5)*2
    return tuple(a[c]*(1-k)+b[c]*k for c in range(4))


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


def closed_lash_target(obj,points,radius,depth):
    """Corresponding full-thickness line geometry for a continuous lid morph."""
    mapped=[(x,face_surface(x,z)-depth,z) for x,z in points]
    temporary=proto.tube("TemporaryClosedLashTarget",mapped,[radius]*len(mapped),[{"head":1}]*len(mapped),"WhaleEyeLash",8)
    target=[]
    for vertex in temporary.data.vertices:
        ring=vertex.index//8
        bulge=(vertex.co.y-mapped[ring][1])*.18
        target.append(Vector((vertex.co.x,face_surface(vertex.co.x,vertex.co.z)-depth+bulge,vertex.co.z)))
    if len(target)!=len(obj.data.vertices):raise RuntimeError("Closed lid topology must match open lash")
    BLINK_TARGETS[obj.name]=target
    if temporary in proto.MESHES:proto.MESHES.remove(temporary)
    bpy.data.objects.remove(temporary,do_unlink=True)


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
    tangents=[(points[min(idx+1,len(points)-1)]-points[max(idx-1,0)]).normalized() for idx in range(len(points))]
    arc=[0.0]
    for idx in range(1,len(points)):arc.append(arc[-1]+(points[idx]-points[idx-1]).length)
    requested=[];local_limits=[];curvature_radii=[]
    for idx,p in enumerate(points):
        f=idx/samples
        segment=min(int(f),len(widths)-2); amount=f-segment
        width=widths[segment]*(1-amount)+widths[segment+1]*amount
        depth=depths[segment]*(1-amount)+depths[segment+1]*amount
        requested.append((width,depth))
        curvature=0.0
        if 0<idx<len(points)-1:
            a=(points[idx]-points[idx-1]).length
            b=(points[idx+1]-points[idx]).length
            c=(points[idx+1]-points[idx-1]).length
            cross=(points[idx]-points[idx-1]).cross(points[idx+1]-points[idx]).length
            curvature=2*cross/(a*b*c) if a*b*c>1e-12 else 0
        radius=1/curvature if curvature>1e-5 else 1000.
        curvature_radii.append(radius)
        # The whole elliptical section must remain inside the bend radius; the
        # previous broad pipe folded through itself at the hook-shaped tips.
        local_limits.append(radius*.56)
    # A returning curl can overlap another section even when each local bend is
    # regular. Limit non-local near passes according to true arc length, without
    # shrinking ordinary straight neighboring rings.
    nonlocal_limits=[1000.]*len(points);near_passes=0
    for a in range(len(points)):
        for b in range(a+1,len(points)):
            along=arc[b]-arc[a];distance=(points[b]-points[a]).length
            if along<.15 or distance>=along*.65:continue
            limit=.42*distance
            if limit<max(requested[a]) or limit<max(requested[b]):near_passes+=1
            nonlocal_limits[a]=min(nonlocal_limits[a],limit)
            nonlocal_limits[b]=min(nonlocal_limits[b],limit)
    limits=[min(local_limits[i],nonlocal_limits[i]) for i in range(len(points))]
    # Conservative neighbor smoothing avoids a sudden thick/thin collar around
    # a tightly constrained curl. Never increase a unsafe radius bound.
    for _ in range(3):
        limits=[min(limits[i],limits[max(i-1,0)]*1.18,limits[min(i+1,len(limits)-1)]*1.18) for i in range(len(limits))]
    vertices=[]; faces=[]; weights=[];actual=[];frames=[]
    u=tangents[0].cross(Vector((0,1,0)))
    if u.length<1e-5:u=tangents[0].cross(Vector((1,0,0)))
    u.normalize();previous=tangents[0]
    for idx,p in enumerate(points):
        tangent=tangents[idx]
        if idx:
            # Parallel transport is continuous through vertical/horizontal curls,
            # unlike rebuilding a frame against a fixed axis at every ring.
            transport=previous.rotation_difference(tangent)
            u=transport@u
            u=(u-tangent*u.dot(tangent)).normalized()
        v=tangent.cross(u).normalized();previous=tangent
        frames.append((u.copy(),v.copy(),tangent.copy()))
        width,depth=requested[idx]
        reduction=min(1,limits[idx]/max(width,depth))
        width*=reduction;depth*=reduction;actual.append((width,depth))
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
    obj=mesh_object(name,vertices,faces,"WhaleHairGradient" if gradient else material)
    for group in list(obj.vertex_groups):obj.vertex_groups.remove(group)
    for bone in ("head","hair"):
        group=obj.vertex_groups.new(name=bone)
        for idx,weight in enumerate(weights):
            if weight[bone]>.000001:group.add([idx],weight[bone],"REPLACE")
    if gradient:
        # Linear vertex colors survive GLTF COLOR_0; runtime must honor COLOR.
        attribute=obj.data.color_attributes.new(name="WhaleHairGradient",type="FLOAT_COLOR",domain="POINT")
        obj.data.color_attributes.active_color=attribute
        obj.data.color_attributes.render_color_index=0
        for idx,vertex in enumerate(obj.data.vertices):
            attribute.data[idx].color=hair_gradient(vertex.co.z)
    # Detect folded side polygons directly against the expected outward radial
    # direction, and prove ring frames neither flip nor lose orthogonality.
    inward=[];degenerate=[]
    for idx,face in enumerate(faces[:-2]):
        ring=idx//sides
        coordinates=[vertices[i] for i in face]
        normal=(coordinates[1]-coordinates[0]).cross(coordinates[2]-coordinates[0])
        radial=sum(coordinates,Vector())/len(coordinates)-(points[ring]+points[ring+1])*.5
        if normal.length<1e-11:degenerate.append(idx)
        elif normal.dot(radial)<-1e-10:inward.append(idx)
    max_twist=0.
    for idx in range(1,len(frames)):
        transported=tangents[idx-1].rotation_difference(tangents[idx])@frames[idx-1][0]
        max_twist=max(max_twist,transported.angle(frames[idx][0]))
    nonlocal_clearances=[];nonlocal_ratios=[]
    for a in range(len(points)):
        for b in range(a+1,len(points)):
            along=arc[b]-arc[a];distance=(points[b]-points[a]).length
            if along<.15 or distance>=along*.65:continue
            section=max(actual[a])+max(actual[b])
            nonlocal_clearances.append(distance-section)
            nonlocal_ratios.append(section/distance if distance else float("inf"))
    if nonlocal_clearances and min(nonlocal_clearances)<-1e-8:
        raise RuntimeError(f"{name}: returning curl overlaps nonlocal enclosing sections")
    if inward or degenerate:raise RuntimeError(f"{name}: folded/degenerate sweep {len(inward)}/{len(degenerate)}")
    SWEEP_AUDITS[name]={"rings":len(points),"parallel_transport_max_residual_radians":max_twist,
        "minimum_curve_radius_m":min(curvature_radii),"constrained_rings":sum(max(a)<max(r)*.999 for a,r in zip(actual,requested)),
        "nonlocal_near_pass_pairs":near_passes,"inward_side_polygons":inward,"degenerate_side_polygons":degenerate,
        "minimum_nonlocal_centerline_enclosure_clearance_m":min(nonlocal_clearances) if nonlocal_clearances else None,
        "maximum_nonlocal_enclosing_section_to_separation_ratio":max(nonlocal_ratios) if nonlocal_ratios else 0.,
        "maximum_section_to_curve_radius_ratio":max(max(a)/r for a,r in zip(actual,curvature_radii)),
        "gradient":"linear_COLOR_0_smoothstep_height" if gradient else "constant material"}
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
        upper=face_line(f"WhaleUpperLash.{label}",points,.008,.011,"WhaleEyeLash",semantic)
        closed_points=[]
        for x,_ in points:
            z=1.631+.026*(1-((x-cx)/.157)**2)+tilt*(x-cx)*.3
            closed_points.append((x,z))
        closed_lash_target(upper,closed_points,.010,.013)
        cornerx=cx+s*.145; cornerz=cz+tilt*s*.145+.035
        for idx in range(2):
            lash=face_line(f"WhaleOuterLash.{label}.{idx}",[(cornerx-s*.018,cornerz),
                      (cornerx+s*(.012+.011*idx),cornerz+.025+.017*idx)],.0055,.012,"WhaleEyeLash",semantic)
            closed_corner=1.631+.026*(1-(.145/.157)**2)+tilt*s*.145*.3
            closed_lash_target(lash,[(cornerx-s*.016,closed_corner+.001+idx*.003),
                    (cornerx+s*(.016+.011*idx),closed_corner+.020+.018*idx)],.006,.014)
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
        patch=obj.data.attributes.new("whale_eye_patch","INT","POINT")
        target=obj.data.attributes.new("whale_blink_target","FLOAT_VECTOR","POINT")
        semantic=FACE_SEMANTICS[obj.name]
        is_patch=semantic.startswith("eye_") and obj.name not in BLINK_TARGETS
        side=1 if semantic=="eye_L" else -1 if semantic=="eye_R" else 0
        for idx,vertex in enumerate(obj.data.vertices):
            patch.data[idx].value=side if is_patch else 0
            world=obj.matrix_world@vertex.co
            if obj.name in BLINK_TARGETS:
                new=BLINK_TARGETS[obj.name][idx]
            elif is_patch:
                new=world.copy()
                # Hidden lenses retain non-degenerate geometry behind the opaque
                # skin. A full closure contains no white/blue/glint thin remnants.
                new.z=1.642+(world.z-1.642)*.12
                original_offset=face_surface(world.x,world.z)-world.y
                # Preserve a small front/back thickness behind skin, rather than
                # collapsing all layers onto one coincident two-sided surface.
                hidden_offset=-.0012+.02*(original_offset-.009)
                new.y=face_surface(new.x,new.z)-hidden_offset
            else:new=world
            target.data[idx].vector=new
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
                # Full-thickness lash target and intentionally skin-hidden lenses
                # are distinct; the eyelash is never flattened with the eye disk.
                new=face.data.attributes["whale_blink_target"].data[idx].vector.copy()
            else:
                new.x*=1.33
                new.z=1.417+(world.z-1.417)*1.65
            if not name.startswith("Blink"):
                new.y=face_surface(new.x,new.z)-offset
            key.data[idx].co=inverse@new
    face.data.shape_keys.key_blocks["Smile"].value=.2
    for label,side in (("L",1),("R",-1)):
        correction=face.shape_key_add(name=f"LidDepth_{label}",from_mix=False)
        correction.relative_key=basis;correction.value=0
        closing=face.data.shape_keys.key_blocks[f"Blink_{label}"]
        for idx,world in enumerate(baseline):
            if face.data.attributes["whale_eye_patch"].data[idx].value!=side:continue
            target=face.matrix_world@closing.data[idx].co
            midpoint=(world+target)*.5
            initial_offset=face_surface(world.x,world.z)-world.y
            target_offset=face_surface(target.x,target.z)-target.y
            desired_y=face_surface(midpoint.x,midpoint.z)-(.5*(initial_offset+target_offset)+.0015)
            corrected=world.copy();corrected.y+=desired_y-midpoint.y
            correction.data[idx].co=inverse@corrected
    return face,semantics


def expression_audit(face,semantics):
    keys=face.data.shape_keys.key_blocks;basis=keys["Basis"]
    attr=face.data.attributes.get("whale_semantic")
    inverse={idx:name for name,idx in semantics.items()}
    result={}
    for name,expected in (("Blink_L","eye_L"),("Blink_R","eye_R"),("LidDepth_L","eye_L"),("LidDepth_R","eye_R"),("Smile","mouth")):
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
        if key.name.startswith("LidDepth"):continue
        offsets=[];hidden=[]
        patch=face.data.attributes.get("whale_eye_patch")
        hidden_side=1 if key.name=="Blink_L" else -1 if key.name=="Blink_R" else 0
        for idx,vertex in enumerate(key.data):
            world=face.matrix_world@vertex.co
            offset=face_surface(world.x,world.z)-world.y
            if patch and hidden_side and patch.data[idx].value==hidden_side:hidden.append(offset)
            else:offsets.append(offset)
        entry={"vertices":len(offsets),"min_front_offset_m":min(offsets),
               "max_front_offset_m":max(offsets)}
        entry["passed"]=entry["min_front_offset_m"]>.0002 and entry["max_front_offset_m"]<.020
        entry["intentionally_skin_hidden_lens_vertices"]=len(hidden)
        if hidden:
            entry["hidden_max_front_offset_m"]=max(hidden)
            entry["hidden_min_front_offset_m"]=min(hidden)
            entry["passed"]=entry["passed"] and max(hidden)<-.0008
        if not entry["passed"]:raise RuntimeError(f"Face depth failure {key.name}: {entry}")
        result[key.name]=entry
    return result


def blink_visibility_audit(face):
    """Full blink must leave thick lashes outside skin and all lenses inside it."""
    keys=face.data.shape_keys.key_blocks
    attrs=face.data.attributes
    patch=attrs.get("whale_eye_patch")
    semantic=attrs.get("whale_semantic")
    by_material={}
    for polygon in face.data.polygons:
        by_material.setdefault(face.data.materials[polygon.material_index].name,set()).update(polygon.vertices)
    lash_indices=by_material["WhaleEyeLash"]
    records={}
    for name,side in (("Blink_L",1),("Blink_R",-1)):
        visible=[];hidden=[]
        for idx in range(len(keys[name].data)):
            basis=face.matrix_world@keys["Basis"].data[idx].co
            target=face.matrix_world@keys[name].data[idx].co
            if basis.x*side<=0:continue
            if patch.data[idx].value==side:hidden.append(target)
            elif idx in lash_indices:visible.append(target)
        if not visible or not hidden:raise RuntimeError("Blink visibility audit lacks components")
        hidden_offsets=[face_surface(v.x,v.z)-v.y for v in hidden]
        visible_offsets=[face_surface(v.x,v.z)-v.y for v in visible]
        # The eyelid at its central crest keeps a full 20mm section, which reads
        # clearly at desktop scale instead of the prior sub-pixel flattened line.
        cx=.223*side
        central=[v for v in visible if abs(v.x-cx)<.025]
        thickness=max(v.z for v in central)-min(v.z for v in central)
        entry={"visible_lash_vertices":len(visible),"hidden_lens_vertices":len(hidden),
               "visible_lash_min_front_offset_m":min(visible_offsets),
               "hidden_lens_max_front_offset_m":max(hidden_offsets),
               "central_closed_lash_height_m":thickness}
        entry["passed"]=min(visible_offsets)>.006 and max(hidden_offsets)<-.0008 and thickness>.015
        if not entry["passed"]:raise RuntimeError(f"{name}: closed eye visibility failure {entry}")
        records[name]=entry
    return records


def blink_transition_audit(face):
    keys=face.data.shape_keys.key_blocks;basis=keys["Basis"]
    by_material={}
    for poly in face.data.polygons:
        by_material.setdefault(face.data.materials[poly.material_index].name,set()).update(poly.vertices)
    patches=face.data.attributes["whale_eye_patch"]
    results={}
    for label,side in (("L",1),("R",-1)):
        close=keys[f"Blink_{label}"];correct=keys[f"LidDepth_{label}"]
        samples=[]
        for amount in (0,.25,.5,.75,1):
            pulse=4*amount*(1-amount)
            points=[face.matrix_world@(base.co+(end.co-base.co)*amount+(extra.co-base.co)*pulse)
                    for base,end,extra in zip(basis.data,close.data,correct.data)]
            facts={}
            for material,indices in by_material.items():
                selected=[i for i in indices if patches.data[i].value==side]
                if not selected:continue
                offsets=[face_surface(points[i].x,points[i].z)-points[i].y for i in selected]
                entry={"vertices":len(selected),"min_front_offset_m":min(offsets),"max_front_offset_m":max(offsets)}
                entry["passed"]=min(offsets)>.00025 if amount<1 else max(offsets)<-.0008
                if not entry["passed"]:raise RuntimeError(f"Blink {label}/{amount}/{material}: discontinuous lens visibility {entry}")
                facts[material]=entry
            minimum_area_ratio=1.
            for poly in face.data.polygons:
                if not all(patches.data[i].value==side for i in poly.vertices):continue
                a,b,c=poly.vertices
                basepoints=[face.matrix_world@basis.data[i].co for i in (a,b,c)]
                original=(basepoints[1]-basepoints[0]).cross(basepoints[2]-basepoints[0]).length
                if original<1e-10:continue
                area=(points[b]-points[a]).cross(points[c]-points[a]).length
                if not math.isfinite(area) or area<1e-11:raise RuntimeError(f"Blink {label}/{amount}: degenerate interpolated patch")
                minimum_area_ratio=min(minimum_area_ratio,area/original)
            samples.append({"blink":amount,"lid_depth_weight":pulse,"materials":facts,"minimum_nonzero_triangle_area_ratio":minimum_area_ratio,"passed":True})
        results[label]=samples
    return results


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
    records=[];color_meshes=[]
    for mesh in document["meshes"]:
        for index,primitive in enumerate(mesh["primitives"]):
            attributes=primitive["attributes"]
            required={"POSITION":3,"NORMAL":3,"TANGENT":4,"TEXCOORD_0":2}
            missing=sorted(set(required)-set(attributes))
            if missing:raise RuntimeError(f"{mesh.get('name')}/{index}: missing exported attributes {missing}")
            count=document["accessors"][attributes["POSITION"]]["count"]
            facts={};attribute_values={}
            for name,components in required.items():
                accessor=document["accessors"][attributes[name]]
                if accessor["count"]!=count or accessor["componentType"]!=5126 or accessor["type"]!=f"VEC{components}":
                    raise RuntimeError(f"{mesh.get('name')}/{index}: incompatible {name} accessor")
                view=document["bufferViews"][accessor["bufferView"]]
                start=view.get("byteOffset",0)+accessor.get("byteOffset",0)
                stride=view.get("byteStride",components*4)
                values=[struct.unpack_from("<"+"f"*components,binary,start+i*stride) for i in range(count)]
                attribute_values[name]=values
                if not all(math.isfinite(x) for value in values for x in value):
                    raise RuntimeError(f"{mesh.get('name')}/{index}: non-finite exported {name}")
                extent=[max(v[i] for v in values)-min(v[i] for v in values) for i in range(components)]
                if name=="TEXCOORD_0" and min(extent)<.0001:
                    raise RuntimeError(f"{mesh.get('name')}/{index}: empty UV chart {extent}")
                facts[name]={"vertices":count,"finite":True,"extent":extent}
            records.append({"mesh":mesh.get("name"),"primitive":index,"attributes":facts})
            if mesh.get("name","").startswith(("WhaleBackCascade","WhaleOuterCascade","WhaleSideCurl")):
                if "COLOR_0" not in attributes:raise RuntimeError(f"{mesh.get('name')}: continuous gradient COLOR_0 missing")
                accessor=document["accessors"][attributes["COLOR_0"]]
                if accessor["count"]!=count or accessor["type"]!="VEC4" or accessor["componentType"] not in (5121,5123,5126):
                    raise RuntimeError(f"{mesh.get('name')}: invalid vertex color accessor")
                if accessor["componentType"]!=5126 and accessor.get("normalized") is not True:
                    raise RuntimeError(f"{mesh.get('name')}: integer COLOR_0 must be normalized")
                view=document["bufferViews"][accessor["bufferView"]]
                start=view.get("byteOffset",0)+accessor.get("byteOffset",0)
                fmt,size,divisor={5121:("B",1,255.),5123:("H",2,65535.),5126:("f",4,1.)}[accessor["componentType"]]
                stride=view.get("byteStride",4*size)
                colors=[tuple(x/divisor for x in struct.unpack_from("<"+fmt*4,binary,start+i*stride)) for i in range(count)]
                low=[min(v[i] for v in colors) for i in range(4)]
                high=[max(v[i] for v in colors) for i in range(4)]
                spans=[high[i]-low[i] for i in range(4)]
                # Presence alone allowed a white exporter placeholder to pass.
                # End-to-end values must match the intentional height palette.
                error=max(abs(c[i]-hair_gradient(p[1])[i]) for c,p in zip(colors,attribute_values["POSITION"]) for i in range(4))
                if error>.006 or spans[1]<.12 or high[0]>.5 or low[3]<.999:
                    raise RuntimeError(f"{mesh.get('name')}: wrong/constant COLOR_0 min={low} max={high} error={error}")
                positions=attribute_values["POSITION"]
                top=max(range(count),key=lambda i:positions[i][1]);bottom=min(range(count),key=lambda i:positions[i][1])
                facts["COLOR_0"]={"vertices":count,"normalized_rgba_min":low,"normalized_rgba_max":high,
                    "channel_span":spans,"max_height_palette_error":error,
                    "top_endpoint":{"height_m":positions[top][1],"rgba":colors[top]},
                    "bottom_endpoint":{"height_m":positions[bottom][1],"rgba":colors[bottom]},"passed":True}
                color_meshes.append(mesh.get("name"))
    if len(set(color_meshes))!=9:raise RuntimeError("Expected 9 continuous-gradient long-hair meshes")
    return {"mesh_count":len(document["meshes"]),"primitive_count":len(records),"all_primitives_passed":True,
            "required_attributes":["POSITION","NORMAL","TANGENT","TEXCOORD_0"],"primitives":records,
            "continuous_gradient_COLOR_0_meshes":color_meshes}


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
            material_name=document["materials"][primitive["material"]].get("name","")
            targets={name:accessor(target["POSITION"]) for name,target in zip(names,primitive["targets"])}
            for name,target in zip(names,primitive["targets"]):
                delta=accessor(target["POSITION"])
                record=report.setdefault(name,{"changed_vertices":0,"opposite_eye_changed_vertices":0,"mouth_changed_vertices":0,
                    "closed_lash_visible_vertices":0,"closed_lens_skin_hidden_vertices":0,"closed_wrong_depth_vertices":0})
                for point,change in zip(baseline,delta):
                    if max(abs(x) for x in change)<=1e-8:continue
                    record["changed_vertices"]+=1
                    # Detail meshes were authored with identity transforms and
                    # global vertices. GLTF Y is original Blender Z.
                    world_x=point[0]
                    world_z=point[1]
                    if name in ("Blink_L","LidDepth_L") and world_x<0:record["opposite_eye_changed_vertices"]+=1
                    if name in ("Blink_R","LidDepth_R") and world_x>0:record["opposite_eye_changed_vertices"]+=1
                    if name!="Smile" and world_z<1.45:record["mouth_changed_vertices"]+=1
                    if name=="Smile" and world_z>1.45:raise RuntimeError("Exported smile edited eye/nose geometry")
                    if name.startswith("Blink"):
                        target_x=point[0]+change[0]
                        target_z=point[1]+change[1]
                        target_y=-(point[2]+change[2])
                        front=face_surface(target_x,target_z)-target_y
                        if material_name=="WhaleEyeLash" and front>.006:
                            record["closed_lash_visible_vertices"]+=1
                        elif front<-.0008:
                            record["closed_lens_skin_hidden_vertices"]+=1
                        else:record["closed_wrong_depth_vertices"]+=1
            ia=document["accessors"][primitive["indices"]]
            iv=document["bufferViews"][ia["bufferView"]]
            istart=iv.get("byteOffset",0)+ia.get("byteOffset",0)
            ifmt,isize={5121:("B",1),5123:("H",2),5125:("I",4)}[ia["componentType"]]
            indices=[struct.unpack_from("<"+ifmt,binary,istart+i*isize)[0] for i in range(ia["count"])]
            for label,side in (("L",1),("R",-1)):
                if f"LidDepth_{label}" not in targets:continue
                close=targets[f"Blink_{label}"];correct=targets[f"LidDepth_{label}"]
                selected={i for i,c in enumerate(correct) if max(abs(x)for x in c)>1e-8 and baseline[i][0]*side>0}
                if not selected:continue
                samples=report[f"Blink_{label}"].setdefault("transition_samples",{})
                for amount in (0,.25,.5,.75,1):
                    pulse=4*amount*(1-amount)
                    points=[Vector(tuple(p[k]+d[k]*amount+c[k]*pulse for k in range(3))) for p,d,c in zip(baseline,close,correct)]
                    offsets=[face_surface(points[i].x,points[i].y)+points[i].z for i in selected]
                    entry={"material":material_name,"vertices":len(selected),"min_front_offset_m":min(offsets),"max_front_offset_m":max(offsets),
                           "lid_depth_weight":pulse}
                    entry["passed"]=min(offsets)>.00025 if amount<1 else max(offsets)<-.0008
                    if not entry["passed"]:raise RuntimeError(f"Exported blink {label}/{amount}/{material_name}: depth discontinuity {entry}")
                    minimum=1.
                    for start in range(0,len(indices),3):
                        a,b,c=indices[start:start+3]
                        if not {a,b,c}<=selected:continue
                        original=(Vector(baseline[b])-Vector(baseline[a])).cross(Vector(baseline[c])-Vector(baseline[a])).length
                        if original<1e-10:continue
                        area=(points[b]-points[a]).cross(points[c]-points[a]).length
                        if not math.isfinite(area) or area<1e-11:raise RuntimeError(f"Exported blink {label}/{amount}: degenerate patch triangle")
                        minimum=min(minimum,area/original)
                    entry["minimum_nonzero_triangle_area_ratio"]=minimum
                    samples.setdefault(str(amount),[]).append(entry)
    for name,record in report.items():
        if not record["changed_vertices"] or record["opposite_eye_changed_vertices"] or record["mouth_changed_vertices"]:
            raise RuntimeError(f"Exported expression isolation failed {name}: {record}")
        if name.startswith("Blink") and (record["closed_lash_visible_vertices"]<100 or
                record["closed_lens_skin_hidden_vertices"]<1000 or record["closed_wrong_depth_vertices"]):
            raise RuntimeError(f"Exported closed-eye visibility failed {name}: {record}")
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
        depth=facial_depth_audit(face) if kind=="author" else None
        blink=blink_visibility_audit(face) if kind=="author" else None
        transition=blink_transition_audit(face) if kind=="author" else None
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
                      "expression_isolation":expr,"facial_depth":depth,"blink_visibility":blink,"blink_transition":transition,"clips":clips}
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
    proto.MESHES.clear();proto.MATERIALS.clear();proto.REST.clear();SWEEP_AUDITS.clear();BLINK_TARGETS.clear()
    scene=bpy.context.scene;scene.render.fps=30;scene.frame_start=1;scene.frame_end=120
    arm=proto.build_rig();proto.build_geometry();remove_proxy_head()
    palette={"WhaleSkin":"FFE8DE","WhaleHairRoyal":"4563A5","WhaleHairMid":"5E9AD5", "WhaleHairCyan":"70C7EF",
             "WhaleLace":"FFF9FF","WhaleRibbon":"49AFE9","WhaleEyeLash":"1D2949","WhaleEyeBrow":"5D526F",
             "WhaleEyeWhite":"FFF8FB","WhaleIrisDeep":"3C4F92","WhaleIrisBlue":"4E7EC7","WhaleIrisGlow":"7BCDF5",
             "WhalePupil":"223B71","WhaleHighlight":"FFFFFF","WhaleMouth":"8C4556","WhaleEyeBlush":"F3B6B7",
             "WhaleHairGradient":"FFFFFF"}
    for name,color in palette.items():proto.material(name,linear_hex(color))
    # The editable Blender source must show the same vertex colors as GLTF.
    gradient_material=proto.MATERIALS["WhaleHairGradient"]
    color_node=gradient_material.node_tree.nodes.new("ShaderNodeVertexColor")
    color_node.layer_name="WhaleHairGradient"
    gradient_material.node_tree.links.new(color_node.outputs["Color"],gradient_material.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
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
    blink_visibility=blink_visibility_audit(face)
    blink_transition=blink_transition_audit(face)
    bpy.data.orphans_purge(do_local_ids=True,do_linked_ids=True,do_recursive=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    options={"export_format":"GLB","export_yup":True,"export_animations":True,"export_animation_mode":"ACTIONS",
             "export_def_bones":True,"export_morph":True,"export_morph_normal":True,"export_morph_animation":False,
             "export_skins":True,"export_all_influences":False,"export_optimize_animation_size":False,
             "export_apply":False,"export_cameras":False,"export_lights":False,
             "export_texcoords":True,"export_normals":True,"export_tangents":True,
             "export_vertex_color":"ACTIVE","export_all_vertex_colors":False}
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
        "expressions":{"Blink_L":{"range":[0,1],"default":0},"Blink_R":{"range":[0,1],"default":0},"Smile":{"range":[0,1],"default":.2},
                       "LidDepth_L":{"range":[0,1],"default":0,"ownership":"left eye patch depth only"},
                       "LidDepth_R":{"range":[0,1],"default":0,"ownership":"right eye patch depth only"}},
        "expression_drivers":{"LidDepth_L":"4*Blink_L*(1-Blink_L)","LidDepth_R":"4*Blink_R*(1-Blink_R)"},
        "materials":list(proto.MATERIALS),"material_alpha":"all opaque geometry; no bitmap cards",
        "animations":animations,"rise":"play sit backwards; do not scale limbs",
        "sit_contact":{"stool_top_y":.35,"stool_center":[0,.175,-.18],"stool_size":[.56,.35,.36],
                       "foot_anchor_L":[.145,.12,0],"foot_anchor_R":[-.145,.12,0],"note":"Temporary proxy contact; not a final whale-girl body/clothing rig."},
        "audit":{"max_weight_sum_error":max(weights),"unweighted_vertices":unweighted,"all_deform_bones":True,
                 "structure":proto.glb_structure(glb),"expression_isolation":expressions,
                 "facial_depth":facial_depth,"neutral_bounds_blender":bounds,
                 "blink_visibility":blink_visibility,
                 "blink_transition":blink_transition,
                 "uv_and_tangents":uv_records,"exported_surface_attributes":glb_surface_attribute_audit(glb),
                 "sweep_geometry":SWEEP_AUDITS},
        "source_sha256":proto.digest(blend),"glb_sha256":proto.digest(glb),"visual_status":"pending_actual_godot_review",
        "limits":["Candidate head likeness pending review","temporary prototype torso/limbs and sit animation",
                  "Hair uses continuous geometric vertex-color gradient, without painted strand highlights or texture detail",
                  "No hair collisions, fingers or cloth simulation"]}
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
