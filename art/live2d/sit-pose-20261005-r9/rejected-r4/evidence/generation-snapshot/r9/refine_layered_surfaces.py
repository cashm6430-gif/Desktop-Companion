"""R9 surface prototype: supported navy plane + hanging features, closed knees.

Raster material is never edited here. Source backing is a separate generated
continuous cloth. Apron/bows are overlays with their own, readable projection.
This remains a diagnostic pose, not an approved 3D calf contract or adoption.
"""
from pathlib import Path
import argparse,copy,itertools,json,sys
import numpy as np
from PIL import Image

HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]
sys.path[:0]=[str(HERE),str(ROOT/'tools'),str(ROOT/'.local/authoring/sit-pose-20261004-r1/geometry/r9-source-design')]
from body_crouch_r9 import BodyRig,SOURCE,read
from build_structural_prototype import delta_row,smooth
from probe_cubism_core import digest
from verify_component_coverage import CoverageModel


def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output.resolve()
    if out.exists():raise ValueError('Fresh output required.')
    out.mkdir(parents=True);tool_hash=digest(Path(__file__))
    source_plan=HERE/'prototype-r3/author-plan-arm-scope.json';plan=read(source_plan)
    spec_source=read(HERE/'front-cloth/uv-fragments-r1.json')
    source_specs={x['id']:x for x in spec_source['insert_mesh_specs']}
    rig=BodyRig();rig.set_deep_profile(328)
    candidate=HERE/'api/prototype-r3-20261005T125504Z-70340a61/export/edited.model3.json'
    runtime=CoverageModel(candidate)
    neutral={**rig.default,'ParamBusyLaptop':0.,'ParamHandGround':1.}
    calf_rows={m['id']:m for m in plan['mesh_grids'] if m['id'].startswith(('ArtMeshSitStocking','ArtMeshSitKnee'))}
    ankle_local={}
    for side,pivot in [('L',[555.,1134.]),('R',[698.,1134.])]:
        ankle_local[side]=rig.inverse('DeformPair_Footwear',[pivot],neutral)[0]
    for coordinate in itertools.product(range(3),repeat=4):
        p1=rig.context(coordinate,1.);runtime.update({'parameters':p1})
        final_centers={side:runtime.source_xy(runtime.positions['ArtMeshSitKneeFront'+side])[0] for side in ['L','R']}
        for name,row in calf_rows.items():
            row['keyforms']=[f for f in row['keyforms'] if tuple(f['coordinate'][1:])!=coordinate]
        keys=next(iter(calf_rows.values()))['replace_axes'][0]['keys']
        for si,sit in enumerate(keys):
            if sit==0:continue
            params=rig.context(coordinate,sit);runtime.update({'parameters':params})
            for side in ['L','R']:
                center=runtime.source_xy(runtime.positions['ArtMeshSitKneeFront'+side])[0]
                ankle=rig.points('DeformPair_Footwear',ankle_local[side],rig.context(coordinate,0.))[0]
                move=(final_centers[side][0]-center[0])*smooth(sit/.35)
                for kind in ['KneeFront','Stocking']:
                    name='ArtMeshSit'+kind+side;node=rig.D[name]
                    desired=runtime.source_xy(runtime.positions[name]).copy()
                    if kind=='KneeFront':desired[:,0]+=move
                    else:
                        weight=np.clip((ankle[1]-desired[:,1])/max(1.,ankle[1]-center[1]),0,1.25)
                        desired[:,0]+=move*weight
                    local,_=rig.inverse(node['getParentDeformerId-lmpY1tE'],desired,params,True)
                    base=np.asarray(node['getMesh']['getPositions'],np.float32).reshape(-1,2)
                    calf_rows[name]['keyforms'].append(delta_row(base,local,[si,*coordinate]))
        print('CLOSED_KNEE_CONTEXT '+str(coordinate),flush=True)

    keys=plan['insert_meshes'][0]['axes'][0]['keys']
    table=read(HERE/'front-cloth/uv-layer-execution-table-r2.json')['ownership']
    curve=np.asarray(table['lower_frill_top_seed'],float)
    def surface(sit,points):
        x,y=np.asarray(points).T
        waist=float(np.interp(sit,[0,.35,.65,.8,.92,1],[745,805,984,984,858,858]))
        width=float(np.interp(sit,[0,.35,.65,1],[.50,.55,.55,.55]))
        slope=float(np.interp(sit,[0,.35,.65,.8,.92,1],[.40,.39,.075,.075,.18,.18]))
        fringe=float(np.interp(sit,[0,.35,.65,.8,.92,1],[.4,.28,.28,.28,.28,.28]))
        drape=float(np.interp(sit,[0,.35,.65,1],[10,60,90,85]))
        fold_y=np.interp(x,curve[:,0],curve[:,1])
        projected=np.where(y<=fold_y,(y-379)*slope,(fold_y-379)*slope+(y-fold_y)*fringe)
        yy=waist+projected+drape*(abs(x-627)/627)**1.5*np.clip((y-360)/600,0,1)
        return np.c_[627+(x-627)*width,yy]

    rows=[]
    for row in plan['insert_meshes']:
        source=source_specs[row['id']];points=np.asarray(source['source_pixel_positions']).reshape(-1,2)
        base=np.asarray(row['positions'],np.float32).reshape(-1,2);row['keyforms']=[]
        for si,sit in enumerate(keys):
            target=surface(sit,points)
            waist=float(np.interp(sit,[0,.35,.65,.8,.92,1],[745,805,984,984,858,858]))
            if row['id']=='ArtMeshR9Apron':
                sx=float(np.interp(sit,[0,.35,.65,.8,.92,1],[.43,.38,.34,.34,.38,.38]))
                sy=float(np.interp(sit,[0,.35,.65,.8,.92,1],[.43,.37,.24,.24,.35,.35]))
                target=np.c_[627+(points[:,0]-627)*sx,waist+(points[:,1]-379)*sy]
            elif row['id'].startswith('ArtMeshR9Bow'):
                center=np.array([362.,675.]) if row['id'].endswith('L') else np.array([885.,675.])
                anchor=surface(sit,[center])[0]
                scale=float(np.interp(sit,[0,.35,.65,1],[.48,.44,.38,.43]))
                target=anchor+(points-center)*scale
            elif row['id']=='ArtMeshR9WaistFoldUnder':
                # Replaced by complete navy backing; no floating white rear strip.
                for f in row['channels'][0]['keyforms']:f['value']=0.
            local,_=rig.inverse(row['parent_id'],target,{**neutral,'ParamSitPose':sit},True)
            row['keyforms'].append(delta_row(base,local,[si]))
        rows.append(row)
    # A complete physical backing behind the independently projected apron/bows.
    # Its grid stops at the front fold/ruffle attachment curve, not below the hem.
    backing=ROOT/'art/live2d/sit-pose-20261005-r9/materials/navy-front-backing.png'
    image=Image.open(backing)
    if image.size!=(1254,1254):raise ValueError('Backing material must preserve the reference canvas.')
    xs=np.linspace(0,1254,33);fractions=np.linspace(0,1,13)
    points=np.array([[x,340+t*(np.interp(x,curve[:,0],curve[:,1])-340)] for t in fractions for x in xs])
    indices=[]
    for yy in range(12):
        for xx in range(32):
            q=yy*33+xx;indices.extend([q,q+33,q+1,q+1,q+33,q+34])
    base,_=rig.inverse('DeformBodyZBreath',points,neutral)
    base=np.asarray(base,np.float32)
    row={'id':'ArtMeshR9NavyBacking','name':'Continuous actual navy backing under slope, apron and bows',
         'template_id':'ArtMeshSitFrontSkirt','parent_id':'DeformBodyZBreath','part_id':'PartExtra',
         'texture_page':9,'positions':base.reshape(-1).tolist(),
         'uvs':np.asarray(points/1254,np.float32).reshape(-1).tolist(),'indices':indices,
         'axes':[{'parameter':'ParamSitPose','keys':keys}],'keyforms':[],
         'draw_order':10,'opacity':0.,'channels':[{'channel':'OPACITY','initial_value':0.,
         'append_axes':[{'parameter':'ParamSitPose','keys':keys}],
         'keyforms':[{'coordinate':[i],'value':smooth(s/.0875)} for i,s in enumerate(keys)]}]}
    for si,sit in enumerate(keys):
        local,_=rig.inverse(row['parent_id'],surface(sit,points),{**neutral,'ParamSitPose':sit},True)
        row['keyforms'].append(delta_row(base,local,[si]))
    # Backing is inserted before decorative plane draws at the same order.
    plan['insert_meshes']=[row]+rows
    plan['append_pages'].append({'path':str(backing),'sha256':digest(backing)})
    plan['kind']='R9_layered_surface_diagnostic_prototype_pending_contact_and_visual_approval'
    plan['provenance'].update({'surface_source_plan_sha256':digest(source_plan),'surface_generator_sha256':tool_hash,
                               'surface_actual_reference_MOC_sha256':digest(candidate.with_suffix('').with_suffix('.moc3'))})
    plan['limits'].append('Knee adduction and diagnostic depth projection require actual bone/material review. Apron/bows are independent overlays; navy backing fills their former UV ownership holes.')
    if digest(Path(__file__))!=tool_hash:raise ValueError('Tool changed during generation.')
    dest=out/'author-plan.json';dest.write_text(json.dumps(plan,separators=(',',':'),allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':str(dest),'sha256':digest(dest),'insert_meshes':len(plan['insert_meshes'])},indent=2),flush=True)


if __name__=='__main__':main()
