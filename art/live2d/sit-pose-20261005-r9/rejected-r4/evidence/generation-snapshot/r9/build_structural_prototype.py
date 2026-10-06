"""Isolated R9 neutral-pose prototype; actual Core targets, public CMO edits.

This is NOT the completed multi-axis candidate. Ground sleeves and front-cloth
projection are first reviewed at Body/Arm/Typing/Rock defaults. Old leg world
trajectories are preserved across all 81 keyed body contexts. No raster editing.
"""
from pathlib import Path
import argparse
import itertools
import json
import math
import sys
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
OLD=ROOT/'.local/authoring/sit-pose-20261004-r1'
DESIGN=OLD/'geometry/r9-source-design'
sys.path[:0]=[str(DESIGN),str(ROOT/'tools')]
from body_crouch_r9 import BodyRig,SOURCE,GRAPH,read
from probe_cubism_core import digest
from verify_component_coverage import CoverageModel


def smooth(x):
    x=float(np.clip(x,0,1));return x*x*(3-2*x)


def rotate(points,angle):
    c,s=np.cos(angle),np.sin(angle)
    return np.asarray(points)@np.array([[c,s],[-s,c]])


def rigid_segment(points,old_start,old_end,new_start,new_end):
    a=old_end-old_start;b=new_end-new_start
    angle=math.atan2(b[1],b[0])-math.atan2(a[1],a[0])
    # One rigid rotation; all transverse AND longitudinal source lengths remain.
    return new_start+rotate(np.asarray(points)-old_start,angle)


def chart_from_native_UV(runtime,child,original):
    """Map cloned/clipped UV vertices to the real standing material triangle.

    Hidden Ground Sit0 is a protected historical keyform, not the rest sleeve.
    UV is used only as a chart correspondence; positions come from actual Core.
    """
    uv=runtime.topology[original]['uvs'].astype(float)
    points=runtime.source_xy(runtime.positions[original])
    triangles=runtime.topology[original]['indices']
    output=[]
    for q in runtime.topology[child]['uvs'].astype(float):
        best=None
        for tri in triangles:
            a,b,c=uv[tri];matrix=np.column_stack((b-a,c-a))
            if abs(np.linalg.det(matrix))<1e-15:continue
            w1,w2=np.linalg.solve(matrix,q-a);w=np.array([1-w1-w2,w1,w2])
            score=float(w.min())
            if best is None or score>best[0]:best=(score,w@points[tri])
            if score>=-1e-5:break
        if best is None or best[0]<-.01:raise ValueError(('Material UV not in original sleeve',child,q.tolist(),best))
        output.append(best[1])
    return np.asarray(output)


def delta_row(base,local,coordinate):
    return {'coordinate':list(coordinate),
            'position_deltas':np.asarray(local-base,dtype=np.float32).reshape(-1).tolist()}


def axes_of(node):
    return [{'parameter':a['getParameterId-WD9NFvw'],'keys':a['getKeys']}
            for a in node['getGeometryGrid']['getAxes']]


def remap_axis(old,new):
    return {'parameter':'ParamSitPose','keys':new,
            'source_key_indices':[min(range(len(old)),key=lambda i:abs(old[i]-x)) for x in new]}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--crouch-shin-projection',type=float,default=1.,
                        help='Orthographic projection cosine of backward shin hinge; diagnostic author prototype only.')
    args=parser.parse_args();out=args.output.resolve()
    if not .35<=args.crouch_shin_projection<=1:raise ValueError('Invalid orthographic hinge cosine.')
    if out.exists():raise ValueError('Output must be fresh.')
    out.mkdir(parents=True)
    tool_hash=digest(Path(__file__))
    rig=BodyRig();target=read(DESIGN/'r9-body-CP-and-contact-targets-v2.json')
    rig.set_deep_profile(target['deep_head_drop_source_px'])
    runtime=CoverageModel(SOURCE/'edited.model3.json')
    garment=read(HERE/'front-cloth/uv-fragments-r1.json')
    joints=read(DESIGN/'arm-reach-feasibility.json')['joint_material_receipts']
    handoff=read(OLD/'api/ground-handoff-r1/ground-handoff-proof.json')
    source_model=read(SOURCE/'edited.model3.json')
    pages=[(SOURCE/p).resolve() for p in source_model['FileReferences']['Textures']]
    plan={'schema_version':2,'kind':'R9_neutral_structural_prototype_pending_continuous_and_full_domain_review',
          'source':{'cmo_sha256':digest(SOURCE/'edited.cmo3'),'moc_sha256':digest(SOURCE/'edited.moc3'),
                    'atlas_pages':[{'path':str(p),'sha256':digest(p)} for p in pages]},
          'description':'Approved r2: lower body, short natural arms, upright closed legs with front cloth support. Isolated prototype, not adopted.',
          'deformer_grids':[target['body_CP_overrides']], 'mesh_grids':[],
          'append_pages':garment['append_pages'],'insert_meshes':[],
          'provenance':{'generator_sha256':tool_hash,'source_graph_sha256':digest(GRAPH),
                        'body_target_sha256':digest(DESIGN/'r9-body-CP-and-contact-targets-v2.json'),
                        'UV_fragments_sha256':digest(HERE/'front-cloth/uv-fragments-r1.json'),
                        'concept_r2_sha256':digest(ROOT/'art/live2d/review/sit-pose-20261005-r9/skirt-support-concept-r2-approved.png')},
          'coordinate_contract':'public parent-local normalized coordinates; exact actual source mesh base; source-pixel targets invert full edited ancestor lattice',
          'limits':['Ground/cloth prototype only at default BodyXYZ/Breath/Arm/Typing/Rock; all-domain gate pending.',
                    'No model adopted. Continuous skirt-contact/body/head/hair review pending.']}
    # Feet/stockings/knees/floor references: keep the actual frozen world result,
    # while the parent body lattice changes. Sit0 cells are copied by Java untouched.
    names=[n for n in rig.D if n.startswith('ArtMeshAuthorFloor') or
           n.startswith(('ArtMeshSitShoe','ArtMeshSitStocking','ArtMeshSitKnee'))]
    leg_keys=sorted(set([0.,.025,.05,.075,.1,.15,.2,.25,.3,.35,.4,.45,.5,.55,.6,.65,.7,.75,.8,.85,.9,.92,.95,1.]+
                        [k for n in names for k in axes_of(rig.D[n])[0]['keys']]))
    specs={}
    for name in names:
        node=rig.D[name];old=axes_of(node)[0]['keys']
        specs[name]={'id':name,'replace_axes':[remap_axis(old,leg_keys)],'keyforms':[]}
    maximum_forward=0.;minimum_jacobian=float('inf')
    for coordinate in itertools.product(range(3),repeat=4):
        base_p=rig.context(coordinate,0.)
        ankle_targets={};shaft_units={}
        for side,pivot in [('L',[555.,1134.]),('R',[698.,1134.])]:
            # Source-material ankle pivot carried by the actual source ancestor.
            local,_=rig.inverse('DeformPair_Footwear',[pivot],rig.context((1,1,1,0),0.))
            ankle=rig.points('DeformPair_Footwear',local,base_p)[0]
            next_point=rig.points('DeformPair_Footwear',local+[[0.,-.02]],base_p)[0]
            unit=(ankle-next_point)/np.linalg.norm(ankle-next_point)
            ankle_targets[side]=ankle;shaft_units[side]=unit
        for si,sit in enumerate(leg_keys):
            if sit==0:continue
            p=rig.context(coordinate,sit);runtime.update({'parameters':p})
            # Reuse computed parent lattices across all same-parent meshes.
            world=rig.worlds(p,True);inverse_cache={}
            for name in names:
                node=rig.D[name];parent=node['getParentDeformerId-lmpY1tE']
                desired=runtime.source_xy(runtime.positions[name])
                if args.crouch_shin_projection<1 and name.startswith(('ArtMeshSitStocking','ArtMeshSitKnee')):
                    side=name[-1];ankle=ankle_targets[side]
                    cosine=float(np.interp(sit,[0.,.35,.65,.8,.92,1.],[1.,1.,args.crouch_shin_projection,args.crouch_shin_projection,1.,1.]))
                    angles=([-12.,-8.,-4.,-1.,.5] if side=='L' else [12.,8.,4.,1.,-.5])
                    angle=np.deg2rad(np.interp(sit,[.35,.65,.8,.9,1.],angles))
                    unit=rotate([shaft_units[side]],angle)[0]
                    if 'Knee' in name:
                        # Rounded kneecap is a joint volume, not a flat shin sheet.
                        # Transport its center without squeezing its skin chart.
                        center=desired[0].copy()
                        desired=desired+(cosine-1)*np.dot(center-ankle,unit)*unit
                    else:
                        longitudinal=(desired-ankle)@unit
                        # Unprojected 3D coordinates retain source segment length;
                        # this is a new perspective pose, not the old 2D R8 gate.
                        desired=desired+(cosine-1)*longitudinal[:,None]*unit
                from full_context_geometry import generic_warp_inverse,warp_apply_jacobian
                lattice=world(parent)
                local,proof=generic_warp_inverse(rig.W[parent],lattice,desired)
                base=np.asarray(node['getMesh']['getPositions'],np.float32).reshape(-1,2)
                row=delta_row(base,local,[si,*coordinate]);specs[name]['keyforms'].append(row)
                actual=(base+np.asarray(row['position_deltas'],np.float32).reshape(-1,2)).astype(float)
                maximum_forward=max(maximum_forward,float(abs(warp_apply_jacobian(rig.W[parent],lattice,actual)[0]-desired).max()))
                minimum_jacobian=min(minimum_jacobian,proof['minimum_evaluated_jacobian'])
        print('LEG_CONTEXT '+str(coordinate),flush=True)
    plan['mesh_grids'].extend(specs.values())

    # Natural source joints/material points are sampled from current compiled Core.
    neutral={**rig.default,'ParamSitPose':0.,'ParamHandGround':1.,'ParamBusyLaptop':0.}
    runtime.update({'parameters':neutral})
    bones={side:{key:runtime.source_xy(runtime.anchor({k:a[k] for k in ['drawable','vertices','weights']}))
                 for key,a in joints[side].items()} for side in ['L','R']}
    rest={}
    for side,upper,forearm in [('L','ArtMeshHandwearL2','ArtMeshHandwearL'),('R','ArtMeshHandwearR5','ArtMeshHandwearR4')]:
        for kind,original in [('Upperarm',upper),('Forearm',forearm)]:
            name='ArtMeshGround'+kind+side
            rest[name]=chart_from_native_UV(runtime,name,original)
    bone_locals={side:{key:rig.inverse('DeformBodyZBreath',[point],neutral)[0][0]
                      for key,point in values.items()} for side,values in bones.items()}
    runtime.update({'parameters':{**neutral,'ParamSitPose':.65}})
    palms={side:runtime.source_xy(runtime.positions['ArtMeshGroundPalm'+side]).copy() for side in ['L','R']}
    palm_wrists={side:np.asarray(handoff['palm_wrist_anchors'][side]['weights'])@
                 palms[side][handoff['palm_wrist_anchors'][side]['vertices']] for side in ['L','R']}
    floor_joints={}
    for side in ['L','R']:
        proof=next(r for r in handoff['mesh_proofs'] if r['id']=='ArtMeshGroundForearm'+side)
        floor_joints[side]=next(r for r in proof['neutral_Arm0_body0_pose_keys'] if r['sit']==.65)
    arm_rows=[]
    for side in ['L','R']:
        ids=['ArtMeshGroundUpperarm'+side,'ArtMeshGroundForearm'+side,'ArtMeshGroundPalm'+side]
        edited={name:{'id':name,'keyforms':[]} for name in ids}
        source_axes=axes_of(rig.D[ids[0]]);sit_keys=source_axes[0]['keys']
        L1=np.linalg.norm(bones[side]['elbow']-bones[side]['shoulder'])
        L2=np.linalg.norm(bones[side]['wrist']-bones[side]['elbow'])
        for si,sit in enumerate(sit_keys):
            if sit==0:continue
            p={**neutral,'ParamSitPose':sit};world=rig.worlds(p,True)
            shoulder=rig.points('DeformBodyZBreath',[bone_locals[side]['shoulder']],p,True)[0]
            hover=rig.points('DeformBodyZBreath',[bone_locals[side]['wrist']],p,True)[0]
            ground=np.array([shoulder[0]+(-24 if side=='L' else 23),1147.2])
            # Fully straighten the elbow between old rest and planted branches.
            old_dir=hover-shoulder;old_angle=math.atan2(old_dir[1],old_dir[0])
            old_d=np.linalg.norm(bones[side]['wrist']-bones[side]['shoulder'])
            flex0=math.acos(np.clip((old_d**2-L1**2-L2**2)/(2*L1*L2),-1,1))
            gd=np.linalg.norm(ground-shoulder)
            if .65<=sit<=.8 and gd>L1+L2+1e-5:raise ValueError(('Unreachable planted arm',sit,side,gd,L1+L2))
            gflex=math.acos(np.clip((gd**2-L1**2-L2**2)/(2*L1*L2),-1,1))
            q=smooth((sit-.36)/(.65-.36))
            sign=1 if side=='L' else -1
            signed_flex=sign*((1-q)*flex0-q*gflex)
            distance=math.sqrt(L1**2+L2**2+2*L1*L2*math.cos(signed_flex))
            angle=old_angle+(math.atan2(ground[1]-shoulder[1],ground[0]-shoulder[0])-old_angle)*q
            direction=np.array([math.cos(angle),math.sin(angle)])
            wrist=shoulder+distance*direction
            # The return target comes from actual old busy hand, not a fixed chart.
            if sit>.8:
                runtime.update({'parameters':{**p,'ParamBusyLaptop':1}})
                a=handoff['busy_wrist_anchors'][side]
                busy=runtime.source_xy(runtime.anchor({k:a[k] for k in ['drawable','vertices','weights']}))
                t=smooth((sit-.8)/.12);wrist=(1-t)*wrist+t*busy
            vector=wrist-shoulder;d=np.linalg.norm(vector)
            if d>L1+L2+1e-3:raise ValueError(('Neutral reach exceeded',sit,side,d,L1+L2))
            unit=vector/max(d,1e-8);normal=np.array([-unit[1],unit[0]])
            along=(L1**2-L2**2+d*d)/(2*d);height=math.sqrt(max(0,L1**2-along*along))
            branch=(-sign if signed_flex>0 else sign)
            if sit>.8:branch=sign  # preserved shoulder/wrist; return elbow pending full handoff review.
            elbow=shoulder+along*unit+branch*height*normal
            # Palm remains the real complete generated open hand, rotating/placing
            # around its opaque wrist. Ground orientation is held while planted.
            palm_scale=.65+.35*q
            if sit>.8:palm_scale=1-.45*smooth((sit-.8)/.12)
            desired_palm=wrist+(palms[side]-palm_wrists[side])*palm_scale
            desired={ids[0]:rigid_segment(rest[ids[0]],bones[side]['shoulder'],bones[side]['elbow'],shoulder,elbow),
                     ids[1]:rigid_segment(rest[ids[1]],bones[side]['elbow'],bones[side]['wrist'],elbow,wrist),
                     ids[2]:desired_palm}
            for name in ids:
                node=rig.D[name];local,proof=rig.inverse(node['getParentDeformerId-lmpY1tE'],desired[name],p,True)
                base=np.asarray(node['getMesh']['getPositions'],np.float32).reshape(-1,2)
                for h in [0,1]:
                    c=[si,1,1,2,h]+([0,1] if side=='L' else [])
                    edited[name]['keyforms'].append(delta_row(base,local,c))
            arm_rows.append({'side':side,'sit':sit,'shoulder':shoulder.tolist(),'elbow':elbow.tolist(),'wrist':wrist.tolist(),
                             'actual_upper_length':float(np.linalg.norm(elbow-shoulder)),
                             'actual_forearm_length':float(np.linalg.norm(wrist-elbow)),
                             'rest_upper_length':float(L1),'rest_forearm_length':float(L2)})
        plan['mesh_grids'].extend(edited.values())

    # Old front is a mixed chart with genuine cutout; never stretch it as fill.
    front=rig.D['ArtMeshSitFrontSkirt']['getChannelGrids']['getGridsByChannel']['OPACITY']
    plan['mesh_grids'].append({'id':'ArtMeshSitFrontSkirt','channels':[{'channel':'OPACITY',
        'keyforms':[{'coordinate':c['getCoordinate'],'value':0.} for c in front['getCells']]}]})
    rear=rig.D['ArtMeshSitRearSkirt']['getChannelGrids']['getGridsByChannel']['OPACITY']
    plan['mesh_grids'].append({'id':'ArtMeshSitRearSkirt','channels':[{'channel':'OPACITY',
        'keyforms':[{'coordinate':c['getCoordinate'],'value':0.} for c in rear['getCells']]}]})

    # Front lap, frills, apron and bows share exact projected seam vertices.
    # Fold-under is a different occluded projection island attached to real waist.
    cloth_keys=leg_keys
    cloth_axes=[{'parameter':'ParamSitPose','keys':cloth_keys}]
    waist={row['parameters']['ParamSitPose']:row['waist_real_cloth']
           for row in target['key_targets'] if row['coordinate']==[1,1,1,0]}
    cloth_targets={}
    for sit in cloth_keys:
        width=float(np.interp(sit,[0,.35,.65,1],[.50,.55,.55,.55]))
        drape=float(np.interp(sit,[0,.35,.65,1],[10,80,100,100]))
        deep=[908,914,950,958,970] if args.crouch_shin_projection==1 else [978,984,1023,1038,1060]
        profiles=np.array([[735,760,850,925,995],[780,800,872,936,978],
                           deep,deep,
                           [840,858,916,948,980],[840,858,916,948,980]],float)
        ys=np.array([np.interp(sit,[0,.35,.65,.8,.92,1],profiles[:,i]) for i in range(5)])
        cloth_targets[sit]=(ys,width,drape)
    for spec in garment['insert_mesh_specs']:
        points=np.asarray(spec['source_pixel_positions']).reshape(-1,2)
        base,proof=rig.inverse(spec['parent_id'],points,neutral)
        base=np.asarray(base,np.float32)
        row={k:spec[k] for k in ['id','name','template_id','parent_id','part_id','texture_page','uvs','indices','draw_order']}
        row.update({'opacity':0.,'positions':base.reshape(-1).tolist(),'axes':cloth_axes,'keyforms':[],
                    'channels':[{'channel':'OPACITY','initial_value':0.,'append_axes':cloth_axes,
                                 'keyforms':[{'coordinate':[i],'value':smooth(s/.0875)} for i,s in enumerate(cloth_keys)]}]})
        for si,sit in enumerate(cloth_keys):
            ys,width,drape=cloth_targets[sit];x,y=points.T
            world=np.c_[627+(x-627)*width,
                        np.interp(y,[340,400,610,760,960],ys)+drape*(abs(x-627)/627)**1.5*np.clip((y-360)/600,0,1)]
            if spec['id']=='ArtMeshR9WaistFoldUnder':
                true_waist=float(np.interp(sit,[0,.35,.65,1],[waist[k]['C'][1] for k in [0.,.35,.65,1.]]))
                world[:,1]=true_waist+(y-360)*.45
            p={**neutral,'ParamSitPose':sit};local,_=rig.inverse(spec['parent_id'],world,p,True)
            row['keyforms'].append(delta_row(base,local,[si]))
        plan['insert_meshes'].append(row)
    if digest(Path(__file__))!=tool_hash:raise ValueError('Generator changed during run.')
    path=out/'author-plan.json';path.write_text(json.dumps(plan,separators=(',',':'),allow_nan=False)+'\n',encoding='utf8')
    proof={'kind':'R9_neutral_prototype_construction_receipt','adopted':False,'plan_sha256':digest(path),
           'source_cmo_sha256':plan['source']['cmo_sha256'],'leg_contexts':81,'leg_keys':leg_keys,
           'leg_target_source':'compiled actual 8-page standing-restored source Core',
           'leg_f32_forward_max_source_px':maximum_forward,'minimum_inverse_parent_jacobian':minimum_jacobian,
           'neutral_arm_targets':arm_rows,'cloth_domain':'default body/arm context; all-domain binding pending',
           'fold_under_projection_island':'real waist attachment, separated from front crest; hidden overlap to review',
           'diagnostic_shin_projection_cosine':args.crouch_shin_projection,
           'shin_projection_scope':'Orthographic backward hinge about fixed ankle; material art/perspective and actual 3D joint contract NOT yet approved. R8 constant 2D leg length gate does not apply to this diagnostic.',
           'missing_proofs':['actual exported head rigidity','continuous leg lengths/cloth contact','full ArmXYZ/Typing/Rock and Body domain','Native standing regression','final runtime user adoption']}
    (out/'construction-receipt.json').write_text(json.dumps(proof,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':str(path),'sha256':digest(path),'new_meshes':12,'mesh_edits':len(plan['mesh_grids']),
                      'leg_inverse_f32_max_px':maximum_forward,'status':'prototype_pending_Native_visual'},indent=2),flush=True)


if __name__=='__main__':main()
