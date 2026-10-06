"""Verify actual arbitrary Sit0 poses, including raw float bytes and colors."""
import argparse,ctypes as c,json,hashlib,sys
from pathlib import Path
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
sys.path.insert(0,str(ROOT/'tools'))
from probe_cubism_core import NativeModel,digest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--moc',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'.local'):raise ValueError('Fresh .local output required')
    source=ROOT/'assets/live2d/whale-girl/whale-girl-layered-draft.moc3'
    candidate=a.moc.resolve(strict=True);core=ROOT/'build/Live2DCubismCore.dll'
    classification=ROOT/'.local/authoring/sit-pose-20261004-r1/native-r8-regression-classification.json'
    pins={str(x):digest(x) for x in [source,candidate,core,classification,Path(__file__)]}
    original=NativeModel(core,source);edited=NativeModel(core,candidate)
    ids={n:i for i,n in enumerate(edited.drawable_ids)}
    for model in [original,edited]:
        for name in ['MultiplyColors','ScreenColors']:
            fn=getattr(model.library,'csmGetDrawable'+name);fn.restype=c.POINTER(c.c_float);fn.argtypes=[c.c_void_p]
    classified=json.loads(classification.read_text())['unexpected_standing_changes']
    poses=[{'name':r['frame'],'parameters':dict(r['parameters'],ParamSitPose=0.)} for r in classified]
    rng=np.random.default_rng(20261005)
    for i in range(128):
        params={'ParamSitPose':0.,'ParamBusyLaptop':float(i%2),'ParamGrassVisible':float((i//2)%2),
                'ParamBodyAngleX':float(rng.uniform(-10,10)),'ParamBodyAngleY':float(rng.uniform(-10,10)),
                'ParamBodyAngleZ':float(rng.uniform(-10,10)),'ParamBreath':float(rng.uniform(0,1)),
                'ParamArmLA':float(rng.uniform(-65,65)),'ParamArmRA':float(rng.uniform(-65,65)),
                'ParamBusyTypingL':float(rng.uniform(0,1)),'ParamBusyTypingR':float(rng.uniform(0,1)),
                'ParamLaptopRock':float(rng.uniform(-1,1))}
        poses.append({'name':f'seeded-arbitrary-{i}','parameters':params})
    failures=[];maximum_position=maximum_opacity=maximum_color=0.;hidden_failures=[];floor_failures=[]
    constant_failures=[]
    for i,n in enumerate(original.drawable_ids):
        j=ids[n]
        for field in ['ConstantFlags','TextureIndices','MaskCounts']:
            if original.get('Drawable'+field)[i]!=edited.get('Drawable'+field)[j]:constant_failures.append([n,field])
        count=original.get('DrawableMaskCounts')[i];x=original.get('DrawableMasks')[i];y=edited.get('DrawableMasks')[j]
        if [original.drawable_ids[x[k]] for k in range(count)]!=[edited.drawable_ids[y[k]] for k in range(count)]:constant_failures.append([n,'masks'])
    for pose in poses:
        params={k:v for k,v in pose['parameters'].items() if k in original.parameter_ids}
        event={'name':pose['name'],'parameters':params,'part_opacities':{}}
        original.update(event);edited.update(event)
        ap=original.positions();bp=edited.positions()
        ac={name:np.ctypeslib.as_array(getattr(original.library,'csmGetDrawable'+name)(original.model),shape=(len(original.drawable_ids)*4,)).reshape(-1,4).copy() for name in ['MultiplyColors','ScreenColors']}
        bc={name:np.ctypeslib.as_array(getattr(edited.library,'csmGetDrawable'+name)(edited.model),shape=(len(edited.drawable_ids)*4,)).reshape(-1,4).copy() for name in ac}
        for i,n in enumerate(original.drawable_ids):
            j=ids[n];x=np.asarray(ap[i],dtype=np.float32);y=np.asarray(bp[j],dtype=np.float32)
            alpha=np.asarray([original.get('DrawableOpacities')[i]],dtype=np.float32);beta=np.asarray([edited.get('DrawableOpacities')[j]],dtype=np.float32)
            dp=float(abs(x-y).max());da=float(abs(alpha-beta).max());dc=max(float(abs(ac[name][i]-bc[name][j]).max()) for name in ac)
            maximum_position=max(maximum_position,dp);maximum_opacity=max(maximum_opacity,da);maximum_color=max(maximum_color,dc)
            fields=[]
            if x.tobytes()!=y.tobytes():fields.append('vertex_float_bits')
            if alpha.tobytes()!=beta.tobytes():fields.append('opacity_float_bits')
            for name in ac:
                if ac[name][i].tobytes()!=bc[name][j].tobytes():fields.append(name+'_float_bits')
            if original.get('DrawableDrawOrders')[i]!=edited.get('DrawableDrawOrders')[j]:fields.append('draw_order')
            if fields and len(failures)<100:failures.append({'pose':pose['name'],'drawable':n,'fields':fields,'maximum_position':dp,'maximum_opacity':da,'maximum_color':dc})
        for n,j in ids.items():
            if n not in original.drawable_ids and edited.get('DrawableOpacities')[j]!=0 and len(hidden_failures)<100:hidden_failures.append([pose['name'],n,float(edited.get('DrawableOpacities')[j])])
        x=min(np.asarray(ap[original.drawable_ids.index(n)],dtype=np.float32)[:,1].min() for n in ['ArtMeshFootwearL','ArtMeshFootwearR'])
        y=min(np.asarray(bp[ids[n]],dtype=np.float32)[:,1].min() for n in ['ArtMeshFootwearL','ArtMeshFootwearR'])
        if x.tobytes()!=y.tobytes() and len(floor_failures)<100:floor_failures.append([pose['name'],float(x),float(y)])
    if any(digest(Path(path))!=sha for path,sha in pins.items()):raise ValueError('Inputs changed during probe')
    passed=not(failures or hidden_failures or floor_failures or constant_failures)
    report={'schema_version':1,'kind':'actual_Core_standing_master_restore_regression','passed':passed,
            'inputs':pins,'classified_arbitrary_standing_poses':len(classified),'seeded_random_arbitrary_poses':128,
            'old_drawables':len(original.drawable_ids),'total_poses':len(poses),'all_old_vertex_opacity_multiply_screen_float_bits_exact':not failures,
            'maximum_native_position_difference':maximum_position,'maximum_opacity_difference':maximum_opacity,'maximum_color_difference':maximum_color,
            'constant_mask_flags_texture_differences':constant_failures,'failures_first_100':failures,
            'new_mesh_nonzero_Sit0_opacities_first_100':hidden_failures,'original_foot_floor_bit_differences_first_100':floor_failures,
            'scope':'Core only. Logical Sit0 passed directly to both Native models; no physics, renderer patches, matrix, pixels or visual approval.',
            'pending':['Native unchanged-standing decoded-RGBA regression','positive-Sit author floor-reference selection']}
    out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('inputs','failures_first_100')},indent=2))
    if not passed:raise SystemExit(1)

if __name__=='__main__':main()
