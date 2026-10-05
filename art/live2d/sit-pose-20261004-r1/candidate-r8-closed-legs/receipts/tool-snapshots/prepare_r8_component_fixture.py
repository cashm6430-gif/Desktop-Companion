"""Frozen repo tools plus explicit R8 receipt: no probe or threshold relaxation."""
from pathlib import Path
import argparse
import copy
import hashlib
import itertools
import json
import sys
import numpy as np
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from prepare_component_coverage_fixture import build
from verify_component_coverage import CoverageModel
from probe_cubism_core import digest

G=ROOT/'.local/authoring/sit-pose-20261004-r1'


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def front_mask(model,output):
    """Conservative visible cloth envelope from actual original source preview.

    Excludes the copied blue side hair. Source polygon is recorded as evidence;
    per-triangle native UV mapping preserves actual chart correspondence.
    """
    polygon=[[505,725],[732,725],[755,785],[807,851],[837,887],[829,911],
             [795,939],[750,955],[500,955],[433,939],[400,920],[383,893],
             [418,858],[468,810]]
    runtime=CoverageModel(model);runtime.update({'parameters':{'ParamSitPose':0}})
    name='ArtMeshBottomwear';top=runtime.topology[name]
    source=runtime.source_xy(runtime.positions[name]);uv=top['uvs']
    height,width=runtime.pages[top['texture']].shape
    atlas=np.column_stack((uv[:,0]*width-.5,(1-uv[:,1])*height-.5))
    source_mask=Image.new('L',(1254,1254),0);ImageDraw.Draw(source_mask).polygon([tuple(p) for p in polygon],fill=255)
    src=np.asarray(source_mask);mask=np.zeros((height,width),np.uint8)
    for tri in top['indices']:
        a,b,c=atlas[tri];e=b-a;f=c-a;det=e[0]*f[1]-e[1]*f[0]
        if abs(det)<1e-10:continue
        lo=np.maximum(0,np.floor(atlas[tri].min(0)).astype(int));hi=np.minimum([width-1,height-1],np.ceil(atlas[tri].max(0)).astype(int))
        xx,yy=np.meshgrid(np.arange(lo[0],hi[0]+1),np.arange(lo[1],hi[1]+1));p=np.column_stack((xx.ravel(),yy.ravel()));d=p-a
        w1=(d[:,0]*f[1]-d[:,1]*f[0])/det;w2=(e[0]*d[:,1]-e[1]*d[:,0])/det;w0=1-w1-w2
        inside=(w0>=-1e-7)&(w1>=-1e-7)&(w2>=-1e-7)
        if not np.any(inside):continue
        xy=np.rint(w0[inside,None]*source[tri[0]]+w1[inside,None]*source[tri[1]]+w2[inside,None]*source[tri[2]]).astype(int)
        xy=np.clip(xy,[0,0],[1253,1253]);pixel=p[inside].astype(int);mask[pixel[:,1],pixel[:,0]]=np.maximum(mask[pixel[:,1],pixel[:,0]],src[xy[:,1],xy[:,0]])
    output.parent.mkdir(parents=True,exist_ok=True);Image.fromarray(mask).save(output)
    return {'source':'art/live2d/layers/17-bottomwear.png','source_sha256':digest(ROOT/'art/live2d/layers/17-bottomwear.png'),
            'source_clothing_polygon':polygon,'mask_sha256':digest(output),'atlas_page':top['texture'],
            'mapping':'Actual standing Core Bottomwear source XY and per-triangle UV barycentric raster; clone UV/topology must match.',
            'source_model':str(model),'source_model_sha256':digest(model),'source_MOC_pins':runtime.pins,
            'excluded':'Blue side hair outside conservative navy/apron/frill cloth envelope.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for arg in ['plan','bridge-proof','model','output']:parser.add_argument('--'+arg,type=Path,required=True)
    parser.add_argument('--additional-proof',type=Path,action='append',default=[])
    parser.add_argument('--edited-moc',type=Path)
    parser.add_argument('--incremental-plan',type=Path)
    args=parser.parse_args();output=args.output.resolve()
    if output.exists():raise ValueError('Fresh fixture required.')
    if bool(args.edited_moc)!=bool(args.incremental_plan):raise ValueError('Final MOC and incremental author plan must be supplied together.')
    paths=[args.plan,args.bridge_proof,*args.additional_proof]
    if args.edited_moc:paths += [args.edited_moc,args.incremental_plan]
    plan=read(args.plan);bridge=read(args.bridge_proof)
    if bridge['kind']!='actual_missing_busy_left_sleeve_author_repair':raise ValueError('Wrong Busy sleeve repair receipt.')
    source_hashes={str(p.resolve()):digest(p) for p in paths}
    inserts={x['id']:x for x in plan['insert_meshes']}
    bridge_ids=['ArtMeshBusyUpperarmBridgeL','ArtMeshBusyForearmBridgeL']
    if any(n not in inserts for n in bridge_ids):raise ValueError('Both real sleeve bridge materials required.')
    fixture=build(args.plan,ROOT/'.local/geometry/component-coverage-fixture-r4.json',
                  G/'geometry/folded-legs-proof-r4.json',G/'api/ground-arms-geometry-r4/ground-arms-proof.json',output,
                  G/'api/ground-handoff-r1/ground-handoff-proof.json')
    mask=output.with_name(output.stem+'-front-cloth-mask.png')
    if mask.exists():raise ValueError('Fresh mask required.')
    material_evidence=front_mask(args.model.resolve(),mask)
    fixture['drawable_material_masks']={'ArtMeshSitFrontSkirt':mask.name}
    for name in bridge_ids:
        record=inserts[name]
        fixture['expected_topology'][name]={'vertices':len(record['positions'])//2,'triangles':len(record['indices'])//3,
            'triangle_membership_sha256':hashlib.sha256(np.sort(np.asarray(record['indices']).reshape(-1,3),axis=1).astype('<u4').tobytes()).hexdigest()}
    prototype=next(p for p in fixture['poses'] if p['name']=='H1-authored-sit0.65-H1-arm0-busy0-body0,0,0-breath0-typing0-rock0-PartBody1')
    for sit in [.725,.85,.9,1.]:
        pose=copy.deepcopy(prototype);pose['name']=f'R8-closed-leg-sit{sit:g}-H1'
        pose['parameters'].update({'ParamSitPose':sit,'ParamBusyLaptop':int(sit>=.93)})
        pose['part_opacities']['PartBody']=int(sit<.93);fixture['poses'].append(pose)
    for sit in [.94,.96]:
        for typing,rock in itertools.product([0.,1.],[-1.,1.]):
            pose=copy.deepcopy(prototype);pose['name']=f'R8-runtime-return-sit{sit:g}-typing{typing:g}-rock{rock:g}'
            pose['parameters'].update({'ParamSitPose':sit,'ParamBusyLaptop':1,'ParamHandGround':1. if sit==.94 else 0.,
                                       'ParamBusyTypingL':typing,'ParamBusyTypingR':typing,'ParamLaptopRock':rock})
            pose['part_opacities']['PartBody']=0
            # Keep the original runtime incoming/return semantic union.
            existing=next(p for p in fixture['poses'] if p['name'].startswith('runtime-activation-sit0.96-'))
            pose['checks']=copy.deepcopy(existing['checks']);fixture['poses'].append(pose)
    for pose in fixture['poses']:
        runtime=pose['name'].startswith(('runtime','R8-runtime'))
        for check in pose['checks']:
            if not runtime:continue
            additions={'cuff_palm_material_bridge_L':['ArtMeshBusyForearmBridgeL'],
                       'forearm_before_wrist_L':['ArtMeshBusyForearmBridgeL'],
                       'upperarm_forearm_elbow_seam_L':bridge_ids,
                       'upperarm_shoulder_cloth_seam_L':['ArtMeshBusyUpperarmBridgeL']}.get(check['name'],[])
            check['required_drawable_group']=list(dict.fromkeys(check['required_drawable_group']+additions))
        for side in ['L','R']:
            pose['checks'].append({'name':'R8_knee_upper_cloth_join_'+side,
                'anchor':{'drawable':'ArtMeshSitKneeFront'+side,'vertices':[25],'weights':[1.]},
                'roi_offsets':{'x':[-2,2],'y':[-4,4],'step':2},
                'required_drawable_group':['ArtMeshSitKneeFront'+side,'ArtMeshSitFrontSkirt','ArtMeshSitRearSkirt'],
                'minimum_alpha':.5,'minimum_fraction':1.,
                'ownership_note':'Front skirt coverage uses a separate conservative garment-only UV mask; copied blue hair cannot fill this joint.'})
    fixture['inputs'].update(source_hashes);fixture['inputs'][str(mask)]=digest(mask)
    fixture['R8_receipt']={'closed_legs_contract':str(G/'geometry/r8-closed-leg-contact-contract.json'),
        'contract_sha256':digest(G/'geometry/r8-closed-leg-contact-contract.json'),
        'bridge_receipt':str(args.bridge_proof.resolve()),'bridge_receipt_sha256':digest(args.bridge_proof),
        'front_cloth_semantic_material':material_evidence,
        'original_handoff_ROIs_and_thresholds_preserved':True,
        'busy_bridge_cloth_never_counted_as_palm_after_wrist_skin':True,
        'new_typing_rock_return_cases':[.94,.95,.96],
        'new_knee_upper_clothing_join_checks':True,
        'compiled_MOC_binding':'Review wrapper binds actual candidate MOC to closed-loop edit and dense reports; fixture pins final merged plan and actual topology.'}
    if args.edited_moc:
        model_doc=read(args.model)
        model_moc=(args.model.parent/model_doc['FileReferences']['Moc']).resolve()
        if digest(model_moc)!=digest(args.edited_moc):raise ValueError('Staged model MOC is different from exact final edited MOC.')
        fixture['author_motion_binding']={'kind':'candidate-moc-pinned','moc_path':str(args.edited_moc.resolve()),
            'moc_sha256':digest(args.edited_moc),
            'effective_plan':{'path':str(args.plan.resolve()),'sha256':digest(args.plan)},
            'incremental_plan':{'path':str(args.incremental_plan.resolve()),'sha256':digest(args.incremental_plan)},
            'provenance':'Rebuilt from R8 closed-leg and actual wrist-following warm return receipt; existing probe coordinates and thresholds retained.'}
    else:
        fixture['author_motion_binding']={'kind':'provisional-plan-only','review_pass_eligible':False}
    for p in [G/'geometry/r8-closed-leg-contact-contract.json',G/'geometry/full-ancestor-closed-legs-r8-proof.json',Path(__file__)]:
        fixture['inputs'][str(p.resolve())]=digest(p)
    fixture['limits'].append('R8 original Sit0 protected; nonzero standing shoe X now follows authored closed-leg trajectory. Dense contact must use R8 contract, never old fixed-X expectation.')
    if any(digest(p)!=sha for p,sha in source_hashes.items()):raise ValueError('Final plan/receipt changed during fixture build.')
    output.write_text(json.dumps(fixture,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'fixture':str(output),'sha256':digest(output),'poses':len(fixture['poses']),
                      'checks':sum(len(p['checks']) for p in fixture['poses']),'components':len(fixture['expected_topology']),
                      'mask':str(mask),'repo_tools_modified':False},indent=2))


if __name__=='__main__':main()
