"""Read-only R9 material/joint evidence and executable rest-UV subdivision.

No production, runtime, author CMO, source PNG or preceding helper is edited.
New folded front-cloth geometry remains pending approved complete key poses.
"""
from pathlib import Path
import hashlib
import json
import sys
import numpy as np
from PIL import Image

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
sys.path.insert(0,str(ROOT/'tools'))
from verify_component_coverage import CoverageModel,sample_texture_alpha
from probe_cubism_core import digest

G=ROOT/'.local/authoring/sit-pose-20261004-r1'
MODEL=G/'materials-r8-final-return/assets/live2d/whale-girl/whale-girl-layered-draft.model3.json'
AUTHOR=G/'api/closed-legs-r8-return-r3b-20261005T060828Z-abca9aff/export'
PLAN=G/'geometry/closed-legs-r8-complete-plan.json'


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def find_anchor(runtime,name,point):
    source=runtime.source_xy(runtime.positions[name]);top=runtime.topology[name]
    for ti,verts in enumerate(top['indices']):
        mat=np.vstack((source[verts].T,np.ones(3)))
        if abs(np.linalg.det(mat))<1e-8:continue
        w=np.linalg.solve(mat,np.r_[point,1.])
        if w.min()<-1e-7:continue
        uv=w@top['uvs'][verts];page=runtime.pages[top['texture']]
        alpha=float(sample_texture_alpha(page,[uv])[0]);h,width=page.shape
        return {'drawable':name,'vertices':verts.tolist(),'weights':w.tolist(),
                'standing_source_xy':list(point),'triangle':ti,'minimum_barycentric_weight':float(w.min()),
                'texture_page':top['texture'],'native_uv':uv.tolist(),
                'PNG_texel_coordinate':(uv*[width,-h]+[0,h]).tolist(),'actual_bilinear_texture_alpha':alpha}
    raise ValueError(('No real triangle for material anchor',name,point))


def source_anchor(runtime,name,point,png,search_radius=0):
    requested=list(point);rgba=np.asarray(Image.open(ROOT/png).convert('RGBA'))
    if search_radius:
        candidates=sorted(([point[0]+x,point[1]+y] for x in range(-search_radius,search_radius+1) for y in range(-search_radius,search_radius+1)),
                          key=lambda p:(sum((np.asarray(p)-point)**2),p[1],p[0]))
        for candidate in candidates:
            x,y=np.rint(candidate).astype(int)
            if rgba[y,x,3]<224:continue
            try:trial=find_anchor(runtime,name,candidate)
            except ValueError:continue
            if trial['actual_bilinear_texture_alpha']>=224/255:
                point=candidate;break
        else:raise ValueError('No opaque material near requested waist seam.')
    a=find_anchor(runtime,name,point)
    x,y=np.rint(point).astype(int);a['source_PNG']=png;a['source_PNG_sha256']=digest(ROOT/png)
    a['source_PNG_RGBA']=rgba[y,x].tolist()
    a['requested_source_xy']=requested
    a['registration_adjustment_source_px']=float(np.linalg.norm(np.asarray(point)-requested))
    if a['actual_bilinear_texture_alpha']<224/255 or rgba[y,x,3]<224:
        raise ValueError(('Not real opaque source material',name,point,a['actual_bilinear_texture_alpha']))
    return a


def world(runtime,a):
    # Receipts also carry a source_xy diagnostic. It must not override the
    # actual moving barycentric point in CoverageModel.anchor.
    actual={key:a[key] for key in ['drawable','vertices','weights']}
    return runtime.source_xy(runtime.anchor(actual))


def arm_feasibility(runtime,arm_records,handoff):
    joints={}
    for side in ['L','R']:
        if side=='L':
            specs=[('shoulder','ArtMeshHandwearL2',[529.,614.],'art/live2d/layers/12-upperarm-l.png'),
                   ('elbow','ArtMeshHandwearL',[493.,714.],'art/live2d/layers/01-handwear-l.png'),
                   ('wrist','ArtMeshHandwearL',[412.,802.],'art/live2d/layers/01-handwear-l.png')]
        else:
            specs=[('shoulder','ArtMeshHandwearR5',[726.,614.],'art/live2d/layers/13-upperarm-r.png'),
                   ('elbow','ArtMeshHandwearR4',[762.,714.],'art/live2d/layers/02-handwear-r.png'),
                   ('wrist','ArtMeshHandwearR3',[842.,804.],'art/live2d/layers/00-handwear_r.png')]
        joints[side]={key:source_anchor(runtime,name,point,png) for key,name,point,png in specs}
    lengths={side:[float(np.linalg.norm(world(runtime,a['elbow'])-world(runtime,a['shoulder']))),
                   float(np.linalg.norm(world(runtime,a['wrist'])-world(runtime,a['elbow'])))]
             for side,a in joints.items()}
    rows=[]
    for sit in [0.,.35,.65,1.]:
        runtime.update({'parameters':{'ParamSitPose':sit,'ParamBusyLaptop':int(sit>=.93),
                                     'ParamHandGround':1. if 0<sit<1 else 0.,'ParamArmLA':0.,'ParamArmRA':0.}})
        row={'sit':sit,'current_R8_body_drop_source_px':float(np.interp(sit,[0.,.35,.65,1.],[0.,60.,120.,120.])),
             'sides':{}}
        for side in ['L','R']:
            record=arm_records['ArtMeshGroundUpperarm'+side]
            a={'drawable':'ArtMeshGroundUpperarm'+side,**record['material_anchors']['proximal']}
            shoulder=world(runtime,a)
            if sit==0:target=world(runtime,joints[side]['wrist']);intended='original standing hand'
            elif sit==1:target=world(runtime,handoff['busy_wrist_anchors'][side]);intended='busy laptop hand'
            else:
                target=world(runtime,{'drawable':'ArtMeshGroundPalm'+side,**handoff['palm_wrist_anchors'][side]})
                intended='hover hand' if sit==.35 else 'floor support wrist'
            L1,L2=lengths[side];maximum=L1+L2;d=target-shoulder;reach=float(np.linalg.norm(d))
            maximum_vertical=np.sqrt(max(0.,maximum**2-d[0]**2))
            row['sides'][side]={'shoulder_source_world_xy':shoulder.tolist(),'target_wrist_source_world_xy':target.tolist(),
                'gesture':intended,'standing_upperarm_length_source_px':L1,'standing_forearm_length_source_px':L2,
                'maximum_reach_source_px':maximum,'minimum_reach_source_px':abs(L1-L2),
                'actual_target_distance_source_px':reach,'end_to_end_ratio':reach/maximum,
                'reachable_without_lengthening':abs(L1-L2)<=reach<=maximum,
                'minimum_extra_shoulder_down_same_hand_X_source_px':float(max(0.,d[1]-maximum_vertical)),
                'minimum_extra_shoulder_down_hands_below_shoulder_source_px':float(max(0.,d[1]-maximum)),
                'criterion':'Two-link joint distance in [abs(L1-L2),L1+L2]. Sleeve overlap and opaque bounding boxes do not increase bone length.'}
        rows.append(row)
    return {'kind':'actual_material_bound_arm_reach_feasibility','joint_material_receipts':joints,'four_key_poses':rows,
        'hip_anatomical_anchor':None,'body_drop_120_is_locked':False,
        'conclusion':'Current upright .65 torso cannot reach floor with original short arms. Repositioning hands below shoulders alone is insufficient; body lean/foreshortening and pelvic/knee design must be approved together, or use knees/lap hand support.',
        'anatomy_limit':'Material pins define an explicit author joint proposal under visible sleeve/hand; source did not contain a named skeleton. No skirt/Topwear bounding box is called a hip bone.'}


def area(points,tri):
    p=points[tri];a=p[:,1]-p[:,0];b=p[:,2]-p[:,0]
    return a[:,0]*b[:,1]-a[:,1]*b[:,0]


def ear_triangles(polygon):
    p=np.asarray(polygon,float)
    signed=.5*np.sum(p[:,0]*np.roll(p[:,1],-1)-p[:,1]*np.roll(p[:,0],-1))
    if signed<0:p=p[::-1]
    ids=list(range(len(p)));output=[]
    while len(ids)>3:
        found=False
        for j in range(len(ids)):
            a,b,c=ids[j-1],ids[j],ids[(j+1)%len(ids)];u=p[b]-p[a];v=p[c]-p[b]
            if u[0]*v[1]-u[1]*v[0]<=1e-8:continue
            mat=np.vstack((p[[a,b,c]].T,np.ones(3)))
            others=[i for i in ids if i not in [a,b,c]]
            if others:
                weights=np.linalg.solve(mat,np.vstack((p[others].T,np.ones(len(others)))))
                if np.any(np.all(weights>=-1e-7,axis=0)):continue
            output.append(p[[a,b,c]]);ids.pop(j);found=True;break
        if not found:raise ValueError('Polygon not simple or ear triangulation failed.')
    output.append(p[ids]);return output


def convex_clip(attrs,triangle):
    poly=list(attrs)
    for a,b in zip(triangle,np.roll(triangle,-1,axis=0)):
        edge=b-a
        def distance(row):
            d=row[:2]-a
            return edge[0]*d[1]-edge[1]*d[0]
        out=[]
        for prev,current in zip(poly[-1:]+poly[:-1],poly):
            dp,dc=distance(prev),distance(current)
            if (dp>=0)!=(dc>=0):out.append(prev+(dp/(dp-dc))*(current-prev))
            if dc>=0:out.append(current)
        poly=out
        if not poly:break
    return poly


def uv_fragment(record,source,polygon,name):
    attrs=np.c_[source,np.asarray(record['positions']).reshape(-1,2),np.asarray(record['uvs']).reshape(-1,2)]
    old_tri=np.asarray(record['indices']).reshape(-1,3);regions=ear_triangles(polygon)
    points=[];base=[];uv=[];triangles=[];parents=[];lookup={}
    for old_index,verts in enumerate(old_tri):
        for region in regions:
            rows=convex_clip(attrs[verts],region)
            if len(rows)<3:continue
            ids=[]
            for row in rows:
                key=tuple(np.round(row[:2],6))
                if key not in lookup:
                    lookup[key]=len(points);points.append(row[:2]);base.append(row[2:4]);uv.append(row[4:6])
                ids.append(lookup[key])
            for i in range(1,len(ids)-1):
                tt=[ids[0],ids[i],ids[i+1]]
                if abs(area(np.asarray(points),np.asarray([tt]))[0])<1e-8:continue
                triangles.append(tt);parents.append(old_index)
    points=np.asarray(points);triangles=np.asarray(triangles)
    if not len(triangles) or np.any(area(points,triangles)>=0):raise ValueError(('UV clipped winding',name))
    return {'id':name,'status':'rest-only diagnostic fragment, not applied',
            'template_id':'ArtMeshSitFrontSkirt','parent_id':record['parent_id'],'part_id':'PartExtra','texture_page':0,
            'positions':np.asarray(base,dtype=np.float32).reshape(-1).tolist(),'uvs':np.asarray(uv,dtype=np.float32).reshape(-1).tolist(),
            'indices':triangles.reshape(-1).tolist(),'standing_source_positions':points.tolist(),
            'source_polygon':polygon,'source_parent_triangle_indices':parents,
            'original_texture_pixels_modified':False,'UV_interpolant_preserved':True,
            'minimum_source_triangle_area2':float(abs(area(points,triangles)).min()),
            'nonzero_pose_keyforms':'Pending approved new front material; these old UV fragments only demonstrate apron/ornament isolation.'}


def main():
    targets=['source-material-anchors.json','arm-reach-feasibility.json','old-cloth-UV-fragments.json','r9-authoring-contract.json']
    if any((HERE/n).exists() for n in targets):raise ValueError('Fresh local source design required.')
    model=read(MODEL);moc=MODEL.parent/model['FileReferences']['Moc']
    if digest(moc)!=digest(AUTHOR/'edited.moc3'):raise ValueError('Use exact restored R8 author candidate.')
    pins={str(p):digest(p) for p in [MODEL,moc,AUTHOR/'edited.cmo3',AUTHOR/'edit-report.json',PLAN,Path(__file__)]}
    runtime=CoverageModel(MODEL);runtime.update({'parameters':{'ParamSitPose':0,'ParamArmLA':0,'ParamArmRA':0}})
    front='ArtMeshSitFrontSkirt';waist={side:source_anchor(runtime,front,p,'art/live2d/layers/17-bottomwear.png',search_radius=8)
        for side,p in [('L',[515.,745.]),('C',[627.,745.]),('R',[735.,745.])]}
    knees={}
    for side in ['L','R']:
        name='ArtMeshSitKneeFront'+side;top=runtime.topology[name];vertex=25
        uv=top['uvs'][vertex];alpha=float(sample_texture_alpha(runtime.pages[top['texture']],[uv])[0]);h,w=runtime.pages[top['texture']].shape
        knees[side]={'drawable':name,'vertices':[vertex],'weights':[1.],
            'standing_source_xy':runtime.source_xy(runtime.positions[name][vertex]).tolist(),'texture_page':top['texture'],
            'native_uv':uv.tolist(),'PNG_texel_coordinate':(uv*[w,-h]+[0,h]).tolist(),
            'actual_bilinear_texture_alpha':alpha,'normal_from':{'drawable':name,'vertices':[0],'weights':[1.]},
            'crown_arc_vertices':[22,23,24,25,26,27,28],
            'meaning':'Actual textured front-knee upper material point. Not a hip or femur origin.'}
        if alpha<224/255:raise ValueError('Knee contact requires actual opaque skin material.')
    arm_proof=read(G/'api/ground-arms-geometry-r4/ground-arms-proof.json');arms={x['id']:x for x in arm_proof['segments']}
    handoff=read(G/'api/ground-handoff-r1/ground-handoff-proof.json')
    reach=arm_feasibility(runtime,arms,handoff)
    runtime.update({'parameters':{'ParamSitPose':0}});source=runtime.source_xy(runtime.positions[front]);record=next(x for x in read(PLAN)['insert_meshes'] if x['id']==front)
    polygons={'ArtMeshR9ApronOriginal':[[507,725],[731,725],[754,799],[727,849],[684,870],[554,870],[508,845],[483,796]],
        'ArtMeshR9BowOriginalL':[[472,840],[540,840],[540,913],[472,913]],
        'ArtMeshR9BowOriginalR':[[705,840],[773,840],[773,913],[705,913]],
        'ArtMeshR9FrillOriginal':[[383,887],[434,889],[521,918],[570,896],[627,886],[686,896],[739,918],[815,889],[855,887],
                                 [835,925],[795,947],[750,964],[500,964],[433,947],[400,923]]}
    fragments=[uv_fragment(record,source,p,n) for n,p in polygons.items()]
    anchor_report={'kind':'R9_actual_source_material_anchor_receipts','inputs':pins,'texture_inputs':runtime.pins,
                   'waist_cloth_anchors':waist,'knee_contact_material_anchors':knees,
                   'anatomical_hip_joints':None,'hip_state':'TBD author skeleton; cloth markers and mesh alpha bbox are not hip joints.',
                   'standing_source_origin':'source1254x1254 xright/ydown, Native PPU and canvas origin from actual Core.'}
    contract={'schema_version':1,'kind':'R9_integrated_cloth_and_joint_source_design','adopted':False,'inputs':pins,
        'inherit_author_CMO':str(AUTHOR/'edited.cmo3'),'inherit_author_MOC_sha256':digest(moc),
        'source_material_receipts':str(HERE/'source-material-anchors.json'),'arm_reach_report':str(HERE/'arm-reach-feasibility.json'),
        'approved_key_pose_source_state':'Parent generates complete clothed .35/.65/1 references; body/arm length errors require correction before binding.',
        'forbidden':['skin ellipse center as hip origin','fixed bodyDrop120 regardless of arm reach','lengthening sleeves to reach floor',
                     'unchanged original skirt above independent kneecap','hair alpha as cloth coverage','whole-cloth UV axial compression as a folded skirt'],
        'mesh_roles':[{'id':'ArtMeshR9ApronOriginal','source':'old atlas0 UV fragment','rule':'Logo, apron centre and waist upper band preserve material proportions.'},
                      {'id':'ArtMeshR9FrontLapL','source':'new approved front-cloth material pending','rule':'Thigh-covering panel joins fixed cloth waist to left knee crown support.'},
                      {'id':'ArtMeshR9FrontLapR','source':'new approved front-cloth material pending','rule':'Right counterpart with its own knee support.'},
                      {'id':'ArtMeshR9CentrePleat','source':'new approved material pending','rule':'Central valley connects both lap panels without exposing thigh.'},
                      {'id':'ArtMeshR9Frill','source':'new folded material or old UV frill only if proportions remain readable','rule':'Frontmost hem masks upper knee; follows curved cloth edge.'},
                      {'id':'ArtMeshR9BowL/R','source':'old atlas0 UV fragments','rule':'Move/rotate with panel; no continuous stretching of ribbons.'},
                      {'id':'ArtMeshSitRearSkirt','source':'existing page1 pure navy','rule':'Back-fold/underside follows pelvis and lap contact; no fixed background patch.'}],
        'blue_side_hair_policy':'Existing original BackHair groups keep ownership. New folded skirt never moves copied side-hair pixels from mixed Bottomwear chart.',
        'author_grid':{'parameter':'ParamSitPose','keys':[0.,.35,.65,1.],'support_keys':'Add only to resolve actual Core interpolation error or material collision.',
                       'coordinates':'Target source world→full actual parent inverse→float32 parent-local MeshDeltaForm. Never source pixels directly into local deltas.',
                       'waist_three_points':'Bind apron/lap rows to actual cloth receipts within .5sourcepx at each same-body context.',
                       'lap_support_two_points':'Bind cloth interior contact vertices to named knee crown material point along centre→crown normal; proposed gap0..2px, penetration>.5px fails.',
                       'hem':'Drape beyond support down knee front, hiding upper cap. Need approved silhouette; contact is not merely alpha overlap.',
                       'gold_bow_length_ratio_minimum':.995,'bow_shear':False,'frill_rule':'Bent arc with preserved edge-band thickness; no full row squash.',
                       'render_order':'rear fold < leg/knee < front lap/frill < laptop/hand. Current knee9/front5 must be replaced by explicit ownership.'},
        'four_key_joint_workflow':[{'sit':0,'state':'Original34 geometry+opacity and standing source protected byte-for-byte.'},
            {'sit':.35,'state':'Small knee bend; original short arm range can hover above lap, skirt begins knee-contact bend.'},
            {'sit':.65,'state':'Upright R8 torso/fixed shins plus floor hands is infeasible. Decide approved torso lean/pelvis change, or palms on knees/lap before modeling.'},
            {'sit':1,'state':'Closed knees, cloth covers thigh/top knee, natural short arms work on lap laptop; fully folded front material must be authored.'}],
        'required_gates':['actual skeleton reach before geometry','static full clothed user pose approval','source alpha/UV pin verification',
                          'original zero plane preservation','full-context actual Core joint lengths and contact','continuous signed areas',
                          'semantic knee/cloth alpha + correct render occlusion','Native shoulder attachment, natural proportions, cloth fold and reverse transition'],
        'state':'Executable source receipts and rest-UV fragments only; new front material and anatomically valid .65 pose pending.'}
    payloads=[anchor_report,reach,{'kind':'old_material_rest_UV_subdivision_audit','inputs':pins,'fragments':fragments,'applied':False},contract]
    for name,payload in zip(targets,payloads):(HERE/name).write_text(json.dumps(payload,indent=2,allow_nan=False)+'\n',encoding='utf8')
    if any(digest(p)!=sha for p,sha in pins.items()):raise ValueError('A source changed while reading R9 evidence.')
    print(json.dumps({'directory':str(HERE),'source_MOC_sha256':digest(moc),
        'arm_reach':[{ 'sit':r['sit'],'sides':{s:{'reach_px':v['actual_target_distance_source_px'],'lengths_px':[v['standing_upperarm_length_source_px'],v['standing_forearm_length_source_px']],
        'reachable':v['reachable_without_lengthening'],'extra_shoulder_down_px':v['minimum_extra_shoulder_down_same_hand_X_source_px']} for s,v in r['sides'].items()}} for r in reach['four_key_poses']],
        'UV_fragments':[{'id':r['id'],'vertices':len(r['positions'])//2,'triangles':len(r['indices'])//3} for r in fragments]},indent=2))


if __name__=='__main__':main()
