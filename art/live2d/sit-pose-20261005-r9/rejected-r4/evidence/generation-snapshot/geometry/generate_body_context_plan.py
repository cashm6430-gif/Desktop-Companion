"""Actual typed, context-aware grid edits; isolated authoring candidate only."""
from pathlib import Path
import json
import numpy as np
import body_context_geometry as m

s=m.source
OUT=Path(__file__).resolve().parent
MESH_KEYS=[0.0,.0875,.175,.2625,.35,.425,.5,.575,.65,1.0]
BODY_AXES=[{'parameter':'ParamBodyAngleX','keys':m.BODY_KEYS},
           {'parameter':'ParamBodyAngleY','keys':m.BODY_KEYS}]


def interval_proof(forms,triangles,keys=MESH_KEYS):
    previous=s.KEYS
    try:
        s.KEYS=keys
        return s.all_interval_areas(forms,triangles)
    finally:s.KEYS=previous


def anchor(node_id):
    p=s.read(OUT/'candidate-geometry-proof.json')['footwear'][node_id]
    return (p.get('fixed_heel_vertices',p.get('heel_vertices')),
            np.asarray(p.get('fixed_heel_weights',p.get('heel_barycentric_weights'))))


def edits_for(identifier):
    node=s.D[identifier];base=s.f32(node['getMesh']['getPositions']).reshape(-1,2)
    is_busy=identifier.startswith('ArtMeshObjects')
    side={'ArtMeshFootwearL':'L','ArtMeshFootwearR':'R','ArtMeshObjects5':'L','ArtMeshObjects6':'R'}[identifier]
    foot=s.D['ArtMeshFootwear'+side]
    triangles=np.asarray(node['getMesh']['getIndices']).reshape(-1,3)
    heel_indices,heel_weights=anchor(identifier)
    target_indices,target_weights=anchor('ArtMeshFootwear'+side)
    keyforms=[];proofs=[];saved={};forward_residual=0
    for bx in range(3):
        for by in range(3):
            params=m.context(bx,by)
            neutral=m.mesh_world(node,params)
            target_zero=m.mesh_world(foot,params)
            target_heel=target_weights@target_zero[target_indices]
            current_heel=heel_weights@neutral[heel_indices]
            forms=[s.absolute_form(node,s.neutral_cell(node)).astype(float)]
            for index,sit in enumerate(MESH_KEYS[1:],1):
                params=m.context(bx,by,sit)
                world=(neutral+target_heel-current_heel if is_busy
                       else s.leg_world(neutral,side,*m.key_settings(sit)))
                local=m.local_from_world(node,params,world)
                delta=s.f32(local-base).reshape(-1)
                actual=(base.reshape(-1)+delta).astype(np.float32).reshape(-1,2).astype(float)
                forms.append(actual)
                forward_residual=max(forward_residual,float(abs(m.mesh_world(node,params,True,actual)-world).max()))
                for busy_index in ([0,1] if is_busy else [None]):
                    coord=([index,busy_index,bx,by] if is_busy else [index,bx,by])
                    keyforms.append({'coordinate':coord,'position_deltas':delta.tolist()})
            p=interval_proof(forms,triangles)
            p.update({'body_x':m.BODY_KEYS[bx],'body_y':m.BODY_KEYS[by]})
            assert p['all_triangles_strictly_same_sign'],p
            proofs.append(p);saved[(bx,by)]=forms
    edit={'id':identifier,
          'replace_axes':[{'parameter':'ParamSitPose','keys':MESH_KEYS,
                           'source_key_indices':[min(range(len(node['getGeometryGrid']['getAxes'][0]['getKeys'])),
                                                    key=lambda i:abs(node['getGeometryGrid']['getAxes'][0]['getKeys'][i]-key))
                                                 for key in MESH_KEYS]}],
          'append_axes':BODY_AXES,'keyforms':keyforms}
    return edit,{'intervals':proofs,'minimum_area_ratio':min(p['minimum_area_ratio'] for p in proofs),
                 'maximum_key_forward_residual_source_px':forward_residual,
                 'heel_vertices':heel_indices,'heel_weights':heel_weights.tolist(),
                 'standing_target_id':'ArtMeshFootwear'+side},saved


def mesh_local_at(saved,bx,by,sit):
    return sum(ws*wx*wy*saved[(ix,iy)][is_]
               for is_,ws in m.weights(MESH_KEYS,sit)
               for ix,wx in m.weights(m.BODY_KEYS,bx)
               for iy,wy in m.weights(m.BODY_KEYS,by))


def main():
    mesh_edits=[];proof={};saved={}
    ids=['ArtMeshFootwearL','ArtMeshFootwearR','ArtMeshObjects5','ArtMeshObjects6']
    for identifier in ids:
        edit,p,forms=edits_for(identifier);mesh_edits.append(edit);proof[identifier]=p;saved[identifier]=forms
    # A dense helper check is distinct from the later independent compiled Core
    # gate. It includes intermediate BodyXY values, not just the nine grid keys.
    maxima={identifier:{'error_source_px':0.0} for identifier in ids}
    for bx in np.linspace(-10,10,9):
        for by in np.linspace(-10,10,9):
            original={id:m.mesh_world(s.D[id],{'ParamBodyAngleX':float(bx),'ParamBodyAngleY':float(by)}) for id in ids[:2]}
            targets={id:np.asarray(proof[id]['heel_weights'])@original[id][proof[id]['heel_vertices']] for id in ids[:2]}
            for sit in np.linspace(0,1,201):
                params={'ParamBodyAngleX':float(bx),'ParamBodyAngleY':float(by),'ParamSitPose':float(sit)}
                for identifier in ids:
                    if identifier.startswith('ArtMeshObjects') and sit<.35:continue
                    p=proof[identifier]
                    world=m.mesh_world(s.D[identifier],params,True,mesh_local_at(saved[identifier],bx,by,sit))
                    actual=np.asarray(p['heel_weights'])@world[p['heel_vertices']]
                    target=targets[p['standing_target_id']]
                    error=float(np.linalg.norm(actual-target))
                    if error>maxima[identifier]['error_source_px']:
                        maxima[identifier]={'error_source_px':error,'body_x':float(bx),'body_y':float(by),
                                           'sit':float(sit),'actual_source_xy':actual.tolist(),'target_source_xy':target.tolist()}
    parent_bounds=[]
    for bx in range(3):
        for by in range(3):
            for sit in s.KEYS:
                resolver=m.world_lattices(m.context(bx,by,sit),True)
                for identifier in ['DeformPair_Footwear','DeformBodyZBreath']:
                    parent_bounds.append({'id':identifier,'body_x':m.BODY_KEYS[bx],'body_y':m.BODY_KEYS[by],
                                          'sit':sit,'minimum_jacobian_source_area_per_local_area':m.local_jacobian_bound(s.W[identifier],resolver(identifier))})
    assert all(p['error_source_px']<.05 for p in maxima.values()),maxima
    assert min(p['minimum_jacobian_source_area_per_local_area'] for p in parent_bounds)>0
    metadata=dict(m.BASE_PLAN['authoredSitPose'])
    metadata.update({'version':2,'approvedKeys':s.KEYS,'geometrySupportKeys':MESH_KEYS,
                     'contactReference':'Same BodyXY context, formal Sit0 standing footwear sole XY.',
                     'continuousLegMaterials':'PartExtra new front-knee and stocking/shoe child meshes; old seated leg plates must be hidden for Sit>=.35.'})
    plan=dict(m.BASE_PLAN)
    plan.update({'mesh_grids':mesh_edits,'authoredSitPose':metadata})
    report={'schema_version':2,'kind':'context_aware_public_grid_inverse_geometry',
            'source':plan.get('source'),'approved_keys':s.KEYS,'geometry_support_keys':MESH_KEYS,
            'parent_evaluation':'Resolve each child lattice through its already-resolved parent world; then interpolate each mesh through its immediate resolved parent lattice. Never compose unsampled nested warp functions.',
            'local_area_intervals':proof,'public_parent_jacobians':parent_bounds,
            'helper_dense_contact_scope':{'body_x_y_keys':np.linspace(-10,10,9).tolist(),'sit_keys':201,
                                          'busy_contact_required_from':.35,'core':False},
            'helper_dense_maximum_contacts':maxima,
            'compiled_core_gate_required':True,'adopted':False}
    (OUT/'body-context-plan-r2.json').write_text(json.dumps(plan,indent=2,allow_nan=False)+'\n',encoding='utf8')
    (OUT/'body-context-proof-r2.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':'body-context-plan-r2.json','mesh_form_counts':{e['id']:len(e['keyforms']) for e in mesh_edits},
                      'minimum_area_ratios':{id:p['minimum_area_ratio'] for id,p in proof.items()},'helper_contacts':maxima},indent=2))


if __name__=='__main__':main()
