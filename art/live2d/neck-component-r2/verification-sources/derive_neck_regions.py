"""Project texture-supported Native neck geometry; never read comparison pixels.

Only source texture alpha and the two capture parameter traces are inspected.
The output is an input to compare_neck_reviews.py, not a raster-derived crop.
"""
from __future__ import annotations

import argparse
import ctypes as c
import itertools
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

import audit_neck_surface as audit

ROOT = audit.ROOT
OUT = Path(__file__).resolve().parent


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def hull(points):
    points = sorted(set(map(tuple, points)))
    if len(points) < 3:
        return np.asarray(points, dtype=float)
    def cross(o, a, b):
        return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lower, upper = [], []
    for p in points:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 1e-10:
            lower.pop()
        lower.append(p)
    for p in reversed(points):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 1e-10:
            upper.pop()
        upper.append(p)
    return np.asarray(lower[:-1]+upper[:-1], dtype=float)


def clip_triangle(polygon, triangle):
    orientation = np.linalg.det(np.stack([triangle[1]-triangle[0], triangle[2]-triangle[0]]))
    sign = 1 if orientation > 0 else -1
    result = list(polygon)
    for a, b in zip(triangle, np.roll(triangle, -1, axis=0)):
        if not result:
            return []
        edge = b-a
        side = lambda p: sign*(edge[0]*(p[1]-a[1])-edge[1]*(p[0]-a[0]))
        incoming, result = result, []
        old = incoming[-1]
        old_side = side(old)
        for point in incoming:
            current_side = side(point)
            if (current_side >= -1e-9) != (old_side >= -1e-9):
                result.append(old + (point-old)*old_side/(old_side-current_side))
            if current_side >= -1e-9:
                result.append(point)
            old, old_side = point, current_side
    return result


class TextureSupport:
    """Exact affine extrema of positive-alpha texels, clipped to mesh triangles.

    A positive texel centre has bilinear support +/-1 texel, hence a pixel
    at integer x,y contributes [x-.5,x+1.5] x [y-.5,y+1.5]. Taking a hull
    within each UV triangle preserves every possible projected extremum.
    """
    def __init__(self, alpha, uv_pixels, triangles):
        yy, xx = np.where(alpha > 0)
        centres = np.stack([xx+.5, yy+.5], axis=1)
        self.active_texels = len(xx)
        self.samples = []
        self.triangle_ids = []
        self.texture_hull_points = []
        for i, indices in enumerate(triangles):
            tri = uv_pixels[indices]
            matrix = np.stack([tri[0]-tri[2], tri[1]-tri[2]], axis=1)
            if abs(np.linalg.det(matrix)) < 1e-10:
                continue
            selected = centres[((centres >= tri.min(axis=0)-1).all(axis=1))
                               & ((centres <= tri.max(axis=0)+1).all(axis=1))]
            clipped = []
            for centre in selected:
                rectangle = centre + np.asarray([[-1,-1],[1,-1],[1,1],[-1,1]])
                clipped.extend(clip_triangle(rectangle, tri))
            if not clipped:
                continue
            points = hull(clipped)
            ab = np.linalg.solve(matrix, (points-tri[2]).T).T
            weights = np.column_stack([ab, 1-ab.sum(axis=1)])
            self.samples.append((indices, weights))
            self.triangle_ids.append(i)
            self.texture_hull_points.append(points)
        if not self.samples:
            raise ValueError("No positive-alpha texels intersect the supplied UV mesh")

    def points(self, native_positions):
        return np.concatenate([weights @ native_positions[indices]
                               for indices, weights in self.samples])


class GeometryNeckROI:
    def __init__(self, candidate_model_directory):
        self.model_dir = Path(candidate_model_directory)
        self.meta_path = self.model_dir / "whale-girl-layered-draft.psd2live.json"
        metadata = read_json(self.meta_path)
        separation = metadata["runtimeMaterialSeparation"]
        ownership_path = self.model_dir / separation["ownership"]
        ownership = read_json(ownership_path)
        chart = ownership["source_charts"]["face"]
        main_settings = read_json(self.model_dir / "whale-girl-layered-draft.model3.json")
        main_moc_path = self.model_dir / main_settings["FileReferences"]["Moc"]
        if audit.digest(main_moc_path) != audit.digest(audit.MODEL/"whale-girl-layered-draft.moc3"):
            raise ValueError("The candidate main MOC differs from the Native source sampler")
        if metadata["layers"] != audit.META["layers"] or ownership["source_charts"] != audit.OWN["source_charts"]:
            raise ValueError("The candidate source charts/layer mapping differ from the Native source sampler")
        atlas_path = self.model_dir / main_settings["FileReferences"]["Textures"][0]
        mask_path = self.model_dir / ownership["neck_mask"]
        x0, y0, x1, y1 = chart["atlas"]
        alpha = np.asarray(Image.open(atlas_path).convert("RGBA"))[y0:y1,x0:x1,3]
        mask = np.asarray(Image.open(mask_path).convert("L"))[y0:y1,x0:x1]
        alpha = ((alpha.astype(np.uint16)*mask.astype(np.uint16)+127)//255).astype(np.uint8)
        pixels = np.zeros((1254,1254,4), dtype=np.uint8)
        dx, dy = chart["offset"]
        pixels[y0+dy:y1+dy,x0+dx:x1+dx,3] = alpha
        self.audit = audit.Audit(pixels)
        self.main = self.audit.native
        self.main.update({"name":"initial-native-default", "parameters":{}, "part_opacities":{}})
        self.initial_floor = np.float32(self.audit.floor("footwear-l", "footwear-r"))
        canvas, origin, ppu = audit.core.Vec2(), audit.core.Vec2(), c.c_float()
        self.main.api["ReadCanvasInfo"](self.main.model,c.byref(canvas),c.byref(origin),c.byref(ppu))
        self.scale = np.float32(1.90/(canvas.Y/ppu.value))
        self.canvas_info = {"canvas_pixels":[canvas.X,canvas.Y],"origin_pixels":[origin.X,origin.Y],
                            "pixels_per_unit":ppu.value,"model_matrix_scale":float(self.scale),
                            "model_matrix_translation":[0,0],"initial_standing_floor":float(self.initial_floor)}
        # Audit's source coordinates subtract these cropped texture origins.
        old_uv = self.audit.uv["face"]-np.asarray([x0+dx,y0+dy])
        self.old_support = TextureSupport(alpha, old_uv, self.audit.tri)
        surface_ref = metadata["runtimeNeckConnection"]["independentSurface"]
        model3 = self.model_dir / surface_ref["model"]
        rig_path = self.model_dir / surface_ref["rig"]
        self.rig = read_json(rig_path)
        surface_settings = read_json(model3)
        moc_path = model3.parent / surface_settings["FileReferences"]["Moc"]
        texture_path = model3.parent / surface_settings["FileReferences"]["Textures"][0]
        self.aux = audit.core.NativeModel(audit.core.find_core(), moc_path)
        self.aux_index = self.aux.drawable_ids.index(self.rig["drawable"])
        self.aux.update({"name":"aux-neutral", "parameters":{}, "part_opacities":{}})
        self.aux_neutral = self.aux_positions()
        source_points = np.asarray(self.rig["vertexSourcePoints"],dtype=float)
        self.source_points = source_points
        self.face_samples = [self.audit.anchor("face", point) for point in source_points]
        with Image.open(texture_path) as texture:
            texture_width, texture_height = texture.size
            auxiliary_alpha = np.asarray(texture.convert("RGBA"))[:,:,3]
        pointer = self.aux.get("DrawableVertexUvs")[self.aux_index]
        uv = np.asarray([[pointer[v].X*texture_width,(1-pointer[v].Y)*texture_height]
                         for v in range(self.aux.vertex_counts[self.aux_index])])
        pointer = self.aux.get("DrawableIndices")[self.aux_index]
        triangles = np.asarray([pointer[i] for i in range(self.aux.index_counts[self.aux_index])]).reshape(-1,3)
        self.new_support = TextureSupport(auxiliary_alpha, uv, triangles)
        old_source_points = self.old_support.points(self.audit.uv["face"])
        new_source_points = self.new_support.points(source_points)
        self.support_info = {
            "old_positive_alpha_texels":self.old_support.active_texels,
            "old_support_triangles":self.old_support.triangle_ids,
            "old_positive_alpha_support_source_bounds":self.bounds(old_source_points),
            "new_positive_alpha_texels":self.new_support.active_texels,
            "new_support_triangles":self.new_support.triangle_ids,
            "new_positive_alpha_support_source_bounds":self.bounds(new_source_points),
            "auxiliary_vertex_count":len(source_points),"auxiliary_triangle_count":len(triangles),
            "bilinear_support_texels":1,"aa_margin_png_pixels":2}
        self.input_paths = [Path(__file__),Path(audit.__file__),ROOT/"tools/probe_cubism_core.py",
                            ROOT/"src/CubismCanvas.cpp",ROOT/"src/CubismNeckBinding.cpp",
                            ROOT/"src/CubismPostureTransition.h",audit.core.find_core(),
                            audit.MODEL/"whale-girl-layered-draft.moc3",
                            audit.MODEL/"whale-girl-layered-draft.psd2live.json",
                            audit.MODEL/"material-ownership/ownership.json",
                            self.meta_path,main_moc_path,ownership_path,atlas_path,mask_path,model3,rig_path,moc_path,texture_path]
        self.physics_path = self.model_dir/main_settings["FileReferences"]["Physics"]
        self.input_paths.append(self.physics_path)
        destinations = {output["Destination"]["Id"]
                        for setting in read_json(self.physics_path)["PhysicsSettings"]
                        for output in setting["Output"]}
        if destinations != {"ParamHairBack","ParamHairFront"}:
            raise ValueError(f"Unexpected physics output parameters: {destinations}")
        self.physics_outputs = sorted(destinations)

    def aux_positions(self):
        pointer = self.aux.get("DrawableVertexPositions")[self.aux_index]
        return np.asarray([[pointer[i].X,pointer[i].Y]
                           for i in range(self.aux.vertex_counts[self.aux_index])])

    @staticmethod
    def bounds(points):
        return np.r_[points.min(axis=0),points.max(axis=0)].tolist()

    @staticmethod
    def native_request(parameters):
        raw_busy, raw_sit = parameters.get("ParamBusyLaptop",0), parameters.get("ParamSitPose",0)
        smooth = lambda x: (lambda t:t*t*(3-2*t))(min(1,max(0,x)))
        mix = smooth((raw_busy-.74)/.19)
        material = int(mix >= .5)
        requested = dict(parameters)
        requested["ParamBusyLaptop"] = material
        requested["ParamSitPose"] = .93*smooth(raw_sit/.9)*(1-mix)+mix
        gape, smile = parameters.get("ParamMouthGape",0)>=.5, parameters.get("ParamSmileOpen",0)>=.5
        if "ParamMouthGape" in requested:
            requested["ParamMouthGape"] = int(gape)
        if "ParamMouthOpenY" in requested:
            if gape:
                requested["ParamMouthOpenY"] = 1
            elif not smile:
                requested["ParamMouthOpenY"] = 0
        return requested,mix,material

    def update_main(self, parameters, name):
        requested,mix,material = self.native_request(parameters)
        self.main.update({"name":name,"parameters":requested,"part_opacities":{"PartBody":1-material}})
        return requested,mix,material

    def relevant_geometry(self):
        return np.concatenate([self.audit.positions(source) for source in
                               ("face","topwear","busy torso","footwear-l","footwear-r","busy leg l","busy leg r")])

    def physics_invariance(self, parameters, name):
        requested,_,_ = self.update_main(parameters,name)
        reference = self.relevant_geometry()
        domain = [[self.main.ranges[key]["min"],self.main.ranges[key]["default"],self.main.ranges[key]["max"]]
                  for key in self.physics_outputs]
        maximum = 0.0
        for values in itertools.product(*domain):
            self.update_main(parameters | dict(zip(self.physics_outputs,values)),name+"-hair-domain")
            maximum = max(maximum,float(np.abs(self.relevant_geometry()-reference).max()))
        if maximum != 0:
            raise ValueError(f"Hair physics affects neck/floor/collar geometry for {name}: {maximum}")
        return maximum

    def project(self, points, standing_floor, width, height):
        grounded = np.float32(np.float32(self.scale*self.initial_floor)-np.float32(self.scale*standing_floor))
        transformed = points*float(self.scale)+np.asarray([0,float(grounded)])
        return (transformed*np.asarray([1,-1])+1)*np.asarray([width,height])/2, float(grounded)

    def region(self, parameters, name="pose", width=840, height=840):
        _,mix,material = self.update_main(parameters,name)
        face = self.audit.positions("face")
        standing_floor = np.float32(self.audit.floor("footwear-l","footwear-r"))
        seated_floor = np.float32(self.audit.floor("busy leg l","busy leg r"))
        standing_positions = self.audit.positions("topwear").astype(np.float32)
        seated_positions = self.audit.positions("busy torso").astype(np.float32)
        seated_positions[:,1] += np.float32(standing_floor-seated_floor)
        standing = self.audit.sample("topwear",(628,588),standing_positions)
        seated = self.audit.sample("busy torso",(628,588),seated_positions)
        target = standing+mix*(seated-standing)
        aligned = []
        for collar,positions in ((standing,standing_positions),(seated,seated_positions)):
            if not target[1] > standing_floor or not collar[1] > standing_floor:
                raise ValueError(f"Invalid floor-pinned body transform for {name}")
            weights = (positions[:,1].astype(float)-float(standing_floor))/(collar[1]-float(standing_floor))
            aligned.append((positions.astype(float)+weights[:,None]*(target-collar)).astype(np.float32))
        visible_collar = self.audit.sample("busy torso" if material else "topwear",(628,588),aligned[material])
        head_tip = self.audit.sample("face",(628,588),face)
        delta = visible_collar-head_tip
        old_vertices = (face+self.audit.weights[:,None]*delta).astype(np.float32)
        ppu = self.rig["pixelsPerUnit"]
        aux_values = {self.rig["parameters"]["bottomX"]:float(np.float32(delta[0]*ppu)),
                      self.rig["parameters"]["bottomY"]:float(np.float32(delta[1]*ppu)),
                      self.rig["parameters"]["curve"]:float(np.float32(np.clip(delta[0]*ppu/24,-1,1)))}
        for key,value in aux_values.items():
            if not self.aux.ranges[key]["min"] <= value <= self.aux.ranges[key]["max"]:
                raise ValueError(f"Auxiliary parameter out of range: {name}.{key}={value}")
        self.aux.update({"name":name+"-aux","parameters":aux_values,"part_opacities":{}})
        attached = np.asarray([weights @ face[triangle] for triangle,weights in self.face_samples])
        new_vertices = (attached+self.aux_positions()-self.aux_neutral).astype(np.float32)
        old_pixels,grounded = self.project(self.old_support.points(old_vertices),standing_floor,width,height)
        new_pixels,_ = self.project(self.new_support.points(new_vertices),standing_floor,width,height)
        points = np.concatenate([old_pixels,new_pixels])
        minimum,maximum = points.min(axis=0),points.max(axis=0)
        bbox = [max(0,math.floor(minimum[0])-2),max(0,math.floor(minimum[1])-2),
                min(width,math.ceil(maximum[0])+2),min(height,math.ceil(maximum[1])+2)]
        return bbox,{"old_neck_png_bounds":self.bounds(old_pixels),"new_neck_png_bounds":self.bounds(new_pixels),
                     "seated_mix":mix,"material":material,"grounded_translation_y":grounded,
                     "head_tip_native":head_tip.tolist(),"visible_collar_native":visible_collar.tolist(),
                     "aux_parameters":aux_values,"viewport_pixels":[width,height]}


def trace_rows(session):
    rows = []
    document = read_json(session/"poses.json")
    for index,pose in enumerate(document["keyframes"]):
        parameters = {"ParamEyeLOpen":1,"ParamEyeROpen":1} | pose["parameters"]
        rows.append((f"poses/neck-{index:02d}.png",parameters))
    for scene,filename,field,count in (("turn-ended-laptop","scene.json","parameters",75),
                                       ("busy-laptop","capture.json","requested_parameters",195)):
        frames = read_json(session/scene/filename)["frames"]
        if len(frames) != count:
            raise ValueError(f"Unexpected frame count for {scene}: {len(frames)}")
        rows += [(f"{scene}/frame-{index:03d}.png",frame[field]) for index,frame in enumerate(frames)]
    if len(rows) != 353 or len(dict(rows)) != 353:
        raise ValueError("Expected exactly 83 static + 270 continuous frames")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline",type=Path,required=True)
    parser.add_argument("--candidate",type=Path,required=True)
    parser.add_argument("--output",type=Path,default=OUT/"native-neck-regions.json")
    parser.add_argument("--width",type=int,default=840)
    parser.add_argument("--height",type=int,default=840)
    args = parser.parse_args()
    baseline,candidate = args.baseline.resolve(),args.candidate.resolve()
    baseline_session,candidate_session = read_json(baseline/"session.json"),read_json(candidate/"session.json")
    old_rows,new_rows = trace_rows(baseline),trace_rows(candidate)
    if old_rows != new_rows:
        raise ValueError("Baseline/candidate parameter traces differ")
    model_dir = Path(candidate_session["model_manifest_path"]).parent
    engine = GeometryNeckROI(model_dir)
    paths = engine.input_paths + [folder/name for folder in (baseline,candidate)
                                  for name in ("session.json","poses.json","turn-ended-laptop/scene.json","busy-laptop/capture.json")]
    hashes = {path.resolve():audit.digest(path) for path in paths}
    # Verify all live texture/Core inputs against capture-pinned records.
    for session in (baseline_session,candidate_session):
        for record in session["inputs"]:
            if Path(record["path"]).resolve() in hashes and audit.digest(record["path"]) != record["sha256"]:
                raise ValueError(f"Capture-pinned input changed: {record['path']}")
    regions,details = {},{}
    for index,(name,parameters) in enumerate(new_rows):
        engine.physics_invariance(parameters,name)
        bbox,details[name] = engine.region(parameters,name,args.width,args.height)
        regions[name] = [bbox]
        if (index+1)%50 == 0:
            print(f"Derived {index+1}/353 Native neck regions",flush=True)
    for path,pinned in hashes.items():
        if audit.digest(path) != pinned:
            raise ValueError(f"Protected derivation input changed: {path}")
    areas = [(r[0][2]-r[0][0])*(r[0][3]-r[0][1]) for r in regions.values()]
    report = {"schema_version":1,"derivation":"native_geometry_before_raster_comparison",
              "baseline_session_sha256":audit.digest(baseline/"session.json"),
              "candidate_session_sha256":audit.digest(candidate/"session.json"),
              "inputs":[{"path":str(path),"sha256":sha} for path,sha in sorted(hashes.items())],
              "method":{"source_alpha_support_clipped_to_actual_uv_triangles":True,
                        "runtime_core_posture_floor_and_aux_keyforms_replayed":True,
                        "comparison_png_pixels_read":False,"regions_adjusted_from_raster_differences":False,
                        "occlusion_subtracted":False,"bounds_convention":"left/top inclusive, right/bottom exclusive",
                        "physics_outputs":engine.physics_outputs,"physics_invariance_poses":353,
                        "physics_invariance_samples_per_pose":9,"physics_max_geometry_difference_native":0.0},
              "projection":engine.canvas_info,"texture_support":engine.support_info,
              "summary":{"region_count":len(regions),"static_pose_count":83,"continuous_frame_count":270,
                         "minimum_bbox_area_pixels":min(areas),"maximum_bbox_area_pixels":max(areas),
                         "maximum_frame_fraction":max(areas)/(args.width*args.height)},
              "regions":regions,"geometry_details":details}
    args.output.resolve().write_text(json.dumps(report,ensure_ascii=False,indent=2,allow_nan=False)+"\n",encoding="utf8")
    print(json.dumps(report["summary"],ensure_ascii=False),flush=True)
    print(str(args.output.resolve()),flush=True)


if __name__ == "__main__":
    main()
