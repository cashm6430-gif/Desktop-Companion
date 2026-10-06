"""R6/R7 full ancestor leg retargeting in an isolated public-editor plan.

Existing helpers are intentionally unchanged. The inverse supports a general
bilinear warp (cross-axis Jacobian), including extended edge cells. Root R5
UV/pages/channels and all existing default-Z/default-Breath deltas are kept.
"""
from pathlib import Path
import argparse
import hashlib
import json
import numpy as np
import body_context_geometry as parent
from generate_body_context_plan import MESH_KEYS as SOURCE_MESH_KEYS,interval_proof
from generate_folded_legs import PIVOTS,CUFFS,angle,rotation

s=parent.source
OUT=Path(__file__).resolve().parent
INPUT=OUT/'support-clean-knees-r5-plan.json'
BODY_Z_KEYS=s.W['DeformBodyZBreath']['getGeometryGrid']['getAxes'][0]['getKeys']
BREATH_KEYS=s.W['DeformBodyZBreath']['getGeometryGrid']['getAxes'][1]['getKeys']
ADDITIONAL_AXES=[{'parameter':'ParamBodyAngleZ','keys':BODY_Z_KEYS},
                 {'parameter':'ParamBreath','keys':BREATH_KEYS}]
DEFAULT_COORD=[BODY_Z_KEYS.index(s.default['ParamBodyAngleZ']),BREATH_KEYS.index(s.default['ParamBreath'])]
MESH_KEYS=sorted(SOURCE_MESH_KEYS+[.5375,.6125])


def source_delta_at(cells,bx,by,sit):
    """Keep old-key bytes; new default forms preserve the old local interpolant."""
    if sit in SOURCE_MESH_KEYS:return cells[(SOURCE_MESH_KEYS.index(sit),bx,by)].copy()
    return s.f32(sum(weight*cells[(index,bx,by)].astype(float)
                     for index,weight in parent.weights(SOURCE_MESH_KEYS,sit)))


def warp_apply_jacobian(node,lattice,points):
    cols,rows=node['getColumns'],node['getRows']
    cp=np.asarray(lattice).reshape(rows+1,cols+1,2)
    points=np.asarray(points,dtype=float).reshape(-1,2)
    scaled=points*[cols,rows];ij=np.clip(np.floor(scaled).astype(int),[0,0],[cols-1,rows-1])
    x,y=ij.T;u,v=(scaled-ij).T
    a,b,c,d=cp[y,x],cp[y,x+1],cp[y+1,x],cp[y+1,x+1]
    world=(a*(1-u[:,None])*(1-v[:,None])+b*u[:,None]*(1-v[:,None])
           +c*(1-u[:,None])*v[:,None]+d*u[:,None]*v[:,None])
    dx=cols*((b-a)*(1-v[:,None])+(d-c)*v[:,None])
    dy=rows*((c-a)*(1-u[:,None])+(d-b)*u[:,None])
    jacobian=np.stack((dx,dy),axis=2)
    outside=np.any((points<0)|(points>=1),axis=1)
    if np.any(outside):
        world[outside],jacobian[outside]=warp_extrap_jacobian(node,lattice,points[outside])
    return world,jacobian


def warp_extrap_jacobian(node,lattice,points):
    """Public DeformTransformsKt.warpExtrap perimeter/far-affine contract.

    The public API triangulates a two-unit perimeter around the boundary CPs;
    it does not extrapolate the edge bilinear cell. Source: exported public
    API bytecode DeformTransforms-bytecode.txt, warpExtrap, offsets 0..1573.
    """
    cols,rows=node['getColumns'],node['getRows']
    cp=np.asarray(lattice,dtype=float).reshape(rows+1,cols+1,2)
    a,b,c,d=cp[0,0],cp[0,-1],cp[-1,0],cp[-1,-1]
    axis_x=(d-a+b-c)*.5;axis_y=(d-a-b+c)*.5
    origin=(a+b+c+d)*.25-(d-a)*.5
    affine=lambda x,y:origin+x*axis_x+y*axis_y
    output=[];jacobians=[]
    for x,y in np.asarray(points,dtype=float).reshape(-1,2):
        if x<=-2 or x>=3 or y<=-2 or y>=3:
            output.append(affine(x,y));jacobians.append(np.column_stack((axis_x,axis_y)));continue
        if x<=0:
            u=(x+2)*.5;us=.5
            if y<=0:
                v=(y+2)*.5;vs=.5
                A,B,C,D=affine(-2,-2),affine(0,-2),affine(-2,0),a
            elif y<1:
                ri=min(int(y*rows),rows-1);v=y*rows-ri;vs=rows
                A,B,C,D=affine(-2,ri/rows),cp[ri,0],affine(-2,(ri+1)/rows),cp[ri+1,0]
            else:
                v=(y-1)*.5;vs=.5
                A,B,C,D=affine(-2,1),c,affine(-2,3),affine(0,3)
        elif x<1:
            ci=min(int(x*cols),cols-1);u=x*cols-ci;us=cols
            if y<=0:
                v=(y+2)*.5;vs=.5
                A,B,C,D=affine(ci/cols,-2),affine((ci+1)/cols,-2),cp[0,ci],cp[0,ci+1]
            else:
                v=(y-1)*.5;vs=.5
                A,B,C,D=cp[-1,ci],cp[-1,ci+1],affine(ci/cols,3),affine((ci+1)/cols,3)
        else:
            u=(x-1)*.5;us=.5
            if y<=0:
                v=(y+2)*.5;vs=.5
                A,B,C,D=affine(1,-2),affine(3,-2),b,affine(3,0)
            elif y<1:
                ri=min(int(y*rows),rows-1);v=y*rows-ri;vs=rows
                A,B,C,D=cp[ri,-1],affine(3,ri/rows),cp[ri+1,-1],affine(3,(ri+1)/rows)
            else:
                v=(y-1)*.5;vs=.5
                A,B,C,D=d,affine(3,1),affine(1,3),affine(3,3)
        if u+v<=1:
            output.append(A+(B-A)*u+(C-A)*v)
            jacobians.append(np.column_stack(((B-A)*us,(C-A)*vs)))
        else:
            output.append(D+(C-D)*(1-u)+(B-D)*(1-v))
            jacobians.append(np.column_stack(((D-C)*us,(D-B)*vs)))
    return np.asarray(output),np.asarray(jacobians)


def world_lattices(parameters,authored=False):
    """Resolve every ancestor CP using the public in-grid/outside rule."""
    cache={}
    def resolve(identifier):
        if identifier in cache:return cache[identifier]
        node=s.W[identifier];cp=parent.interpolate_grid(node,parameters,authored)
        ancestor=node['getParent-lmpY1tE']
        if ancestor:cp=warp_apply_jacobian(s.W[ancestor],resolve(ancestor),cp)[0]
        cache[identifier]=cp
        return cp
    return resolve


def generic_warp_inverse(node,lattice,target):
    """Newton inverse of actual final-world CP; no Y-separable assumption."""
    target=np.asarray(target,dtype=float).reshape(-1,2)
    cols,rows=node['getColumns'],node['getRows']
    normalized=np.asarray([(x/cols,y/rows) for y in range(rows+1) for x in range(cols+1)])
    fitted=np.linalg.lstsq(np.column_stack((normalized,np.ones(len(normalized)))),np.asarray(lattice).reshape(-1,2),rcond=None)[0]
    points=(target-fitted[2])@np.linalg.inv(fitted[:2])
    minimum_jacobian=float('inf')
    for iteration in range(24):
        actual,J=warp_apply_jacobian(node,lattice,points)
        error=actual-target
        determinants=J[:,0,0]*J[:,1,1]-J[:,0,1]*J[:,1,0]
        minimum_jacobian=min(minimum_jacobian,float(determinants.min()))
        if np.any(determinants<=1e-7):raise ValueError('Parent warp lost positive Jacobian during inverse.')
        if float(abs(error).max())<1e-8:break
        delta=np.column_stack(((J[:,1,1]*error[:,0]-J[:,0,1]*error[:,1])/determinants,
                               (-J[:,1,0]*error[:,0]+J[:,0,0]*error[:,1])/determinants))
        points-=delta
    actual,J=warp_apply_jacobian(node,lattice,points)
    residual=float(abs(actual-target).max())
    if not np.all(np.isfinite(points)) or residual>1e-6:raise ValueError(('Generic warp inverse did not converge',residual))
    return points,{'max_residual_source_px':residual,'iterations':iteration+1,'minimum_evaluated_jacobian':minimum_jacobian}


def params(bx,by,sit,z=0.,breath=0.):
    return {'ParamBodyAngleX':parent.BODY_KEYS[bx],'ParamBodyAngleY':parent.BODY_KEYS[by],
            'ParamSitPose':sit,'ParamBodyAngleZ':z,'ParamBreath':breath}


def world_positions(node_parent,local,parameters,authored):
    return warp_apply_jacobian(s.W[node_parent],world_lattices(parameters,authored)(node_parent),local)[0]


def local_inverse(node_parent,world,parameters):
    return generic_warp_inverse(s.W[node_parent],world_lattices(parameters,True)(node_parent),world)


def byte_hash(values):return hashlib.sha256(np.asarray(values,dtype=np.float32).tobytes()).hexdigest()


def old_mesh_forms(node,edit):
    base=s.f32(node['getMesh']['getPositions']).reshape(-1,2)
    zero=s.f32(s.neutral_cell(node)['getForm']['getPositionDeltas']).reshape(-1,2)
    cells={tuple(c['coordinate']):s.f32(c['position_deltas']).reshape(-1,2) for c in edit['keyforms']}
    for bx in range(3):
        for by in range(3):cells[(0,bx,by)]=zero
    return base,cells


def retarget(identifier,record,old=False):
    node=s.D[identifier] if old else None
    node_parent=node['getParentDeformerId-lmpY1tE'] if old else record['parent_id']
    if old:base,cells=old_mesh_forms(node,record);indices=np.asarray(node['getMesh']['getIndices']).reshape(-1,3)
    else:
        base=s.f32(record['positions']).reshape(-1,2)
        cells={tuple(c['coordinate']):s.f32(c['position_deltas']).reshape(-1,2) for c in record['keyforms']}
        indices=np.asarray(record['indices']).reshape(-1,3)
    side='L' if identifier.endswith('L') else 'R'
    kind=('old_footwear' if old else ('stocking' if 'Stocking' in identifier else ('shoe' if 'Shoe' in identifier else 'knee')))
    foot=s.D['ArtMeshFootwear'+side]
    pivot_local=parent.local_from_world(foot,{},np.asarray(PIVOTS[side]).reshape(-1,2),False)
    cuff_local=parent.local_from_world(foot,{},np.asarray(CUFFS[side]).reshape(-1,2),False)
    keyforms=[];proofs=[];minimum_jacobian=float('inf');maximum_inverse_residual=0.
    default_checks=[];all_forms={};maximum_target_residual=0.
    for bx in range(3):
        for by in range(3):
            zero=(base+cells[(0,bx,by)]).astype(np.float32).astype(float)
            original_default=world_positions(node_parent,zero,params(bx,by,0),False)
            default_pivot=world_positions(node_parent,pivot_local,params(bx,by,0),False)[0]
            default_cuff=world_positions(node_parent,cuff_local,params(bx,by,0),False)[0]
            for zindex,z in enumerate(BODY_Z_KEYS):
                for bindex,breath in enumerate(BREATH_KEYS):
                    original=world_positions(node_parent,zero,params(bx,by,0,z,breath),False)
                    pivot=world_positions(node_parent,pivot_local,params(bx,by,0,z,breath),False)[0]
                    cuff=world_positions(node_parent,cuff_local,params(bx,by,0,z,breath),False)[0]
                    local_forms=[]
                    for sitindex,sit in enumerate(MESH_KEYS):
                        coordinate=(sitindex,bx,by,zindex,bindex)
                        old_delta=source_delta_at(cells,bx,by,sit)
                        is_default=[zindex,bindex]==DEFAULT_COORD
                        if sitindex==0 or is_default:
                            # Full original float32 delta bytes retained for
                            # every Sit0 and the complete default Z/Breath slice.
                            delta=old_delta.copy()
                            local=(base+delta).astype(np.float32).astype(float)
                            if is_default:default_checks.append({'coordinate':list(coordinate),'sit':sit,'original_R5_key':sit in SOURCE_MESH_KEYS,
                                                                 'same_f32_delta_bytes':delta.tobytes()==old_delta.tobytes(),
                                                                 'sha256':byte_hash(delta)})
                        else:
                            R=rotation(angle(sit,side))
                            if kind=='shoe':target=original.copy()
                            elif kind=='stocking':target=(original-pivot)@R.T+pivot
                            elif kind=='knee':
                                default_local=(base+old_delta).astype(np.float32).astype(float)
                                default_world=world_positions(node_parent,default_local,params(bx,by,sit),True)
                                joint_default=(default_cuff-default_pivot)@R.T+default_pivot
                                joint=(cuff-pivot)@R.T+pivot
                                target=default_world-joint_default+joint
                            else:
                                default_local=(base+old_delta).astype(np.float32).astype(float)
                                default_world=world_positions(node_parent,default_local,params(bx,by,sit),True)
                                target=default_world-original_default+original
                                # The old hidden plate still supplies runtime's
                                # floor anchor. Protect its whole original shoe.
                                rigid=original_default[:,1]>=1120
                                target[rigid]=original[rigid]
                            wanted,solve=local_inverse(node_parent,target,params(bx,by,sit,z,breath))
                            minimum_jacobian=min(minimum_jacobian,solve['minimum_evaluated_jacobian'])
                            maximum_inverse_residual=max(maximum_inverse_residual,solve['max_residual_source_px'])
                            delta=s.f32(wanted-base)
                            local=(base+delta).astype(np.float32).astype(float)
                            actual=world_positions(node_parent,local,params(bx,by,sit,z,breath),True)
                            maximum_target_residual=max(maximum_target_residual,float(abs(actual-target).max()))
                        local_forms.append(local)
                        if not old or sitindex!=0:
                            keyforms.append({'coordinate':list(coordinate),'position_deltas':delta.reshape(-1).tolist()})
                    interval=interval_proof(local_forms,indices,keys=MESH_KEYS)
                    if not interval['all_triangles_strictly_same_sign']:raise ValueError((identifier,bx,by,z,breath,interval['failures']))
                    proofs.append({'body_x':parent.BODY_KEYS[bx],'body_y':parent.BODY_KEYS[by],'body_z':z,'breath':breath,
                                   'minimum_area_ratio':interval['minimum_area_ratio'],'intervals':interval['intervals']})
                    all_forms[(bx,by,zindex,bindex)]=local_forms
    if old:
        record['append_axes']=record['append_axes']+ADDITIONAL_AXES
        sit_axis=next(axis for axis in record['replace_axes'] if axis['parameter']=='ParamSitPose')
        source_keys=node['getGeometryGrid']['getAxes'][0]['getKeys']
        sit_axis['keys']=MESH_KEYS
        sit_axis['source_key_indices']=[min(range(len(source_keys)),key=lambda i:abs(source_keys[i]-sit)) for sit in MESH_KEYS]
    else:
        record['axes']=record['axes']+ADDITIONAL_AXES
        next(axis for axis in record['axes'] if axis['parameter']=='ParamSitPose')['keys']=MESH_KEYS
    record['keyforms']=keyforms
    return {'id':identifier,'kind':kind,'parent':node_parent,'target_contexts':81,'new_keyform_overrides':len(keyforms),
            'Sit0_delta_preservation':'Original source Sit0 cloned for old mesh; original R5 Sit0 delta copied bit-for-bit for every new Z/Breath context.',
            'default_Z_Breath_slice_all_delta_bits_preserved':all(c['same_f32_delta_bytes'] for c in default_checks),
            'default_slice_delta_hashes':default_checks,'minimum_local_interval_area_ratio':min(c['minimum_area_ratio'] for c in proofs),
            'all_interval_area_polynomials_same_original_sign':True,'per_context_intervals':proofs,
            'maximum_generic_inverse_residual_source_px':maximum_inverse_residual,
            'maximum_f32_key_target_residual_source_px':maximum_target_residual,
            'minimum_evaluated_parent_jacobian_during_inverse':minimum_jacobian}


def main():
    global MESH_KEYS
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision',choices=['r6','r7'],default='r7')
    parser.add_argument('--output-prefix',help='Isolated filename prefix; existing evidence is never overwritten.')
    args=parser.parse_args()
    MESH_KEYS=list(SOURCE_MESH_KEYS) if args.revision=='r6' else sorted(SOURCE_MESH_KEYS+[.5375,.6125])
    prefix=args.output_prefix or 'full-ancestor-legs-'+args.revision
    plan_path=OUT/(prefix+'-plan.json');proof_path=OUT/(prefix+'-proof.json')
    if plan_path.exists() or proof_path.exists():raise ValueError('Use a fresh output prefix; previous candidate/proof must remain unchanged.')
    plan=s.read(INPUT);input_pin=hashlib.sha256(INPUT.read_bytes()).hexdigest()
    body=next(x for x in plan['deformer_grids'] if x['id']=='DeformBodyXY')
    if any(not np.array_equal(s.f32(c['control_points']),s.f32(parent.BODY_EDITS[tuple(c['coordinate'])].reshape(-1))) for c in body['keyforms']):
        raise ValueError('Root BodyXY edits differ from the verified parent-world input.')
    records=[]
    for edit in plan['mesh_grids']:
        if edit['id'] in ['ArtMeshFootwearL','ArtMeshFootwearR']:records.append(retarget(edit['id'],edit,True))
    for child in plan['insert_meshes']:
        if child['id'].startswith(('ArtMeshSitKneeFront','ArtMeshSitStocking','ArtMeshSitShoe')):
            records.append(retarget(child['id'],child))
    if len(records)!=8:raise ValueError('Expected only two old floor plates plus six visible continuous-leg children.')
    plan['authoredSitPose']['fullAncestorLegCompensation']={'version':1,'bodyZKeys':BODY_Z_KEYS,'breathKeys':BREATH_KEYS,
        'contextTarget':'Same complete BodyXY/BodyZ/Breath formal Sit0 shoe world geometry; rigid stocking uses its own same-context source length.',
        'defaultZBreathSlicePreserved':True,'Sit0Preserved':True,'parameterClamping':False}
    plan['authoredSitPose']['geometrySupportKeys']=MESH_KEYS
    if hashlib.sha256(INPUT.read_bytes()).hexdigest()!=input_pin:raise ValueError('Root input plan changed during generation.')
    report={'schema_version':1,'kind':'full_ancestor_leg_public_bilinear_inverse_geometry','adopted':False,
            'source':plan['source'],'input_plan':str(INPUT),'input_plan_sha256':input_pin,'body_z_keys':BODY_Z_KEYS,'breath_keys':BREATH_KEYS,
            'default_coordinate':DEFAULT_COORD,'sit_support_keys':MESH_KEYS,'expanded_mesh_ids':[r['id'] for r in records],
            'revision':args.revision,'original_support_keys':SOURCE_MESH_KEYS,
            'added_support_keys':[key for key in MESH_KEYS if key not in SOURCE_MESH_KEYS],
            'default_slice_new_keys':'Float32 interpolation of original R5 local deltas; every original R5 key retains its original float32 delta bytes.',
            'grid_cells_per_expanded_mesh':len(MESH_KEYS)*81,'total_grid_cells':len(MESH_KEYS)*81*8,'records':records,
            'not_expanded':['hidden Objects5/6','ordinary front/rear skirt','palms and arm bridge'],
            'UV_texture_pages_channels_parts_and_parameters_changed':False,
            'warp_contract':{'inside':'Quad/bilinear interpolation in actual CP cells.',
                             'outside':'Public warpExtrap two-unit perimeter triangulation; far outside [-2,3] uses the corner-derived affine frame.',
                             'ancestor_control_points':'Recursively transformed by the same public inside/outside contract.',
                             'source_public_API_evidence':str(OUT.parent/'api/multipage-v2/DeformTransforms-bytecode.txt'),
                             'frozen_R6_worst_Core_agreement_source_px':.00011734433201127104,
                             'frozen_R6_old_edge_extrap_error_source_px':1.022522819192318,
                             'agreement_context':{'ParamBodyAngleX':10,'ParamBodyAngleY':-10,'ParamBodyAngleZ':10,'ParamBreath':1,'ParamSitPose':.65},
                             'agreement_drawable':'ArtMeshSitStockingL','tested_vertices':67},
            'required_next_gate':'Independent compiled Core full BodyXY x BodyZ x Breath x Sit sampled combinations, whole-shoe XY/minY, same-context calf length and triangle orientation; Native/alpha/visual acceptance remains separate.'}
    plan_path.write_text(json.dumps(plan,indent=2,allow_nan=False)+'\n',encoding='utf8')
    proof_path.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':str(plan_path),'bytes':plan_path.stat().st_size,
         'meshes':[{'id':r['id'],'forms':r['new_keyform_overrides'],'min_area_ratio':r['minimum_local_interval_area_ratio'],
                   'default_slice_preserved':r['default_Z_Breath_slice_all_delta_bits_preserved'],'inverse_error_px':r['maximum_generic_inverse_residual_source_px'],
                   'float32_error_px':r['maximum_f32_key_target_residual_source_px']} for r in records]},indent=2))


if __name__=='__main__':main()
