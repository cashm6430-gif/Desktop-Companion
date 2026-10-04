"""Read-only Native audit and exact neck layer extraction; no production edits."""
from pathlib import Path
import ctypes as c
import hashlib
import itertools
import json
import math
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import probe_cubism_core as core

OUT = Path(__file__).resolve().parent
MODEL = ROOT / "assets/live2d/whale-girl"
OWN = json.loads((MODEL / "material-ownership/ownership.json").read_text(encoding="utf8"))
META = json.loads((MODEL / "whale-girl-layered-draft.psd2live.json").read_text(encoding="utf8"))
ATLAS = MODEL / "whale-girl-layered-draft.4096/texture_00.png"

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def save(name, data):
    (OUT / name).write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf8")

def extraction():
    if digest(ATLAS) != OWN["atlas_sha256"]:
        raise ValueError("Atlas does not match pinned ownership input")
    mask_path = MODEL / OWN["neck_mask"]
    if digest(mask_path) != OWN["neck_mask_sha256"]:
        raise ValueError("Neck mask does not match pinned ownership input")
    original = np.array(Image.open(ATLAS).convert("RGBA"))
    mask = np.array(Image.open(mask_path).convert("L"))
    neck = original.copy()
    neck[:, :, 3] = ((neck[:, :, 3].astype(np.uint16) * mask.astype(np.uint16) + 127) // 255).astype(np.uint8)
    chart = OWN["source_charts"]["face"]
    x0, y0, x1, y1 = chart["atlas"]
    dx, dy = chart["offset"]
    canvas = Image.new("RGBA", tuple(OWN["source_canvas"]))
    canvas.paste(Image.fromarray(neck[y0:y1, x0:x1]), (x0 + dx, y0 + dy))
    canvas.save(OUT / "neck-original.png")
    box = canvas.getbbox()
    canvas.crop(box).save(OUT / "neck-original-crop.png")
    raw = np.array(canvas)
    row_stats = []
    for y in range(box[1], box[3]):
        active = raw[y, :, 3] > 0
        warm = active & (raw[y, :, 0].astype(int) > raw[y, :, 2].astype(int) + 18) & (raw[y, :, 0] > 200)
        x = np.flatnonzero(active)
        warm_x = np.flatnonzero(warm)
        row_stats.append({"source_y": y, "painted_count": int(active.sum()),
                          "painted_x_bounds": [int(x.min()), int(x.max())] if len(x) else None,
                          "warm_count": int(warm.sum()),
                          "warm_x_bounds": [int(warm_x.min()), int(warm_x.max())] if len(warm_x) else None})
    receipt = {"kind": "isolated_exact_neck_source_extraction", "full_canvas": OWN["source_canvas"],
               "cropped_bbox_source": list(box), "cropped_size": [box[2]-box[0], box[3]-box[1]],
               "atlas": str(ATLAS), "atlas_sha256": digest(ATLAS), "mask": str(mask_path),
               "mask_sha256": digest(mask_path), "rgba_colour_repainted": False,
               "jaw_overlap_rows": [568, 570], "main_erased_from_y": 570,
               "neck_tip_source": [628, 588], "source_row_statistics": row_stats,
               "outputs": {name: digest(OUT/name) for name in ("neck-original.png", "neck-original-crop.png")}}
    save("neck-source.json", receipt)
    return raw, receipt

class Audit:
    def __init__(self, pixels):
        self.native = core.NativeModel(core.find_core(), MODEL / "whale-girl-layered-draft.moc3")
        self.lookup = {layer["source"]: self.native.drawable_ids.index(layer["drawable"]) for layer in META["layers"]}
        self.face = self.lookup["face"]
        self.ppu = 1254
        self.uv = {}
        for source in ("face", "topwear", "busy torso"):
            chart = OWN["source_charts"][source]
            ptr = self.native.get("DrawableVertexUvs")[self.lookup[source]]
            self.uv[source] = np.array([[ptr[v].X*4096+chart["offset"][0],
                                         (1-ptr[v].Y)*4096+chart["offset"][1]]
                                        for v in range(self.native.vertex_counts[self.lookup[source]])])
        ptr = self.native.get("DrawableIndices")[self.face]
        self.tri = np.array([ptr[v] for v in range(self.native.index_counts[self.face])]).reshape(-1, 3)
        # Determine actual painted overlap, rather than a broad rectangular
        # test band that would measure triangles with no visible neck pixels.
        yy, xx = np.where(pixels[:, :, 3] > 0)
        self.pixel_points = np.stack([xx + 0.5, yy + 0.5], axis=1)
        self.visible_triangles = []
        for i, tri in enumerate(self.tri):
            p = self.uv["face"][tri]
            matrix = np.array([[p[0, 0]-p[2, 0], p[1, 0]-p[2, 0]],
                               [p[0, 1]-p[2, 1], p[1, 1]-p[2, 1]]])
            if abs(np.linalg.det(matrix)) < 1e-8: continue
            weights = np.linalg.solve(matrix, (self.pixel_points-p[2]).T).T
            if ((weights[:, 0] >= -1e-7) & (weights[:, 1] >= -1e-7) & (weights.sum(axis=1) <= 1+1e-7)).any():
                self.visible_triangles.append(i)
        self.cached = {}
        for source, coords in (("face", [(628, 570), (628, 588), (600, 570), (655, 570)]),
                                ("topwear", [(628, 588)]), ("busy torso", [(628, 588)])):
            for target in coords: self.cached[(source, target)] = self.anchor(source, target)
        self.weights = (self.uv["face"][:, 1] - 570) / 18

    def anchor(self, source, target):
        mesh = self.lookup[source]
        index_ptr = self.native.get("DrawableIndices")[mesh]
        for i in range(0, self.native.index_counts[mesh], 3):
            indices = np.array([index_ptr[i], index_ptr[i+1], index_ptr[i+2]])
            uv = self.uv[source][indices]
            matrix = np.array([[uv[0,0]-uv[2,0], uv[1,0]-uv[2,0]],
                               [uv[0,1]-uv[2,1], uv[1,1]-uv[2,1]]])
            if abs(np.linalg.det(matrix)) < 1e-8: continue
            ab = np.linalg.solve(matrix, np.array(target)-uv[2])
            weights = np.array([ab[0], ab[1], 1-ab.sum()])
            if weights.min() >= -1e-7: return indices, weights
        raise ValueError(f"No triangle owns {source} {target}")

    def positions(self, source):
        mesh = self.lookup[source]
        ptr = self.native.get("DrawableVertexPositions")[mesh]
        return np.array([[ptr[v].X, ptr[v].Y] for v in range(self.native.vertex_counts[mesh])])

    def sample(self, source, point, positions=None):
        tri, weights = self.cached[(source, point)]
        p = self.positions(source) if positions is None else positions
        return weights @ p[tri]

    def floor(self, left, right):
        return min(self.positions(left)[:, 1].min(), self.positions(right)[:, 1].min())

    def evaluate(self, params, name):
        raw_busy = params.get("ParamBusyLaptop", 0)
        raw_sit = params.get("ParamSitPose", 0)
        smooth = lambda x: (lambda t: t*t*(3-2*t))(max(0, min(1, x)))
        mix = smooth((raw_busy-.74)/.19)
        fold = .93*smooth(raw_sit/.9)*(1-mix)+mix
        material = int(mix >= .5)
        requested = dict(params, ParamBusyLaptop=material, ParamSitPose=fold)
        self.native.update({"name": name, "parameters": requested, "part_opacities": {"PartBody": 1-material}})
        head = self.positions("face")
        top = self.sample("face", (628,570), head)
        tip = self.sample("face", (628,588), head)
        standing = self.sample("topwear", (628,588))
        seated = self.sample("busy torso", (628,588))
        standing_floor = self.floor("footwear-l", "footwear-r")
        seated_floor = self.floor("busy leg l", "busy leg r")
        seated[1] += standing_floor-seated_floor
        target = standing + mix*(seated-standing)
        delta = target-tip
        neck = head + self.weights[:, None]*delta
        ratios = []
        for i in self.visible_triangles:
            tri = self.tri[i]
            a, b = head[tri], neck[tri]
            area = lambda p: float(np.linalg.det(np.stack([p[1]-p[0], p[2]-p[0]], axis=1)))
            ratios.append(area(b)/area(a))
        length = float(np.linalg.norm(target-top)*self.ppu)
        return {"name": name, "raw_parameters": params, "seated_mix": mix, "material": material,
                "neck_length_pixels": length, "neck_length_scale": length/(np.linalg.norm(tip-top)*self.ppu),
                "delta_pixels": (delta*np.array([self.ppu,-self.ppu])).tolist(),
                "minimum_neck_triangle_area_ratio": min(ratios),
                "flipped_or_collapsed_visible_triangles": sum(r <= 0 for r in ratios),
                "collar_body_scales": [(target[1]-standing_floor)/(anchor[1]-standing_floor) for anchor in (standing,seated)],
                "head_top_native": top.tolist(), "collar_native": target.tolist()}

def pose_sets():
    current = []
    for path in sorted((ROOT/"assets/motions").glob("*.motion.json")):
        clip = json.loads(path.read_text(encoding="utf8"))
        for i, frame in enumerate(clip.get("keyframes", [])):
            current.append((path.stem+f"-{i}", clip.get("constants", {})|frame.get("parameters", {})))
    stages = [("standing",0,0), ("folded",.74,.9), ("switch-before",.834999,.9),
              ("switch-after",.835001,.9), ("seated",1,1)]
    boundaries = []
    for stage, busy, sit in stages:
        for x,y,z,body in itertools.product([-45,-30,0,30,45],[-30,-20,0,20,30],[-30,-18,0,18,30],[-8,0,8]):
            params = {"ParamBusyLaptop":busy,"ParamSitPose":sit,"ParamAngleX":x,"ParamAngleY":y,
                      "ParamAngleZ":z,"ParamBodyAngleX":-body,"ParamBodyAngleZ":body}
            boundaries.append((f"{stage}-x{x}-y{y}-z{z}-body{body}",params))
    return current, boundaries

def main():
    pixels, receipt = extraction()
    native = Audit(pixels)
    current, bounds = pose_sets()
    report = {"kind":"read_only_runtime_neck_geometry_audit",
              "scope":{"real_native_core":True,"canvas_posture_and_neck_math_reproduced":True,
                       "physics_evaluated":False,"rendered_pixels_evaluated":False,"production_modified":False},
              "inputs":{"moc":digest(MODEL/"whale-girl-layered-draft.moc3"),"atlas":digest(ATLAS),
                        "ownership":digest(MODEL/"material-ownership/ownership.json"),
                        "neck_cpp":digest(ROOT/"src/CubismNeckBinding.cpp"),"canvas_cpp":digest(ROOT/"src/CubismCanvas.cpp")},
              "face_vertex_count":native.native.vertex_counts[native.face],
              "visible_neck_triangles":native.visible_triangles,
              "native_parameter_ranges":{key:native.native.ranges[key] for key in ("ParamAngleX","ParamAngleY","ParamAngleZ","ParamBodyAngleX","ParamBodyAngleZ")}}
    for label, poses in (("current_authored_keyframes",current),("native_boundaries",bounds)):
        rows = [native.evaluate(params,name) for name,params in poses]
        report[label] = {"pose_count":len(rows),"flipped_pose_count":sum(row["flipped_or_collapsed_visible_triangles"]>0 for row in rows),
                         "worst_triangle":min(rows,key=lambda x:x["minimum_neck_triangle_area_ratio"]),
                         "longest_neck":max(rows,key=lambda x:x["neck_length_pixels"]),
                         "shortest_neck":min(rows,key=lambda x:x["neck_length_pixels"]),
                         "minimum_body_scale":min(min(row["collar_body_scales"]) for row in rows),
                         "maximum_body_scale":max(max(row["collar_body_scales"]) for row in rows)}
        save(label+".json",rows)
    save("runtime-neck-audit.json", report)
    print(json.dumps({key:report[key] for key in ("face_vertex_count","visible_neck_triangles","current_authored_keyframes","native_boundaries")},ensure_ascii=False))

if __name__ == "__main__": main()
