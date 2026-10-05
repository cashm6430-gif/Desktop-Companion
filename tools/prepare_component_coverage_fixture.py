"""Build the approved Sit joint fixture from pinned leg/arm anchor evidence.

The result is model-path independent. Re-run it against each final staged
model with verify_component_coverage.py. Changed mesh vertex/triangle counts
are rejected instead of silently reusing stale barycentric anchors.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path

import numpy as np

from probe_cubism_core import digest


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def anchor(drawable,record):return {'drawable':drawable,'vertices':record['vertices'],'weights':record['weights']}


def build(plan_path,seed_path,leg_proof_path,arm_proof_path,output_path,handoff_proof_path=None):
    paths=[Path(p).resolve() for p in [plan_path,seed_path,leg_proof_path,arm_proof_path]]
    plan,seed,legs,arms=map(read,paths)
    handoff=None
    if handoff_proof_path:
        paths.append(Path(handoff_proof_path).resolve());handoff=read(paths[-1])
        if handoff.get('kind')!='continuous_ground_handoff_author_geometry':raise ValueError('Expected the explicit final hover/ground/return handoff proof.')
    inserts={row['id']:row for row in plan['insert_meshes']}
    leg_records={row['id']:row for row in legs['children']}
    arm_records={row['id']:row for row in arms['segments']}
    required=[f'ArtMeshSit{material}{side}' for side in ['L','R'] for material in ['KneeFront','Stocking','Shoe']]
    required+=['ArtMeshSitFrontSkirt','ArtMeshSitRearSkirt']
    required+=[f'ArtMeshGround{material}{side}' for side in ['L','R'] for material in ['Palm','Upperarm','Forearm']]
    missing=set(required)-set(inserts)
    if missing:raise ValueError('Plan needs the final sleeve/palm and continuous-leg components: '+str(sorted(missing)))
    front_check=copy.deepcopy(next(c for c in seed['poses'][0]['checks'] if c['name']=='skirt_cutout_has_cloth_backing'))
    specs=[(.35,0,0,0,0,0,1),(.45,0,0,0,0,0,1),(.575,0,0,0,0,0,1),(.65,0,0,0,0,0,1),(.8,0,0,0,0,0,1),(1,1,0,0,0,0,0),(.65,1,0,0,0,0,0)]
    specs += [(.65,0,bx,by,0,0,1) for bx,by in itertools.product([-10,10],repeat=2)]
    specs += [(.65,0,bx,by,0,.5,1) for bx,by in itertools.product([-5,5],repeat=2)]
    specs += [(.65,0,bx,by,bz,breath,1) for bx,by in [(-10,10),(10,-10)] for bz,breath in itertools.product([-10,10],[0,.5,1])]
    specs += [(sit,0,-10,10,10,1,1) for sit in [.45,.575,.8,1]]
    specs += [(1,1,10,-10,-10,1,0)]
    cases=[{'sit':sit,'busy':busy,'bx':bx,'by':by,'bz':bz,'breath':breath,'part':part,'h':1.,'arm':0.,'profile':'H1-authored','typing':0.,'rock':0.}
           for sit,busy,bx,by,bz,breath,part in dict.fromkeys(specs)]
    if handoff:
        def phase_case(sit,h=1.,arm=0.,bx=0.,by=0.,bz=0.,breath=0.,profile='H1-authored',typing=0.,rock=0.):
            return {'sit':sit,'busy':int(sit>=.93),'bx':bx,'by':by,'bz':bz,'breath':breath,
                    'part':int(sit<.93),'h':h,'arm':arm,'profile':profile,'typing':typing,'rock':rock}
        phases=[.32,.34,.35,.36,.45,.5,.65,.8,.84,.88,.92,.93,.94,.95,.96]
        cases += [phase_case(sit) for sit in phases]
        cases += [phase_case(sit,arm=arm) for sit in [.35,.65,.94] for arm in [-65.,-35.,0.,35.,65.]]
        cases += [phase_case(sit,bx=bx,by=by,bz=bz,breath=1.) for sit in [.35,.94]
                  for bx,by,bz in itertools.product([-10.,10.],repeat=3)]
        smooth=lambda x:max(0.,min(1.,x))**2*(3-2*max(0.,min(1.,x)))
        envelope=handoff['runtime_activation_envelope'];incoming=envelope['incoming'];outgoing=envelope['outgoing']
        activation=lambda sit:smooth((sit-incoming[0])/(incoming[1]-incoming[0]))*(1-smooth((sit-outgoing[0])/(outgoing[1]-outgoing[0])))
        cases += [phase_case(sit,h=activation(sit),profile='runtime-activation') for sit in phases]
        cases += [phase_case(.95,h=activation(.95),profile='runtime-typing-rock',typing=typing,rock=rock)
                  for typing,rock in itertools.product([0.,1.],[-1.,1.])]
    unique={tuple(case.values()):case for case in cases};poses=[]
    for case in unique.values():
        sit,busy,bx,by,bz,breath,part=[case[field] for field in ['sit','busy','bx','by','bz','breath','part']]
        checks=[copy.deepcopy(front_check)]
        for side in ['L','R']:
            knee='ArtMeshSitKneeFront'+side;sock='ArtMeshSitStocking'+side;shoe='ArtMeshSitShoe'+side
            prox,distal=leg_records[sock]['length_anchors']
            a=anchor(sock,prox);b=anchor(sock,distal)
            checks.append({'name':'knee_material_'+side,'anchor':{'drawable':knee,'vertices':[0],'weights':[1]},
                           'required_drawable_group':[knee],'roi_offsets':{'x':[-4,4],'y':[-4,4],'step':2},
                           'minimum_alpha':.8,'minimum_fraction':1})
            checks.append({'name':'knee_sock_seam_'+side,'anchor':a,'offset_frame':{'toward_anchor':b},
                           'roi_offsets':{'x':[-8,8],'y':[-10,-2],'step':2},
                           'required_drawable_group':[knee,sock],'minimum_alpha':.5,'minimum_fraction':.95})
            checks.append({'name':'ankle_shoe_seam_'+side,'anchor':b,'offset_frame':{'toward_anchor':a},
                           'roi_offsets':{'x':[-8,8],'y':[-14,-6],'step':2},
                           'required_drawable_group':[sock,shoe],'minimum_alpha':.5,'minimum_fraction':.95})
            upper='ArtMeshGroundUpperarm'+side;forearm='ArtMeshGroundForearm'+side;palm='ArtMeshGroundPalm'+side
            upper_record=arm_records[upper];record=arm_records[forearm]
            wrist=anchor(palm,record['palm_wrist_anchor'])
            forearm_prox=anchor(forearm,record['material_anchors']['proximal'])
            upper_prox=anchor(upper,upper_record['material_anchors']['proximal'])
            upper_distal=anchor(upper,upper_record['material_anchors']['distal'])
            # At Sit .35 the author explicitly crossfades the sleeve at .5
            # opacity. Detect absent material with a phase-aware threshold;
            # fully deployed Sit>=.45 always requires alpha>=.5.
            threshold=.5 if handoff else (.2 if sit<.45 else .5)
            runtime=case['profile'].startswith('runtime')
            old_upper=['ArtMeshHandwearL2'] if side=='L' else ['ArtMeshHandwearR5']
            old_lower=['ArtMeshHandwearL','ArtMeshObjects3'] if side=='L' else ['ArtMeshHandwearR4','ArtMeshHandwearR','ArtMeshHandwearR3','ArtMeshObjects2']
            suffix_lower=old_lower if runtime else []
            suffix_upper=old_upper if runtime else []
            checks.append({'name':'cuff_palm_material_bridge_'+side,'anchor':wrist,'offset_frame':{'toward_anchor':forearm_prox},
                           'offsets_source_pixels':[[x,y] for x in [-2,0,2] for y in [-6,-3,0,3,6,10]],
                           'required_drawable_group':[forearm,palm]+suffix_lower,'minimum_alpha':threshold,'minimum_fraction':1,
                           'ownership_note':'Only actual matching forearm and palm alpha may cover the wrist seam.'})
            checks.append({'name':'forearm_before_wrist_'+side,'anchor':wrist,'offset_frame':{'toward_anchor':forearm_prox},
                           'offsets_source_pixels':[[x,y] for x in [-2,0,2] for y in [12,16]],
                           'required_drawable_group':[forearm]+suffix_lower,'minimum_alpha':threshold,'minimum_fraction':1})
            checks.append({'name':'palm_after_wrist_'+side,'anchor':wrist,'offset_frame':{'toward_anchor':forearm_prox},
                           'offsets_source_pixels':[[0,-8],[0,-4],[0,0]],
                           'required_drawable_group':[palm]+suffix_lower,'minimum_alpha':threshold,'minimum_fraction':1})
            checks.append({'name':'upperarm_forearm_elbow_seam_'+side,'anchor':upper_distal,'offset_frame':{'toward_anchor':upper_prox},
                           'roi_offsets':{'x':[-2,2],'y':[-4,4],'step':2},
                           'required_drawable_group':[upper,forearm]+suffix_upper+suffix_lower,'minimum_alpha':threshold,'minimum_fraction':1})
            checks.append({'name':'upperarm_shoulder_cloth_seam_'+side,'anchor':upper_prox,'offset_frame':{'toward_anchor':upper_distal},
                           'roi_offsets':{'x':[-2,2],'y':[-3,3],'step':2},
                           'required_drawable_group':[upper,'ArtMeshTopwear','ArtMeshObjects8']+suffix_upper,
                           'minimum_alpha':threshold,'minimum_fraction':1})
        poses.append({'name':f"{case['profile']}-sit{sit:g}-H{case['h']:.5g}-arm{case['arm']:g}-busy{busy}-body{bx:g},{by:g},{bz:g}-breath{breath:g}-typing{case['typing']:g}-rock{case['rock']:g}-PartBody{part}",
                      'parameters':{'ParamSitPose':sit,'ParamBusyLaptop':busy,'ParamBodyAngleX':bx,'ParamBodyAngleY':by,
                                    'ParamBodyAngleZ':bz,'ParamBreath':breath,'ParamSkirtSpread':1,'ParamHandGround':case['h'],
                                    'ParamArmLA':case['arm'],'ParamArmRA':case['arm'],
                                    'ParamBusyTypingL':case['typing'],'ParamBusyTypingR':case['typing'],'ParamLaptopRock':case['rock']},
                      'part_opacities':{'PartBody':part},'checks':checks})
    fixture={'schema_version':1,'kind':'authored_sit_final_candidate_named_material_joint_fixture',
             'inputs':{str(path):digest(path) for path in paths},
             'generator_sha256':digest(Path(__file__)),
             'expected_topology':{name:{'vertices':len(inserts[name]['positions'])//2,'triangles':len(inserts[name]['indices'])//3,
                                       'triangle_membership_sha256':hashlib.sha256(np.sort(np.asarray(inserts[name]['indices']).reshape(-1,3),axis=1).astype('<u4').tobytes()).hexdigest()} for name in required},
             'poses':poses,'group_policy':{'skirt_opening':['ArtMeshSitRearSkirt'],
                                          'limb_groups':'Same-side knee/sock/shoe and same-side cloth sleeve/palm only.',
                                          'excluded_as_coverage':['ArtMeshBackHair','ArtMeshBackHair2','ArtMeshFrontHair','ArtMeshHandwearR2','ArtMeshObjects','ArtMeshObjects4','ArtMeshTail'],
                                          'mixed_front_skirt':'Front skirt includes blue hair; never used as the rear-cloth proof.'},
             'handoff_contract':({'kind':handoff['kind'],'runtime_activation_envelope':handoff['runtime_activation_envelope'],
                                  'required_phases':handoff['component_coverage_required_stages'],
                                  'H1_alpha_threshold':.5,'runtime_alpha_threshold':.5,
                                  'old_matching_sleeve_hand_materials_allowed_only_during_runtime_handoff':True,
                                  'Sit035_low_threshold_removed':True} if handoff else None),
             'limits':['This fixture detects actual material/Part/mask gaps; it does not approve proportions or beauty.',
                       'Standing Sit0 byte equivalence, full dense contact/orientation and transition opacity remain independent gates.',
                       'Raw independent neck Core cannot replace runtime mapped-neck review.',
                       ('Final hover/ground/lift/return H1 is opaque at .35; all probes require .5, including runtime old/new handoff union.' if handoff else 'Sleeve and palm .35 are an explicit opacity crossfade; .2 is used only for that phase, .5 after full deployment.'),
                       'Parameter-driven body contexts are represented; runtime physics and GPU clipping buffers require Native review.']}
    output=Path(output_path).resolve()
    if output in paths:raise ValueError('Fixture cannot overwrite an input.')
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(fixture,indent=2,allow_nan=False)+'\n',encoding='utf8')
    return fixture


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['plan','seed-fixture','leg-proof','arm-proof','output']:parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--handoff-proof',type=Path)
    args=parser.parse_args();result=build(args.plan,args.seed_fixture,args.leg_proof,args.arm_proof,args.output,args.handoff_proof)
    print(json.dumps({'poses':len(result['poses']),'checks':sum(len(p['checks']) for p in result['poses']),
                      'components':len(result['expected_topology'])}))


if __name__=='__main__':main()
