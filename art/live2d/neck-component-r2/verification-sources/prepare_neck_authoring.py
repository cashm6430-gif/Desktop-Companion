"""Create isolated neck artwork/rig instructions and audit the denser proposal."""
from pathlib import Path
import itertools
import json

import numpy as np
from PIL import Image

from audit_neck_surface import Audit, OUT, ROOT, MODEL, OWN, ATLAS, digest, save

def main():
    original = np.array(Image.open(OUT/"neck-original.png").convert("RGBA"))
    atlas = Image.open(ATLAS).convert("RGBA")
    chart = OWN["source_charts"]["face"]
    x0,y0,x1,y1 = chart["atlas"]
    dx,dy = chart["offset"]
    face = Image.new("RGBA", tuple(OWN["source_canvas"]))
    face.paste(atlas.crop((x0,y0,x1,y1)), (x0+dx,y0+dy))
    face = np.array(face)
    author = original.copy()
    # Upper overlap is copied from the actual skin under the original jaw.
    # It remains behind the retained main Face and has zero bottom influence.
    author[562:568,600:656] = face[562:568,600:656]
    # The small lower tail uses the original neck interior's RGBA colours.
    # Main standing/seated collar artwork occludes this tail; it is not a
    # second fixed skin surface and never changes the chest below the bow.
    for y in range(592,598):
        author[y,624:633] = face[582,624:633]
    # Canonical transparent RGB makes the single cropped PSD layer's decoded
    # full-canvas RGBA exactly reproducible, without changing visible pixels.
    author[author[:,:,3] == 0,:3] = 0
    Image.fromarray(author).save(OUT/"neck-authoring.png")
    bounds = Image.fromarray(author).getbbox()
    Image.fromarray(author).crop(bounds).save(OUT/"neck-authoring-crop.png")
    columns = np.linspace(555,682,9).tolist()
    rows = [562,568,570,578,584,588,598]
    grid = [[x,y] for y in rows for x in columns]
    indices = []
    for row in range(len(rows)-1):
        for column in range(len(columns)-1):
            a = row*len(columns)+column
            indices.extend([a,a+1,a+len(columns),a+1,a+len(columns)+1,a+len(columns)])
    t = [min(1,max(0,(y-570)/18)) for x,y in grid]
    weights = [v*v*(3-2*v) for v in t]
    # A unit-positive Curve peaks at 2 source pixels, never moves either
    # endpoint, and is zero on both hidden overlap bands.
    curves = [8*v*(1-v) for v in t]
    design = {
        "kind":"isolated_neck_auxiliary_author_model_design", "status":"candidate_not_adopted",
        "art":{"full_canvas":[1254,1254],"source_png":str(OUT/"neck-authoring.png"),
               "source_png_sha256":digest(OUT/"neck-authoring.png"),"source_bbox":list(bounds),
               "original_exact_pixels_png":str(OUT/"neck-original.png"),
               "hidden_overlap":{"upper":[600,562,656,568],"upper_method":"original Face RGBA copied at identical coordinates",
                                 "lower":[624,592,633,598],"lower_method":"original source y582 neck-interior RGBA copied downward",
                                 "upper_occluder":"retained main Face jaw", "lower_occluder":"visible main body collar",
                                 "face_features_modified":False,"chest_at_or_below_y607_modified":False}},
        "mesh":{"column_count":9,"row_count":7,"vertex_count":63,"triangle_count":96,
                "columns_source_x":columns,"rows_source_y":rows,"positions_source":grid,
                "triangle_indices":indices,"intent":"retained source UVs, dense independent topology; no full Face mesh reuse"},
        "anchors":{"jaw":{"center":[628,570],"tangent_left":[601,570],"tangent_right":[655,570],
                            "sample_surface":"current main Face Native UV triangles after Update"},
                   "standing_collar":{"center":[628,588],"tangent_left":[615,588],"tangent_right":[641,588],
                                       "sample_surface":"main Topwear after floor-pinned alignment"},
                   "seated_collar":{"center":[628,588],"tangent_left":[615,588],"tangent_right":[641,588],
                                     "sample_surface":"main BusyTorso after sole shift and floor-pinned alignment"}},
        "parameters":{"ParamNeckBottomX":{"min":-80,"default":0,"max":80,"units":"source px; positive right","vertex_weights":weights},
                      "ParamNeckBottomY":{"min":-80,"default":0,"max":80,"units":"source px; positive Native up (negative source Y)","vertex_weights":weights},
                      "ParamNeckCurve":{"min":-1,"default":0,"max":1,"units":"unit controls +/-2 source-pixel midpoint lateral curve","vertex_source_x_deltas_at_plus1":curves}},
        "parameter_responsibilities":{"ParamPosture":"future shape tuning only; selects body and middle width/curve, never applies a second endpoint translation",
                                       "ParamAngleXYZ":"main model resolves all face/head motion; its surface gives the auxiliary baseline and jaw tangent",
                                       "ParamNeckBottomXY":"authored Core differential from zero-pose is added once to current main Face surface evaluated at each auxiliary source vertex",
                                       "ParamNeckCurve":"middle-only differential, both endpoints zero; default0 until Native review validates chosen motion"},
        "runtime_mapping":{"default_native_model_pixel_scale":1254,"recipe":[
            "Keep production MOC and its mouth/head/arm bindings unchanged; keep existing main neck erase mask.",
            "Update posture/body-floor/collar alignment first; obtain continuously aligned visible collar C and main Face source tip H=(628,588).",
            "Drive authored BottomX/Y with (C-H)*1254 in Native x/y coordinates; disable implicit model head/body deformers.",
            "For each auxiliary UV vertex obtain its registered source position s, evaluate main Face F(s) using UV triangle barycentrics, and add authored Native Core position minus its zero-parameter position.",
            "Use the same final model MVP; copy effective Face opacity and color. Do not apply head XYZ a second time inside the auxiliary model.",
            "Render body-underpaint, then auxiliary neck, then original main character so jaw/hair and front collar occlude the overlap."]},
        "acceptance":{"existing_authored_keyframe_count":67,"moderate_domain":{"head_X":[-30,0,30],"head_Y":[-20,0,20],"head_Z":[-12,0,12],"body_Z":[-8,0,8],"posture_raw":[0,.74,.834999,.835001,1]},
                      "required_native_sequences":["standing head shake both directions","sit entry from standing","seated idle typing","seated head shake both directions","turn-ended-laptop entire 75-frame sequence","rise exit into standing"],
                      "measures":["upper overlap matches main jaw surface; zero head feature movement","lower source588 matches visible collar within .1 sourcepx","one skin owner; no stationary duplicate","visible triangle areas finite, same orientation, no zero area","no vertical texture seam at material switch","alpha silhouette and skin shape reviewed at Native desktop size and 2x","no changes to mouth/chest/arms/grass pixels"],
                      "full_native_extremes":"Separate physical contract: head/collar may cross at combined XYZ extremes. Do not silently label these passed; reject or author an occlusion pose before broadening the supported range."}
    }
    save("neck-rig-design.json",design)

    audit = Audit(original)
    for point in grid: audit.cached[("face",tuple(point))] = audit.anchor("face", point)
    grid_tri = np.array(indices).reshape(-1,3)
    active_points = audit.pixel_points
    visible = []
    for i,tri in enumerate(grid_tri):
        p = np.array(grid)[tri]
        matrix = np.stack([p[0]-p[2],p[1]-p[2]],axis=1)
        bary = np.linalg.solve(matrix,(active_points-p[2]).T).T
        if ((bary.min(axis=1)>=-1e-7)&(bary.sum(axis=1)<=1+1e-7)).any():visible.append(i)
    stages=[("standing",0,0),("folded",.74,.9),("switch-before",.834999,.9),("switch-after",.835001,.9),("seated",1,1)]
    evaluated=[]
    for stage,busy,sit in stages:
        for x,y,z,body in itertools.product([-30,0,30],[-20,0,20],[-12,0,12],[-8,0,8]):
            params={"ParamBusyLaptop":busy,"ParamSitPose":sit,"ParamAngleX":x,"ParamAngleY":y,
                    "ParamAngleZ":z,"ParamBodyAngleX":-body,"ParamBodyAngleZ":body}
            name=f"{stage}-x{x}-y{y}-z{z}-body{body}"
            old=audit.evaluate(params,name)
            baseline=np.array([audit.sample("face",tuple(point)) for point in grid])
            delta=np.array(old["delta_pixels"])*[1/1254,-1/1254]
            generated=baseline+np.array(weights)[:,None]*delta
            ratios=[]
            for i in visible:
                a,b=baseline[grid_tri[i]],generated[grid_tri[i]]
                signed_area=lambda p:float(np.linalg.det(np.stack([p[1]-p[0],p[2]-p[0]],axis=1)))
                ratios.append(signed_area(b)/signed_area(a))
            old["candidate_dense_minimum_area_ratio"]=min(ratios)
            old["candidate_dense_flipped_triangles"]=sum(value<=0 for value in ratios)
            evaluated.append(old)
    save("moderate-boundaries.json",evaluated)
    summary={"kind":"candidate_9x7_registered_dense_neck_math_audit","pose_count":len(evaluated),
             "scope":{"real_main_native_core":True,"candidate_auxiliary_native_core_generated":False,
                      "candidate_formula_only":True,"curve_value":0,"rendered_pixels_evaluated":False},
             "candidate_visible_triangle_count":len(visible),
             "original_sparse_flipped_pose_count":sum(r["flipped_or_collapsed_visible_triangles"]>0 for r in evaluated),
             "candidate_dense_flipped_pose_count":sum(r["candidate_dense_flipped_triangles"]>0 for r in evaluated),
             "candidate_worst":min(evaluated,key=lambda r:r["candidate_dense_minimum_area_ratio"]),
             "sparse_worst":min(evaluated,key=lambda r:r["minimum_neck_triangle_area_ratio"]),
             "longest":max(evaluated,key=lambda r:r["neck_length_pixels"])}
    save("dense-neck-moderate-audit.json",summary)
    print(json.dumps(summary,ensure_ascii=False))

if __name__=="__main__":main()
