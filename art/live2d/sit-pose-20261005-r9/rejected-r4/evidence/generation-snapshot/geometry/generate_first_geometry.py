"""Stage typed public-grid edits; never mutate the author CMO or production files.

The first structural pass keeps shoes fixed and bends/foreshortens the leg
centre lines. The approved deep-knee front material is a separate UV child,
not manufactured by stretching the old skin stripe into an unapproved image.
"""
from pathlib import Path
import hashlib
import json
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parent
RUN = ROOT/'.local/authoring/whole-model-migration-20261004-r1/api-audit/moc-recovery-20261004T082620Z'
GRAPH = RUN/'export/recovered-parent-corrected-cmo-public-graph.json'
def read(p): return json.loads(p.read_text(encoding='utf8'))
def ref(p): return {'path':p.relative_to(ROOT).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
g = read(GRAPH)
D = {x['getId-bteaJEs']:x for x in g['getDrawables']}
W = {x['getId-u_t8HeU']:x for x in g['getDeformers']}
default = {x['getId-WD9NFvw']:x['getDefault'] for x in g['getParameters']}
evidence = read(ROOT/'.local/run/sit-contract/sit-coordinate-evidence.json')
KEYS = [0.0,0.35,0.65,1.0]
DROPS = [0.0,60.0,120.0,120.0]
BENDS = [0.0,18.0,35.0,35.0]
MIN_WIDTHS = [1.0,0.9,0.75,0.75]

def f32(x): return np.asarray(x,dtype=np.float32)
def neutral_cell(node):
    grid=node['getGeometryGrid'];coord=[min(range(len(a['getKeys'])),key=lambda i:abs(a['getKeys'][i]-default[a['getParameterId-WD9NFvw']])) for a in grid['getAxes']]
    return next(c for c in grid['getCells'] if c['getCoordinate']==coord)
def affine(node):
    n,m=node['getColumns'],node['getRows']; lattice=np.asarray([(x/n,y/m) for y in range(m+1) for x in range(n+1)])
    cp=np.asarray(neutral_cell(node)['getForm']['getControlPoints']).reshape(-1,2)
    a=np.linalg.lstsq(np.column_stack((lattice,np.ones(len(lattice)))),cp,rcond=None)[0]
    result=np.eye(3); result[:2]=a.T; return result
def parent_matrix(drawable):
    result=np.eye(3);p=drawable['getParentDeformerId-lmpY1tE']
    while p:
        n=W[p];result=affine(n)@result;p=n['getParent-lmpY1tE']
    return result
def transform(matrix,xy):return (np.column_stack((xy,np.ones(len(xy))))@matrix.T)[:,:2]
def absolute_form(node,cell):
    # Match FloatArray addition used by the public author model.
    return (f32(node['getMesh']['getPositions'])+f32(cell['getForm']['getPositionDeltas'])).reshape(-1,2)
def smooth(t):t=np.clip(t,0,1);return t*t*(3-2*t)
def leg_world(source,side,drop,bend,min_width):
    result=source.copy();x,y=source.T
    # Cuff and whole shoe are fixed in world XY. The smooth Y function stays
    # strictly monotone: min dY/dy = 1 - 1.5*drop/220 >=.181818 at max drop.
    descent_weight=1-smooth((y-900)/220)
    result[:,1]=y+drop*descent_weight
    u=np.clip((y-945)/175,0,1); knee_bump=4*u*(1-u)
    width=1-(1-min_width)*knee_bump
    center=560.0 if side=='L' else 700.0
    outward=-1 if side=='L' else 1
    result[:,0]=center+width*(x-center)+outward*bend*knee_bump
    rigid=y>=1120
    result[rigid]=source[rigid]
    return result
def cross2(a,b): return a[...,0]*b[...,1]-a[...,1]*b[...,0]
def area2(points,triangles):
    a,b,c=np.moveaxis(points[triangles],1,0)
    return cross2(b-a,c-a)
def all_interval_areas(forms,triangles):
    baseline=area2(forms[0],triangles);sign=np.sign(baseline)
    assert np.all(sign==-1)
    intervals=[];overall=float('inf');failures=[]
    for interval,(a,b) in enumerate(zip(forms[:-1],forms[1:])):
        aa=a[triangles];dd=(b-a)[triangles]
        e0=aa[:,1]-aa[:,0];e1=aa[:,2]-aa[:,0]
        d0=dd[:,1]-dd[:,0];d1=dd[:,2]-dd[:,0]
        c0=cross2(e0,e1);c1=cross2(e0,d1)+cross2(d0,e1);c2=cross2(d0,d1)
        minimum=float('inf');worst=None
        for i,(p,q,r) in enumerate(zip(c0,c1,c2)):
            ts=[0.,1.]
            if abs(r)>1e-18 and 0 < -q/(2*r) < 1:ts.append(float(-q/(2*r)))
            vals=[(p+q*t+r*t*t)*sign[i]/abs(baseline[i]) for t in ts]
            value=min(vals)
            if value<minimum:minimum=value;worst={'triangle':i,'vertices':triangles[i].tolist(),'ratio':value,'t':ts[int(np.argmin(vals))]}
            if value<=0:failures.append({'interval':interval,'triangle':i,'minimum_signed_ratio':value})
        overall=min(overall,minimum)
        intervals.append({'native_axis_interval':[KEYS[interval],KEYS[interval+1]],'minimum_area_ratio':minimum,'worst':worst})
    return {'all_triangles_strictly_same_sign':not failures,'minimum_area_ratio':overall,'intervals':intervals,'failures':failures}

def main():
    shift_matrix=affine(W['DeformBodyShift']);root_scale_y=shift_matrix[1,1]
    body=W['DeformBodyXY'];body_forms=[]
    for cell in body['getGeometryGrid']['getCells']:
        original=f32(cell['getForm']['getControlPoints']).reshape(-1,2)
        for index in range(1,4):
            cp=original.copy();cp[:,1]=f32(cp[:,1]+np.float32(DROPS[index]/root_scale_y))
            body_forms.append({'coordinate':cell['getCoordinate']+[index],'control_points':cp.reshape(-1).tolist()})

    mesh_edits=[];verification={};anchors={}
    for identifier,side in [('ArtMeshFootwearL','L'),('ArtMeshFootwearR','R')]:
        node=D[identifier];matrix=parent_matrix(node);inverse=np.linalg.inv(matrix)
        source_zero=transform(matrix,absolute_form(node,neutral_cell(node)))
        base=f32(node['getMesh']['getPositions']).reshape(-1,2)
        original_zero=absolute_form(node,neutral_cell(node)).astype(float)
        local_forms=[original_zero];world_forms=[source_zero];keyforms=[]
        for index in range(1,4):
            world=leg_world(source_zero,side,DROPS[index],BENDS[index],MIN_WIDTHS[index])
            # Remove common BodyXY descent before inverse mapping the leg mesh.
            compensated=world.copy();compensated[:,1]-=DROPS[index]
            local=transform(inverse,compensated)
            delta=f32(local-base).reshape(-1)
            local_actual=(base.reshape(-1)+delta).astype(np.float32).reshape(-1,2).astype(float)
            keyforms.append({'coordinate':[index],'position_deltas':delta.tolist()})
            local_forms.append(local_actual)
            world_actual=transform(matrix,local_actual);world_actual[:,1]+=DROPS[index]
            world_forms.append(world_actual)
        triangles=np.asarray(node['getMesh']['getIndices']).reshape(-1,3)
        proof=all_interval_areas(local_forms,triangles)
        anchor=evidence['footwear'][identifier]['anchors']['sole_contact_definition']
        indices=anchor['vertex_indices'];weights=np.asarray(anchor['barycentric_weights'])
        heel0=weights@source_zero[indices]
        contacts=[weights@points[indices] for points in world_forms]
        errors=[float(np.linalg.norm(point-heel0)) for point in contacts]
        rigid=source_zero[:,1]>=1120
        rigid_errors=[float(np.linalg.norm(points[rigid]-source_zero[rigid],axis=1).max()) for points in world_forms]
        proof.update({'key_heel_world_source_xy':[p.tolist() for p in contacts],
            'maximum_key_heel_error_source_px':max(errors),'fixed_heel_vertices':indices,'fixed_heel_weights':weights.tolist(),
            'rigid_shoe_vertex_count':int(rigid.sum()),'maximum_rigid_shoe_vertex_error_source_px':max(rigid_errors),
            'continuous_heel_constraint':'Both the BodyXY translation and mesh deltas interpolate linearly on the same four axis keys. Their opposite Y translations cancel; all heel triangle vertices are in the rigid shoe region. Float32 control-point rounding requires Core verification at the declared tolerance.',
            'world_key_bboxes_xyxy':[[*p.min(0).tolist(),*p.max(0).tolist()] for p in world_forms]})
        verification[identifier]=proof;anchors[identifier]=anchor
        mesh_edits.append({'id':identifier,'replace_axes':[{'parameter':'ParamSitPose','keys':KEYS,'source_key_indices':[0,1,2,3]}],'keyforms':keyforms})
        assert proof['all_triangles_strictly_same_sign'],proof['failures']
        assert proof['minimum_area_ratio']>.05
        assert max(errors)<.01 and max(rigid_errors)<.01

    # Hidden seated leg plates receive independent constant translations at all
    # nonzero approval keys. No texture re-drawing: this registers their contact
    # frame before selecting the seated material at the end of the transition.
    for identifier,filename,side in [('ArtMeshObjects5','busy-leg-l.png','L'),('ArtMeshObjects6','busy-leg-r.png','R')]:
        node=D[identifier];matrix=parent_matrix(node);inverse=np.linalg.inv(matrix)
        neutral=neutral_cell(node);source_zero=transform(matrix,absolute_form(node,neutral))
        base=f32(node['getMesh']['getPositions']).reshape(-1,2)
        image=np.asarray(Image.open(ROOT/'art/live2d/layers'/filename).convert('RGBA'))
        ys,xs=np.nonzero(image[:,:,3]>=128);bottom=int(ys.max());at_bottom=xs[ys==bottom]
        source_heel=np.asarray([(float(at_bottom.min())+float(at_bottom.max()))/2,bottom])
        triangles=np.asarray(node['getMesh']['getIndices']).reshape(-1,3)
        heel_triangle=None
        for ti,ind in enumerate(triangles):
            xy=source_zero[ind];a=np.vstack((xy.T,np.ones(3)))
            if abs(np.linalg.det(a))<1e-12:continue
            bary=np.linalg.solve(a,np.r_[source_heel,1])
            if bary.min()>=-1e-8:heel_triangle=(ti,ind,bary);break
        assert heel_triangle is not None,(identifier,source_heel)
        target=np.asarray(anchors['ArtMeshFootwear'+side]['source_point_xy'])
        keyforms=[];movement=[]
        for index in range(1,4):
            shift=target-source_heel-np.asarray([0,DROPS[index]])
            movement.append({'key':KEYS[index],'source_delta_before_root':shift.tolist()})
            for busy_index in range(2):
                cell=next(c for c in node['getGeometryGrid']['getCells'] if c['getCoordinate']==[1,busy_index])
                original=transform(matrix,absolute_form(node,cell))
                local=transform(inverse,original+shift)
                delta=f32(local-base).reshape(-1)
                keyforms.append({'coordinate':[index,busy_index],'position_deltas':delta.tolist()})
        mesh_edits.append({'id':identifier,'replace_axes':[{'parameter':'ParamSitPose','keys':KEYS,'source_key_indices':[0,1,1,1]}],'keyforms':keyforms})
        ti,ind,bary=heel_triangle
        verification[identifier]={'source_heel':source_heel.tolist(),'target_heel':target.tolist(),
            'heel_triangle':ti,'heel_vertices':ind.tolist(),'heel_barycentric_weights':bary.tolist(),
            'constant_translation_only':True,'seated_contact_valid_from_Sit035':True,
            'key_deltas':movement,'source_zero_forms_untouched':True,
            'material_note':'Hidden seated material is not selected during the start-to-.35 interpolation. Registration geometry preserves its texture topology; new rear cloth must cover any exposed upper-thigh seam.'}

    def determinant_interval_proof(node,extra_forms=None):
        cp=np.asarray([c['getForm']['getControlPoints'] for c in node['getGeometryGrid']['getCells']],dtype=np.float32).reshape(-1,node['getRows']+1,node['getColumns']+1,2).astype(float)
        if extra_forms:
            add=np.asarray([c['control_points'] for c in extra_forms],dtype=np.float32).reshape(-1,node['getRows']+1,node['getColumns']+1,2).astype(float)
            cp=np.concatenate((cp,add))
        du=np.diff(cp,axis=2)*node['getColumns'];dv=np.diff(cp,axis=1)*node['getRows']
        du_flat=du.reshape(-1,2);dv_flat=dv.reshape(-1,2)
        # All rows have identical Y across columns, including added forms. Thus
        # dY/dX=0, and determinant is dX/dX*dY/dY. Both remain positive under
        # convex interpolation of lattice rows and keyform parameters.
        assert np.array_equal(du_flat[:,1],np.zeros(len(du_flat)))
        lower=float(du_flat[:,0].min()*dv_flat[:,1].min())
        assert lower>0
        return {'minimum_du_x':float(du_flat[:,0].min()),'maximum_abs_du_y':float(np.abs(du_flat[:,1]).max()),
            'minimum_dv_y':float(dv_flat[:,1].min()),'strict_positive_determinant_lower_bound':lower,
            'proof_scope':'Public rectangular control lattice derivatives and convex grid/key interpolation on the unit parent domain; final compiled Core dense samples remain a separate gate.'}
    jacobian={name:determinant_interval_proof(W[name],body_forms if name=='DeformBodyXY' else None)
              for name in ['DeformPair_Footwear','DeformBodyZBreath','DeformBodyXY','DeformBodyShift']}

    plan={'schema_version':1,'kind':'approved_sit_structural_first_pass_typed_public_grid_plan',
        'source':{'cmo_sha256':ref(RUN/'export/formal-moc-recovered-parent-corrected.cmo3')['sha256'],
            'moc_sha256':ref(RUN/'export/cmo-parent-corrected.moc3')['sha256'],
            'atlas_sha256':ref(ROOT/'assets/live2d/whale-girl/whale-girl-layered-draft.4096/texture_00.png')['sha256']},
        'coordinate_contract':{'mesh_position_deltas':'float32 in each unchanged current parent-local mesh domain; base/UV/index arrays untouched',
            'deformer_control_points':'float32 in the existing output-to-parent domain; BodyXY output is BodyShift unit input, not canvas source pixels',
            'source_canvas':[1254,1254],'source_to_native':'[(x-627)/1254,(627-y)/1254]'},
        'deformer_grids':[{'id':'DeformBodyXY','append_axes':[{'parameter':'ParamSitPose','keys':KEYS}],'keyforms':body_forms}],
        'mesh_grids':mesh_edits,
        'authoredSitPose':{'version':1,'logicalToNative':'identity','keys':KEYS,
            'legacyStandingFoldRemap':False,'legacyMinimumYGrounding':False,'legacyCollarFloorAffine':False,
            'independentNeckR2':'Update once from actual final head/collar samples after Core. No second posture affine.',
            'bodyDropSourcePixels':DROPS,'key1_head_and_collar_source_delta':[0,120],
            'seatedMaterialContactReadyFrom':0.35,
            'seatedMaterialSelection':'Do not select the seated plates during Sit<.35; preserve exclusive body material selection in the authored runtime path.'},
        'limits':{'material_stage':'Approved deep-knee front is not claimed finished by this geometry-only first pass. Reuse existing skin UV with a new knee-front mesh and add the reviewed rear cloth/palms as separate materials.',
            'new_assets_generated':False,'production_mutated':False,
            'new_support_axes':['ParamSkirtSpread','ParamHandGround'],'geometry_first_preview':'Native raw pose review for 0/.35/.65/1 and reverse contact before adoption.'}}
    (OUT/'candidate-grid-plan.json').write_text(json.dumps(plan,ensure_ascii=False,indent=2),encoding='utf8')
    report={'kind':'first_sit_geometry_interval_contact_proof','input':ref(GRAPH),'plan':ref(OUT/'candidate-grid-plan.json'),
        'keys':KEYS,'body_drops_source_px':DROPS,'source_zero_plane_policy':'All source Sit=0 cells are retained by the public editor without reserialization.',
        'footwear':verification,'root_control_lattice_jacobian':jacobian,
        'maximum_descent_map_differential':{'minimum_dY_dSourceY':1-1.5*max(DROPS)/220,'minimum_dX_dSourceX':min(MIN_WIDTHS)},
        'proof_status':'Passed retained local triangles, rigid-shoe/contact key constraints and positive public root lattice differential. Compiled Core/native visual verification pending.'}
    (OUT/'candidate-geometry-proof.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(json.dumps({'plan':str(OUT/'candidate-grid-plan.json'),
        'footwear':{name:{k:v[k] for k in ['minimum_area_ratio','maximum_key_heel_error_source_px','maximum_rigid_shoe_vertex_error_source_px']} for name,v in verification.items() if name.startswith('ArtMeshFootwear')},
        'body_forms':len(body_forms),'root_minimum_jacobians':{name:v['strict_positive_determinant_lower_bound'] for name,v in jacobian.items()}},indent=2))


if __name__ == "__main__":
    main()
