"""Continue the real seven-page author CMO with exact left busy handoff.

Edits geometry only on the three existing left Ground charts and two left
Busy material bridges.  The warm-hand forward anchor is actual original
paint, not a moved fixture ROI. Existing opacity/parts/UV/pages/leg grids stay
unchanged. This is an isolated JSON generator; it does not call an editor.
"""
from pathlib import Path
import argparse
import copy
import itertools
import json
import sys

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import generate_ground_handoff as h
import generate_busy_left_bridge_r2 as bridge

s,m,arm,ROOT = h.s,h.m,h.arm,h.ROOT
EXTRA_AXES = [{'parameter':'ParamBusyTypingL','keys':[0.,1.]},
              {'parameter':'ParamLaptopRock','keys':[-1.,0.,1.]}]
TARGET_IDS = ['ArtMeshGroundUpperarmL','ArtMeshGroundForearmL','ArtMeshGroundPalmL',
              'ArtMeshBusyUpperarmBridgeL','ArtMeshBusyForearmBridgeL']


def exact_busy_pose(setup,side,params):
    if side != 'L':
        raise ValueError('This narrow revision owns the left return only')
    shoulder=h.anchor_world(setup['hover']['shoulder'],dict(params,ParamArmLA=0.))
    p=dict(params,ParamBusyLaptop=1.)
    wrist=h.anchor_world(setup['busy_wrist'],p)
    forward=h.anchor_world(setup['busy_hand_forward'],p)
    direction=forward-wrist
    if np.linalg.norm(direction)<1.:
        raise ValueError('Busy warm-hand direction is degenerate')
    # A true sleeve ends on the opposite side of the same painted hand.
    elbow=wrist-100.*direction/np.linalg.norm(direction)
    return shoulder,elbow,wrist


def forms_for(node,setup,kind,old_bridge=False):
    base=s.f32(node['positions']).reshape(-1,2)
    tris=np.asarray(node['indices']).reshape(-1,3)
    old_cells={tuple(f['coordinate']):s.f32(f['position_deltas']) for f in node['keyforms']}
    armkeys=[0.] if old_bridge else node['axes'][3]['keys']
    axes=bridge.AXES if old_bridge else node['axes']+EXTRA_AXES
    if kind != 'palm':
        points,uv,indices,_=arm.material('L',kind)
        assert np.array_equal(s.f32(uv).reshape(-1),s.f32(node['uvs']))
        setting=arm.SEGMENTS['L']
        source_start=np.asarray(setting['shoulder'] if kind=='upper' else setting['forearm_elbow'])
        source_end=np.asarray(setting['upper_elbow'] if kind=='upper' else setting['cuff_axis'])
        if kind=='forearm':
            source_end=source_start+setting['forearm_cut_distance']*(source_end-source_start)/np.linalg.norm(source_end-source_start)
    cells=[];proofs=[];scales=[];keyrecords=[];maximum_error=0.
    for bx,by,ai,ti,ri in itertools.product(range(3),range(3),range(len(armkeys)),range(2),range(3)):
        localforms=[]
        for si,sit in enumerate(h.SIT_KEYS):
            p=dict(m.context(bx,by,sit),ParamArmLA=armkeys[ai],ParamBusyTypingL=float(ti),ParamLaptopRock=float(ri-1))
            pose,scale,stage=(exact_busy_pose(setup,'L',p),1.,'busy-source') if old_bridge else h.trajectory(setup,'L',p)
            shoulder,elbow,wrist=pose
            if sit==0.:
                coord=(si,bx,by,ti,ri) if old_bridge else (si,bx,by,ai,1)
                delta=old_cells[coord]
                effective=(base.reshape(-1)+delta).astype(np.float32).reshape(-1,2)
                desired=h.body_world(h.BODY,p,effective)
            else:
                if kind=='palm':
                    desired=h.palm_world(setup,'L',p,pose,scale)
                else:
                    start,end=(shoulder,elbow) if kind=='upper' else (elbow,wrist)
                    if kind=='forearm':
                        end=wrist+arm.CUFF_OVERLAP_SOURCE_PX*(wrist-elbow)/np.linalg.norm(wrist-elbow)
                    desired,scale=arm.segment_map(points,source_start,source_end,start,end)
                local=m.local_from_world(h.BODY,p,desired,True)
                delta=s.f32(local-base).reshape(-1)
                effective=(base.reshape(-1)+delta).astype(np.float32).reshape(-1,2)
            localforms.append(effective)
            maximum_error=max(maximum_error,float(abs(h.body_world(h.BODY,p,effective)-desired).max()))
            if old_bridge:
                cells.append({'coordinate':[si,bx,by,ti,ri],'position_deltas':delta.tolist()})
            else:
                for ground in (0,1):
                    cells.append({'coordinate':[si,bx,by,ai,ground,ti,ri],'position_deltas':delta.tolist()})
            scales.append(float(scale))
            if bx==by==1 and armkeys[ai]==0. and si>=18:
                keyrecords.append({'sit':sit,'typing':ti,'rock':ri-1,'shoulder':shoulder.tolist(),
                                   'elbow':elbow.tolist(),'wrist':wrist.tolist(),'stage':stage})
        proof=h.interval_proof(localforms,tris,keys=h.SIT_KEYS)
        if not proof['all_triangles_strictly_same_sign']:
            raise ValueError((node['id'],bx,by,ai,ti,ri,proof['failures']))
        proofs.append(proof)
    row={'id':node['id'],'keyforms':cells}
    if not old_bridge:
        row['append_axes']=EXTRA_AXES
    return row,{'id':node['id'],'kind':kind,'vertices':len(base),'triangles':len(tris),
                'geometry_axes':axes,'forms':len(cells),'protected_Sit0_delta_bits_copied':True,
                'opacity_UV_page_part_and_base_unchanged':True,
                'maximum_f32_forward_error_source_px':maximum_error,
                'minimum_local_interval_area_ratio':float(min(p['minimum_area_ratio'] for p in proofs)),
                'longitudinal_stretch_or_palm_scale_range':[min(scales),max(scales)],
                'neutral_body_Arm0_return_keys':keyrecords}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-export',type=Path,required=True)
    p.add_argument('--reference-plan',type=Path,required=True)
    p.add_argument('--bridge-fragment',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    args=p.parse_args()
    export=args.source_export.resolve(strict=True);inp=args.reference_plan.resolve(strict=True)
    fragment=args.bridge_fragment.resolve(strict=True);out=args.output_dir.resolve()
    if not out.is_relative_to(ROOT/'.local') or out.exists():
        raise ValueError('Fresh isolated .local output required')
    model=json.loads((export/'edited.model3.json').read_text(encoding='utf8'))
    pages=[export/path for path in model['FileReferences']['Textures']]
    if len(pages)!=7:
        raise ValueError('Actual seven-page R8 author source required')
    pinned=[inp,fragment,export/'edited.cmo3',export/'edited.moc3']+pages
    pins={str(path):h.sha(path) for path in pinned}
    source={'cmo_sha256':h.sha(export/'edited.cmo3'),'moc_sha256':h.sha(export/'edited.moc3'),
            'atlas_pages':[{'path':str(path),'sha256':h.sha(path)} for path in pages]}
    reference=json.loads(inp.read_text(encoding='utf8'))
    bridges=json.loads(fragment.read_text(encoding='utf8'))
    setup=h.trajectory_setup(reference,'L')
    setup['busy_hand_forward']=h.material_anchor(s.D[h.BUSY['L']],[379.,782.])
    png=ROOT/'art/live2d/layers/busy-sleeve-l.png'
    pixel=Image.open(png).convert('RGBA').getpixel((379,782))
    if not (pixel[3]>=224 and pixel[0]>pixel[1]+12 and pixel[0]>pixel[2]+15):
        raise ValueError('Forward direction must be actual original warm hand paint')
    setup['busy_hand_forward'].update({'source_png':str(png),'source_png_sha256':h.sha(png),
                                       'source_pixel_rgba':pixel,'meaning':'Opaque warm hand distal anchor, 18 source pixels beyond original wrist.'})
    h.busy_pose=exact_busy_pose
    original={n['id']:n for n in reference['insert_meshes']}
    original.update({n['id']:n for n in bridges['insert_meshes']})
    built=[]
    for identifier in TARGET_IDS:
        kind='palm' if 'Palm' in identifier else ('upper' if 'Upperarm' in identifier else 'forearm')
        built.append(forms_for(original[identifier],setup,kind,'Bridge' in identifier))
    rows,reports=[x[0] for x in built],[x[1] for x in built]
    if any(h.sha(Path(path))!=sha for path,sha in pins.items()):
        raise ValueError('Input changed during generation')
    out.mkdir(parents=True)
    target=out/'ground-left-return-geometry-plan.json'
    plan={'schema_version':2,'source':source,'mesh_grids':rows,
          'description':'Geometry-only exact current BusyTypingL/LaptopRock and original warm-hand direction for left return; no inserts/pages/opacity edits.'}
    target.write_text(json.dumps(plan,indent=2,allow_nan=False)+'\n',encoding='utf8')
    report={'schema_version':1,'kind':'actual_original_busy_warm_hand_return_geometry','adopted':False,
            'source_pins':pins,'generator_sha256':h.sha(__file__),'plan_sha256':h.sha(target),
            'busy_wrist_anchor':setup['busy_wrist'],'busy_hand_forward_anchor':setup['busy_hand_forward'],
            'ground_wrist_real_TypingL_Rock_driver':True,
            'source_domain':'Existing parent-local DeformBodyZBreath; same original source drawing UVs and ordered seven PNG pages.',
            'unchanged':['all legs and parents','all opacity channels','all existing parts','all mesh UV/index/base arrays','all original PNG bytes','right hand trajectory','protected Sit0 forms'],
            'mesh_proofs':reports,
            'pending':['Unmodified public CMO/readback/Native closure','Actual Core all named palm/sleeve coverage groups unchanged thresholds','Native grey-matte proportion/transition review']}
    (out/'ground-left-return-proof.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':str(target),'sha256':h.sha(target),'proof':str(out/'ground-left-return-proof.json'),
                      'meshes':[{'id':r['id'],'forms':r['forms'],'minimum_area_ratio':r['minimum_local_interval_area_ratio']} for r in reports]},indent=2))


if __name__=='__main__':main()
