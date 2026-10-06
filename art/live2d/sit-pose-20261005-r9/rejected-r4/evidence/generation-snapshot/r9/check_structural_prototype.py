"""Actual Cubism Core gates for the isolated R9 structural prototype.

No Qt, GPU, physics, renderer correction, generator proxy or adoption. Source
is the frozen eight-page master-restored CMO/MOC family. Raw float32 buffers
are read through ctypes. Neutral geometry gates do not certify other angles.
"""
from pathlib import Path
import argparse,ctypes as c,itertools,json,sys
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SOURCE=ROOT/'.local/authoring/sit-pose-20261004-r1/api/standing-restore-v1/probe-20261005T065840Z-77b66c6f/export/edited.model3.json'
sys.path.insert(0,str(ROOT/'tools'))
from probe_cubism_core import NativeModel,model_family,find_core,digest,Vec2

LEG_IDS=['ArtMeshSitKneeFrontL','ArtMeshSitKneeFrontR','ArtMeshSitStockingL','ArtMeshSitStockingR',
         'ArtMeshSitShoeL','ArtMeshSitShoeR','ArtMeshAuthorFloorReferenceL','ArtMeshAuthorFloorReferenceR']
KEY_SITS=[.35,.65,.8,.92,1.]


class RawModel:
    def __init__(self,path,core):
        moc,family=model_family(path);self.family=family;self.native=NativeModel(core,moc)
        self.ids=self.native.drawable_ids;self.lookup={n:i for i,n in enumerate(self.ids)}
        self.topology={}
        idx=self.native.get('DrawableIndices')
        for i,n in enumerate(self.ids):
            self.topology[n]=np.ctypeslib.as_array(idx[i],shape=(self.native.index_counts[i],)).copy().reshape(-1,3)
        for field in ['MultiplyColors','ScreenColors']:
            fn=getattr(self.native.library,'csmGetDrawable'+field);fn.restype=c.POINTER(c.c_float);fn.argtypes=[c.c_void_p]
        size,origin=Vec2(),Vec2();ppu=c.c_float()
        self.native.api['ReadCanvasInfo'](self.native.model,c.byref(size),c.byref(origin),c.byref(ppu))
        self.ppu=float(ppu.value);self.origin=np.asarray([origin.X,origin.Y],float)

    def update(self,params):
        values={k:v for k,v in params.items() if k in self.native.parameter_ids}
        trace=self.native.update({'name':'structural','parameters':values,'part_opacities':{}})
        if any(x['was_clamped'] for x in trace.values()):raise ValueError('Prototype fixture clamped a Native parameter')
        buffer=self.native.get('DrawableVertexPositions');self.positions={}
        for i,n in enumerate(self.ids):
            self.positions[n]=np.ctypeslib.as_array(buffer[i],shape=(self.native.vertex_counts[i],)).view(np.float32).reshape(-1,2).copy()
        self.opacity=np.ctypeslib.as_array(self.native.get('DrawableOpacities'),shape=(len(self.ids),)).copy()
        self.colors={field:np.ctypeslib.as_array(getattr(self.native.library,'csmGetDrawable'+field)(self.native.model),shape=(len(self.ids)*4,)).reshape(-1,4).copy() for field in ['MultiplyColors','ScreenColors']}

    def world(self,name):return self.positions[name].astype(float)*[self.ppu,-self.ppu]+self.origin


def area2(points,triangles):
    a,b,d=points[triangles[:,0]],points[triangles[:,1]],points[triangles[:,2]]
    return (b[:,0]-a[:,0])*(d[:,1]-a[:,1])-(b[:,1]-a[:,1])*(d[:,0]-a[:,0])


def fit_rigid(a,b):
    ac,bc=a.mean(0),b.mean(0);aa,bb=a-ac,b-bc
    u,s,v=np.linalg.svd(aa.T@bb);rotation=u@v
    if np.linalg.det(rotation)<0:u[:,-1]*=-1;rotation=u@v
    translation=bc-ac@rotation;fitted=a@rotation+translation;residual=np.linalg.norm(fitted-b,axis=1)
    scale=float(np.sum((aa@rotation)*bb)/np.sum(aa*aa))
    return {'rotation_degrees':float(np.degrees(np.arctan2(rotation[0,1],rotation[0,0]))),
            'translation_source_px':translation.tolist(),'determinant':float(np.linalg.det(rotation)),
            'best_rigid_rms_source_px':float(np.sqrt(np.mean(residual**2))),
            'best_rigid_maximum_source_px':float(residual.max()),'diagnostic_best_uniform_scale':scale,
            'source_rms_radius_px':float(np.sqrt(np.mean(np.sum(aa*aa,axis=1)))),
            'candidate_rms_radius_px':float(np.sqrt(np.mean(np.sum(bb*bb,axis=1))))}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--model',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();target=args.model.resolve(strict=True);out=args.output.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'.local'):raise ValueError('Fresh .local output required')
    if not out.parent.is_dir():raise ValueError('Output parent must already exist')
    core=find_core();source=RawModel(SOURCE,core);candidate=RawModel(target,core)
    formal_path=ROOT/'assets/live2d/whale-girl/whale-girl-layered-draft.moc3'
    original_inventory=NativeModel(core,formal_path).drawable_ids
    if len(original_inventory)!=34 or any(n not in source.lookup or n not in candidate.lookup for n in original_inventory):raise ValueError('Original34 actual Native inventory mismatch')
    if source.ppu!=candidate.ppu or not np.array_equal(source.origin,candidate.origin):raise ValueError('Source/candidate Native canvas convention differs')
    pins={str(x):digest(x) for x in [core,formal_path,Path(__file__)]}
    for model in [source,candidate]:
        for row in model.family:pins[row['path']]=row['sha256']
    contexts=[{}]+[dict(zip(['ParamBodyAngleX','ParamBodyAngleY','ParamBodyAngleZ','ParamBreath'],values)) for values in itertools.product([-10.,0.,10.],[-10.,0.,10.],[-10.,0.,10.],[0.,.5,1.])]
    rng=np.random.default_rng(20261005)
    contexts += [{'ParamBodyAngleX':float(rng.uniform(-10,10)),'ParamBodyAngleY':float(rng.uniform(-10,10)),
                  'ParamBodyAngleZ':float(rng.uniform(-10,10)),'ParamBreath':float(rng.uniform(0,1)),
                  'ParamArmLA':float(rng.uniform(-65,65)),'ParamArmRA':float(rng.uniform(-65,65)),
                  'ParamBusyTypingL':float(rng.uniform(0,1)),'ParamBusyTypingR':float(rng.uniform(0,1)),
                  'ParamLaptopRock':float(rng.uniform(-1,1))} for _ in range(40)]
    standing_errors=[];standing_max={'vertices_native':0.,'opacity':0.,'multiply_or_screen':0.};hidden=[];standing_count=0
    new_ids=[n for n in candidate.ids if n not in original_inventory]
    for ci,context in enumerate(contexts):
        for busy in [0.,1.]:
            params=dict(context,ParamSitPose=0.,ParamBusyLaptop=busy);source.update(params);candidate.update(params);standing_count+=1
            for name in original_inventory:
                i,j=source.lookup[name],candidate.lookup[name];a,b=source.positions[name],candidate.positions[name]
                fields=[]
                if a.shape!=b.shape:raise ValueError('Original vertex count changed '+name)
                standing_max['vertices_native']=max(standing_max['vertices_native'],float(abs(a-b).max()))
                if a.tobytes()!=b.tobytes():fields.append('vertex_float32_bits')
                standing_max['opacity']=max(standing_max['opacity'],float(abs(source.opacity[i]-candidate.opacity[j])))
                if source.opacity[i:i+1].tobytes()!=candidate.opacity[j:j+1].tobytes():fields.append('opacity_float32_bits')
                for field in source.colors:
                    standing_max['multiply_or_screen']=max(standing_max['multiply_or_screen'],float(abs(source.colors[field][i]-candidate.colors[field][j]).max()))
                    if source.colors[field][i].tobytes()!=candidate.colors[field][j].tobytes():fields.append(field+'_float32_bits')
                if fields:standing_errors.append({'context':ci,'parameters':params,'drawable':name,'fields':fields})
            for name in new_ids:
                alpha=float(candidate.opacity[candidate.lookup[name]])
                if alpha!=0.:hidden.append({'context':ci,'parameters':params,'drawable':name,'opacity':alpha})
    leg_rows=[];worst=None
    for sit in KEY_SITS:
        params={'ParamSitPose':sit};source.update(params);candidate.update(params)
        for name in LEG_IDS:
            if name not in source.lookup or name not in candidate.lookup:raise ValueError('Missing leg/floor '+name)
            a,b=source.world(name),candidate.world(name)
            if a.shape!=b.shape:raise ValueError('Leg/floor vertex count changed '+name)
            distances=np.linalg.norm(a-b,axis=1);i=int(distances.argmax())
            row={'parameters':params,'drawable':name,'maximum_residual_source_px':float(distances[i]),
                 'maximum_component_residual_source_px':float(abs(a-b).max()),'worst_vertex':i,
                 'source_world_xy':a[i].tolist(),'candidate_world_xy':b[i].tolist(),
                 'passed':bool(distances[i]<=.1)}
            leg_rows.append(row)
            if worst is None or row['maximum_residual_source_px']>worst['maximum_residual_source_px']:worst=row
    # Actual source IDs, not guessed candidate aliases: the four established
    # head surfaces are selected from the source Native inventory.
    roles={'face':'ArtMeshFace','front_hair':'ArtMeshFrontHair','headwear':'ArtMeshHeadwear','back_hair':'ArtMeshBackHair2'}
    head_ids={role:next((n for n in source.ids if n==name),None) for role,name in roles.items()}
    if any(n is None or n not in candidate.lookup for n in head_ids.values()):raise ValueError('Four actual source head surfaces unavailable')
    source.update({'ParamSitPose':0.});base={n:source.world(n).copy() for n in head_ids.values()}
    candidate.update({'ParamSitPose':.65});posed={n:candidate.world(n).copy() for n in head_ids.values()}
    fits={n:fit_rigid(base[n],posed[n]) for n in head_ids.values()}
    common=fit_rigid(np.concatenate(list(base.values())),np.concatenate(list(posed.values())))
    edge_stats={}
    for n in head_ids.values():
        tri=source.topology[n];edges=np.unique(np.sort(np.vstack([tri[:,[0,1]],tri[:,[1,2]],tri[:,[2,0]]]),axis=1),axis=0)
        x=np.linalg.norm(base[n][edges[:,1]]-base[n][edges[:,0]],axis=1);y=np.linalg.norm(posed[n][edges[:,1]]-posed[n][edges[:,0]],axis=1);valid=x>1e-4
        ratios=y[valid]/x[valid];edge_stats[n]={'nondegenerate_edges':int(valid.sum()),'minimum_length_ratio':float(ratios.min()),'maximum_length_ratio':float(ratios.max()),'maximum_edge_length_change_source_px':float(abs(y[valid]-x[valid]).max())}
    head_pass=bool(common['best_rigid_maximum_source_px']<=.01 and abs(common['diagnostic_best_uniform_scale']-1)<=1e-4)
    # Neutral-only actual Core sweep. Source at the same parameter value
    # distinguishes pre-existing degeneracies/flips from prototype regressions.
    geometric=[];finite=[];zero=[];flip=[];new_material_first_areas={};sample_sits=sorted(set(np.linspace(.005,1.,200).tolist()+KEY_SITS))
    epsilon_area_px2=1e-8
    for sit in sample_sits:
        params={'ParamSitPose':float(sit)};source.update(params);candidate.update(params)
        for name in candidate.ids:
            points=candidate.world(name)
            if not np.all(np.isfinite(points)):
                finite.append({'parameters':params,'drawable':name,'nonfinite_indices':np.argwhere(~np.isfinite(points)).tolist()});continue
            triangles=candidate.topology[name];areas=area2(points,triangles)
            baseline=area2(source.world(name),source.topology[name]) if name in source.lookup and np.array_equal(triangles,source.topology[name]) else None
            badzero=np.flatnonzero(abs(areas)<=epsilon_area_px2)
            newzero=badzero if baseline is None else badzero[abs(baseline[badzero])>epsilon_area_px2]
            if baseline is None:
                # New material has no source mesh. Compare its real first
                # positive-Sit orientation to subsequent actual Core poses.
                new_material_first_areas.setdefault(name,areas.copy())
                reference_areas=new_material_first_areas[name]
            else:reference_areas=baseline
            badflip=np.flatnonzero(areas*reference_areas<0)
            if len(badzero):zero.append({'parameters':params,'drawable':name,'zero_triangles':badzero.tolist(),'new_zero_triangles':newzero.tolist(),'source_baseline_available':baseline is not None})
            if len(badflip):flip.append({'parameters':params,'drawable':name,'triangles':badflip.tolist(),'source_baseline_available':baseline is not None,
                                         'orientation_reference':'source_same_pose' if baseline is not None else 'candidate_first_positive_Sit'})
    for path,signature in pins.items():
        if digest(Path(path))!=signature:raise ValueError('Model/source/tool inputs changed during probe')
    new_zero_count=sum(len(r['new_zero_triangles']) for r in zero)
    passed=not standing_errors and not hidden and all(r['passed'] for r in leg_rows) and head_pass and not finite and not new_zero_count and not flip
    report={'schema_version':1,'kind':'actual_Core_R9_neutral_structural_prototype','passed_within_declared_scope':passed,
            'prototype_status':'engineering_pass_within_scope_visual_pending' if passed else 'prototype_pending',
            'inputs':pins,'source_model':str(SOURCE),'candidate_model':str(target),
            'canvas':{'pixels_per_unit':candidate.ppu,'origin':candidate.origin.tolist()},
            'standing':{'contexts':len(contexts),'busy_keys':[0.,1.],'poses':standing_count,'original_native_ids':original_inventory,
                        'float32_byte_comparison':True,'maximum_numeric_differences':standing_max,'failures':standing_errors,
                        'new_mesh_nonzero_opacity_failures':hidden,'passed':not standing_errors and not hidden},
            'leg_floor_neutral':{'ids':LEG_IDS,'sit_keys':KEY_SITS,'maximum_allowed_residual_source_px':.1,'worst':worst,'all_rows':leg_rows,'passed':all(r['passed'] for r in leg_rows)},
            'head_neutral_only':{'source_surface_ids':head_ids,'source_sit':0.,'candidate_sit':.65,'other_parameters':'actual Native defaults',
                                 'best_common_rigid_fit':common,'per_surface_rigid_fits':fits,'edge_length_diagnostics':edge_stats,
                                 'declared_rigid_maximum_px':.01,'declared_uniform_scale_tolerance':1e-4,'passed':head_pass,
                                 'head_angles_tested':False,'raw_surface_scope':'All actual vertices, including invisible/mixed chart portions; no renderer material cuts.'},
            'neutral_positive_sit_sweep':{'sample_count':len(sample_sits),'sit_values':sample_sits,'area_epsilon_source_px2':epsilon_area_px2,
                                         'nonfinite':finite,'zero_area_records':zero,'new_zero_triangle_count':new_zero_count,'orientation_changes_vs_source_same_pose':flip,
                                         'new_material_orientation_reference_sit':sample_sits[0],
                                         'new_material_orientation_reference_ids':list(new_material_first_areas),
                                         'new_material_winding_limit':'Neutral sampled actual Core orientation only; no claim of analytic interval or other Body/Arm/Typing/Rock contexts.'},
            'approval':{'visual':'pending_native_review','adoption':'not_requested'},
            'limits':['Not a full-domain Body/Arm/Typing/Rock/head-angle gate.','Core only: no GPU/Native screenshots, physics, renderer patches or common floor/prop registration matrix.',
                      'Source raw-native world comparisons do not certify knee/skirt contact, material ownership or seams.','Actual Native visual review and stable R9 full regression remain required.']}
    out.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'report':str(out),'passed_within_scope':passed,'standing_failures':len(standing_errors),'hidden_failures':len(hidden),
                      'leg_floor_worst':worst,'head_best_rigid':common,'nonfinite_records':len(finite),'new_zero_triangles':new_zero_count,'orientation_records':len(flip)},indent=2))
    if not passed:raise SystemExit(1)

if __name__=='__main__':main()
