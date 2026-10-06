"""R4: rigid calf rotation plus separately fixed shoes and joint-bound knees.

This emits isolated public-editor JSON. It does not modify source PNGs, atlas,
CMO, production model or runtime. No whole-stocking axial compression is used.
"""
from pathlib import Path
import json
import numpy as np
import body_context_geometry as m
from generate_body_context_plan import MESH_KEYS,BODY_AXES,interval_proof
from generate_child_materials import (skin_polygon,old_chart,uv_at,barycentric,
                                     clip_halfplanes)

s=m.source
OUT=Path(__file__).resolve().parent
AXES=[{'parameter':'ParamSitPose','keys':MESH_KEYS}]+BODY_AXES
ANGULAR_KEYS=[0.,12.,24.,24.]
CROSSFADE_END=.0875
OVERLAP=3.
THETA=np.arange(32)*2*np.pi/32
PIVOTS={'L':[555.,1134.],'R':[698.,1134.]}
CUFFS={'L':[570.,984.],'R':[686.,984.]}
LINE_ANCHORS={'L':[[558.,990.],[558.,1120.]],'R':[[698.,990.],[698.,1120.]]}


def angle(sit,side):
    degrees=sum(w*ANGULAR_KEYS[i] for i,w in m.weights(s.KEYS,sit))
    return np.deg2rad(degrees)*(-1 if side=='L' else 1)


def rotation(theta):
    c,t=np.cos(theta),np.sin(theta)
    return np.asarray([[c,-t],[t,c]])


def material_submesh(node,side,kind):
    source,uv,old_triangles=old_chart(node)
    halfplanes=([(1,0,-490),(1,.8,-1310),(-1,.2,417)] if side=='L'
                else [(-1,0,765),(-1,.8,-55),(1,.2,-839)])
    halfplanes += ([(0,1,-984),(0,-1,1146)] if kind=='stocking' else [(0,1,-1120)])
    points=[];uvs=[];triangles=[];lookup={};old_indices=[]
    for source_index,vertices in enumerate(old_triangles):
        polygon=clip_halfplanes([np.r_[source[v],uv[v]] for v in vertices],halfplanes)
        if len(polygon)<3:continue
        ids=[]
        for row in polygon:
            key=tuple(np.round(row[:2],6))
            if key not in lookup:lookup[key]=len(points);points.append(row[:2]);uvs.append(row[2:])
            else:assert np.max(abs(np.asarray(uvs[lookup[key]])-row[2:]))<1e-6
            ids.append(lookup[key])
        for i in range(1,len(ids)-1):
            tri=[ids[0],ids[i],ids[i+1]]
            if abs(s.area2(np.asarray(points),np.asarray([tri]))[0])<1e-8:continue
            triangles.append(tri);old_indices.append(source_index)
    points=np.asarray(points);triangles=np.asarray(triangles)
    assert np.all(s.area2(points,triangles)<0)
    return points,np.asarray(uvs),triangles,{'clipping_halfplanes_aX_bY_c_ge0':halfplanes,
                'old_triangle_index_for_each_child_triangle':old_indices,
                'source_bbox':[*points.min(0).tolist(),*points.max(0).tolist()],
                'material_policy':'Existing atlas0 UV submesh only. No PNG editing. Calf and shoe overlap in source Y1120..1146; fixed navy shoe is rendered above the moving calf.'}


def point_reference(node,source_xy):
    return m.local_from_world(node,{},np.asarray(source_xy).reshape(-1,2),False)


def find_anchor(point,points,triangles):
    for index,vertices in enumerate(triangles):
        weights=barycentric(np.asarray(point),points[vertices])
        if weights is not None and weights.min()>=-1e-7:
            return {'triangle':int(index),'vertices':vertices.tolist(),'weights':weights.tolist(),
                    'source_xy':list(point)}
    raise AssertionError(('Anchor not on material',point))


def make_child(side,kind):
    node=s.D['ArtMeshFootwear'+side];old_source,old_uv,old_triangles=old_chart(node)
    if kind=='knee':
        points,triangles,origin=skin_polygon(side);uvs,mapping=uv_at(points,old_source,old_uv,old_triangles)
        origin['atlas_mapping']=mapping;identifier='ArtMeshSitKneeFront'+side
    else:
        points,uvs,triangles,origin=material_submesh(node,side,kind)
        identifier=('ArtMeshSitStocking' if kind=='stocking' else 'ArtMeshSitShoe')+side
    base=s.f32(point_reference(node,points)).reshape(-1,2)
    pivot_local=point_reference(node,PIVOTS[side]);cuff_local=point_reference(node,CUFFS[side])
    forms=[];proofs=[];all_forms={};joint_keys=[];key_world={}
    for bx in range(3):
        for by in range(3):
            params=m.context(bx,by);zero=m.mesh_world(node,params,False,base.astype(float))
            pivot=m.mesh_world(node,params,False,pivot_local)[0]
            original_cuff=m.mesh_world(node,params,False,cuff_local)[0]
            local_forms=[];world_forms=[]
            for is_,sit in enumerate(MESH_KEYS):
                params=m.context(bx,by,sit);theta=angle(sit,side);R=rotation(theta)
                rotated=(zero-pivot)@R.T+pivot
                cuff=(original_cuff-pivot)@R.T+pivot
                if kind=='shoe':world=zero.copy()
                elif kind=='stocking':world=rotated
                else:
                    t=max(0.,min((sit-.35)/.30,1.));rx,ry=38.+7*t,22.+10*t
                    center=cuff-R@np.asarray([0.,ry-OVERLAP])
                    ellipse=np.vstack((center,center+np.column_stack((rx*np.cos(THETA),ry*np.sin(THETA)))@R.T))
                    blend=min(sit/.35,1.)
                    world=(1-blend)*rotated+blend*ellipse
                    if bx==by==1:
                        joint_keys.append({'native_sit':sit,'angle_degrees':float(np.rad2deg(theta)),
                                            'ankle_pivot_source_world_xy':pivot.tolist(),'sock_cuff_source_world_xy':cuff.tolist(),
                                            'knee_center_source_world_xy':world[0].tolist(),
                                            'knee_radius_xy_source_px':[rx,ry],
                                            'knee_bottom_overlap_source_px':OVERLAP if sit>=.35 else None})
                local=base.copy() if is_==0 else m.local_from_world(node,params,world,True)
                delta=s.f32(local-base).reshape(-1)
                actual=(base.reshape(-1)+delta).astype(np.float32).reshape(-1,2).astype(float)
                local_forms.append(actual);world_forms.append(world)
                forms.append({'coordinate':[is_,bx,by],'position_deltas':delta.tolist()})
            proof=interval_proof(local_forms,triangles)
            assert proof['all_triangles_strictly_same_sign'],proof
            proof.update({'body_x':m.BODY_KEYS[bx],'body_y':m.BODY_KEYS[by]})
            proofs.append(proof);all_forms[(bx,by)]=local_forms;key_world[(bx,by)]=world_forms
    opacity=[{'coordinate':[index],'value':min(sit/CROSSFADE_END,1.)} for index,sit in enumerate(MESH_KEYS)]
    order=(9 if kind=='knee' else (6 if kind=='stocking' else (8 if side=='L' else 7)))
    record={'id':identifier,'name':{'knee':'Front knee ','stocking':'Rigid folded calf ','shoe':'Fixed complete shoe '}[kind]+side,
            'template_id':'ArtMeshFootwear'+side,'parent_id':'DeformPair_Footwear','part_id':'PartExtra',
            'texture_page':0,'positions':base.reshape(-1).tolist(),'uvs':s.f32(uvs).reshape(-1).tolist(),
            'indices':triangles.reshape(-1).tolist(),'axes':AXES,'keyforms':forms,
            'channels':[{'channel':'OPACITY','initial_value':0,'append_axes':[{'parameter':'ParamSitPose','keys':MESH_KEYS}],
                         'keyforms':opacity}],'draw_order':order,'opacity':0}
    report={'id':identifier,'kind':kind,'vertex_count':len(base),'triangle_count':len(triangles),
            'source_geometry':origin,'part':'PartExtra','draw_order':order,'Sit0_invisible':True,
            'local_interval_proofs':proofs,'minimum_area_ratio':min(p['minimum_area_ratio'] for p in proofs),
            'ankle_pivot_source_xy':PIVOTS[side],'sock_cuff_source_xy':CUFFS[side],
            'rotation_approved_key_degrees':[(-1 if side=='L' else 1)*v for v in ANGULAR_KEYS],
            'full_rotation_interval_minimum_length_scale':float(min(np.cos(abs(angle(b,side)-angle(a,side))/2) for a,b in zip(MESH_KEYS[:-1],MESH_KEYS[1:]))) if kind=='stocking' else None}
    if kind=='knee':report['neutral_body_joint_keys']=joint_keys
    if kind=='stocking':
        report['length_anchors']=[find_anchor(point,points,triangles) for point in LINE_ANCHORS[side]]
        report['length_contract']='Every material vertex is a rigid rotation around the ankle in world XY at each key; the proximal/distal centerline length is preserved. Between keys, linear Cartesian interpolation contracts by at most cos(deltaAngle/2), with support keys limiting this below 0.04%. BodyXY parent-domain inversion has separate compiled-Core tolerances.'
        report['expected_neutral_original_centerline_length_source_px']=130.
    if kind=='shoe':
        original=s.evidence['footwear']['ArtMeshFootwear'+side]['anchors']['sole_contact_definition']
        heel=np.asarray(original['barycentric_weights'])@old_source[original['vertex_indices']]
        a=find_anchor(heel,points,triangles)
        report.update({'heel_vertices':a['vertices'],'heel_weights':a['weights'],
                       'standing_target_id':'ArtMeshFootwear'+side,'neutral_heel_source_xy':heel.tolist(),
                       'rigid_all_material_vertices':True,'target_is_same_body_context_formal_Sit0':True})
    return record,report,all_forms


def main():
    plan=s.read(OUT/'continuous-legs-material-plan-r3.json');children=[];proofs=[]
    for side in ['L','R']:
        for kind in ['knee','stocking','shoe']:
            child,p,_=make_child(side,kind);children.append(child);proofs.append(p)
    plan['insert_meshes']=children
    # Keep all old source Sit0 channel values. Complete the material handoff
    # on the first link key so the obsolete axial compression never carries
    # the visible transition after the initial short crossfade.
    for edit in plan['mesh_grids']:
        if edit['id'] in ['ArtMeshFootwearL','ArtMeshFootwearR','ArtMeshObjects5','ArtMeshObjects6']:
            channel=edit['channels'][0]
            for cell in channel['keyforms']:
                cell['value']=cell['value'] if cell['coordinate'][-1]==0 else 0.
    plan['authoredSitPose'].update({'version':2,'continuousLegDrawableIds':[c['id'] for c in children],
            'legConstruction':'Rigid calf rotation about ankle plus separately fixed full shoes; no axial compression of the stocking material.',
            'materialCrossfadeEnd':CROSSFADE_END,'legacySeatedLegOpacityOffFrom':CROSSFADE_END,
            'sockKneeOverlapSourcePixels':OVERLAP,'frontKneeDrawOrder':9,
            'seatedMaterialSelection':'Use knee/calf/shoe PartExtra children through Sit=1. Old seated legs are disabled after the first support key.',
            'frontSkirtRequirement':'Root-owned PartExtra front-skirt clone at integer order5 throughout Sit. Hide original BusySkirt to avoid a missing-leg cutout; retain separate rear cloth behind knee/calf materials.'})
    report={'schema_version':1,'kind':'rigid_folded_leg_UV_child_geometry_candidate','adopted':False,
            'source':plan.get('source'),'source_pngs_or_atlas_modified':False,'support_keys':MESH_KEYS,
            'approved_keys':s.KEYS,'material_crossfade_end':CROSSFADE_END,'children':proofs,
            'contact_contract':'Same BodyXY context formal Sit0 full shoe vertices and heel XY, not a single neutral global coordinate.',
            'legacy_hidden_drawables':['ArtMeshFootwearL','ArtMeshFootwearR','ArtMeshObjects5','ArtMeshObjects6'],
            'legacy_hidden_from_native_sit':CROSSFADE_END,
            'root_owned_pending_components':['PartExtra original front-skirt clone','rear skirt cloth','support palms/wrist binding'],
            'compiled_core_required':True,'visual_review_required':True,
            'structural_limit':'This is the approved crouch leg construction using existing stocking/shoe/skin pixels. Rear-thigh and hip connection are cloth occlusion, not invented painted skin. Native alpha-aware composition must prove knee/cuff and ankle coverage.'}
    (OUT/'folded-legs-plan-r4.json').write_text(json.dumps(plan,indent=2,allow_nan=False)+'\n',encoding='utf8')
    (OUT/'folded-legs-proof-r4.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':'folded-legs-plan-r4.json','children':[{'id':p['id'],'vertices':p['vertex_count'],'triangles':p['triangle_count'],
                 'min_area_ratio':p['minimum_area_ratio'],'min_length_scale':p['full_rotation_interval_minimum_length_scale']} for p in proofs],
                 'joint_keys':{p['id']:p.get('neutral_body_joint_keys') for p in proofs if p['kind']=='knee'}},indent=2))


if __name__=='__main__':main()
