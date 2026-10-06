"""Public PSD2Live Rotation/Warp parent evaluator, independently Core checked.

Extends BodyRig without modifying it. A Rotation child uses origin-centred
local coordinates: public rotationXform maps local (0,0) to parent-mapped
origin. Parent angular inheritance uses buildRotationWorld's negative-Y
probe, and WarpWorld accY inherits the parent value (not a bbox scale).
This file authors no keyform, CP, raster, model or runtime geometry.
"""
from pathlib import Path
from dataclasses import dataclass
import argparse,itertools,json,math,sys
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
DESIGN=ROOT/'.local/authoring/sit-pose-20261004-r1/geometry/r9-source-design'
sys.path[:0]=[str(DESIGN),str(ROOT/'tools')]
from body_crouch_r9 import BodyRig,SOURCE,GRAPH,weights,read
from full_context_geometry import warp_apply_jacobian,generic_warp_inverse
from verify_component_coverage import CoverageModel
from probe_cubism_core import digest

HAIR_GRAPH=ROOT/'.local/authoring/sit-pose-20261004-r1/api/public-readback-v1/readback-20261005-r3-static-hair/reports/saved-CMO-public-graph.json'
PI=np.float32(3.1415927)
TAU=np.float32(6.2831855)
f32=np.float32


@dataclass(frozen=True)
class RotationWorld:
    matrix: np.ndarray
    origin: np.ndarray
    acc_y: float
    angle_degrees: float

    def apply(self,points):
        p=np.asarray(points,np.float32).reshape(-1,2)
        a=np.asarray(self.matrix,np.float32);o=np.asarray(self.origin,np.float32)
        # Same multiplication/addition order as RotationXform.apply bytecode.
        x=f32(f32(a[0,1]*p[:,1])+f32(a[0,0]*p[:,0]));x=f32(x+o[0])
        y=f32(f32(p[:,1]*a[1,1])+f32(p[:,0]*a[1,0]));y=f32(y+o[1])
        return np.column_stack((x,y)).astype(float)


class HeadParentRig(BodyRig):
    def __init__(self):
        super().__init__()
        # These constant hair cells were explicitly omitted by the earlier
        # compact graph. Read them from a second actual saved-CMO readback.
        supplement=read(HAIR_GRAPH)
        receipt=read(HAIR_GRAPH.with_name('public-graph-receipt.json'))
        if receipt['input_sha256'].get(str(SOURCE/'edited.cmo3'))!=digest(SOURCE/'edited.cmo3'):
            raise ValueError('Hair supplement is not bound to the actual eight-page source CMO')
        if receipt['public_graph_sha256']!=digest(HAIR_GRAPH) or not receipt['reference_native_baseline_byte_exact']:
            raise ValueError('Hair public readback provenance invalid')
        for node in supplement['getDrawables']:
            if node['getId-bteaJEs'] in ('ArtMeshBackHair2','ArtMeshBackHair'):
                if node['getGeometryGrid'].get('_omitted_cells'):raise ValueError('Actual hair geometry is still omitted')
                self.D[node['getId-bteaJEs']]=node
        self.rotation_forms={}
        for name,node in self.W.items():
            if 'getColumns' in node:continue
            if not node['type'].endswith('$Rotation') or node.get('getBlendShapes'):
                raise ValueError('Unsupported non-Warp or Rotation blendshape '+name)
            self.rotation_forms[name]={tuple(cell['getCoordinate']):cell['getForm'] for cell in node['getGeometryGrid']['getCells']}

    def _rotation_form(self,name,parameters):
        node=self.W[name];selected=[([],f32(1.))]
        for axis in node['getGeometryGrid']['getAxes']:
            parameter=axis['getParameterId-WD9NFvw'];value=parameters.get(parameter,self.default[parameter])
            selected=[(coord+[index],f32(w*f32(coef))) for coord,w in selected for index,coef in weights(axis['getKeys'],value)]
        fields=['getOriginX','getOriginY','getScale','getAngle'];v=np.zeros(4,np.float32)
        for coord,w in selected:
            form=self.rotation_forms[name][tuple(coord)]
            v=f32(v+f32(w*np.asarray([form[key] for key in fields],np.float32)))
        return v

    def _flip(self,node,channel,parameters,default):
        grid=node['getChannelGrids']['getGridsByChannel'].get(channel)
        if grid is None:return bool(default)
        selected=[([],1.)]
        for axis in grid['getAxes']:
            parameter=axis['getParameterId-WD9NFvw'];selected=[(coord+[i],w*coef) for coord,w in selected for i,coef in weights(axis['getKeys'],parameters.get(parameter,self.default[parameter]))]
        cells={tuple(c['getCoordinate']):c['getForm']['getValue'] for c in grid['getCells']}
        return sum(w*cells[tuple(coord)] for coord,w in selected)>=.5

    def _apply(self,name,world,points):
        if isinstance(world,RotationWorld):return world.apply(points)
        return warp_apply_jacobian(self.W[name],world,np.asarray(points,np.float32))[0].astype(np.float32).astype(float)

    def worlds(self,parameters,edited=False):
        cache={};acc_y={};visiting=set()
        def resolve(name):
            if name in cache:return cache[name]
            if name in visiting:raise ValueError('Cyclic deformer parents '+name)
            visiting.add(name);node=self.W[name];parent=node['getParent-lmpY1tE']
            parent_world=resolve(parent) if parent else None
            inherited=f32(acc_y[parent] if parent else 1.)
            if 'getColumns' in node:
                cp=np.asarray(self.cp(name,parameters,edited),np.float32).astype(float)
                if parent:cp=self._apply(parent,parent_world,cp)
                cache[name]=cp;acc_y[name]=inherited
            else:
                ox,oy,scale,angle=self._rotation_form(name,parameters)
                angle=f32(f32(node['getBaseAngle'])+angle);scale=f32(inherited*scale)
                if parent:
                    origin=self._apply(parent,parent_world,[[ox,oy]])[0].astype(np.float32)
                    delta=f32(-10. if isinstance(parent_world,RotationWorld) else -.1)
                    step=f32(1.);difference=np.zeros(2,np.float32)
                    for _ in range(10):
                        probe=self._apply(parent,parent_world,[[ox,f32(oy+f32(delta*step))]])[0].astype(np.float32)
                        difference=f32(probe-origin)
                        if np.any(difference!=0):break
                        probe=self._apply(parent,parent_world,[[ox,f32(oy-f32(delta*step))]])[0].astype(np.float32)
                        difference=f32(origin-probe)
                        if np.any(difference!=0):break
                        step=f32(step*f32(.1))
                    dtheta=f32(f32(math.atan2(float(delta),0.))-f32(math.atan2(float(difference[1]),float(difference[0]))))
                    while dtheta>PI:dtheta=f32(dtheta-TAU)
                    while dtheta<-PI:dtheta=f32(dtheta+TAU)
                    angle=f32(angle-f32(f32(dtheta*f32(180.))/PI))
                else:origin=np.asarray([ox,oy],np.float32)
                rad=f32(f32(angle*PI)/f32(180.));si=f32(math.sin(float(rad)));co=f32(math.cos(float(rad)))
                fx=f32(-1. if self._flip(node,'FLIP_X',parameters,node['getFlipX']) else 1.)
                fy=f32(-1. if self._flip(node,'FLIP_Y',parameters,node['getFlipY']) else 1.)
                c12=f32(f32(co*scale)*fx);c13=f32(f32(fy*co)*scale)
                c14=f32(f32(fx*scale)*si);c15=f32(f32(-si*scale)*fy)
                cache[name]=RotationWorld(np.asarray([[c12,c15],[c14,c13]],np.float32),origin,float(scale),float(angle))
                acc_y[name]=scale
            visiting.remove(name);return cache[name]
        return resolve

    def points(self,parent,local,parameters,edited=False):
        if not parent:return np.asarray(local,float).reshape(-1,2)
        return self._apply(parent,self.worlds(parameters,edited)(parent),local)

    def inverse(self,parent,target,parameters,edited=False):
        world=self.worlds(parameters,edited)(parent);target=np.asarray(target,float).reshape(-1,2)
        if isinstance(world,RotationWorld):
            matrix=np.asarray(world.matrix,float);det=float(np.linalg.det(matrix))
            if not np.isfinite(det) or abs(det)<1e-12:raise ValueError('Singular public Rotation '+parent)
            local=(target-np.asarray(world.origin,float))@np.linalg.inv(matrix).T
            residual=np.linalg.norm(world.apply(local)-target,axis=1)
            return local,{'method':'public_rotation_affine_inverse','minimum_evaluated_jacobian':det,
                          'maximum_forward_residual_source_px':float(residual.max())}
        return generic_warp_inverse(self.W[parent],world,target)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);args=p.parse_args();out=args.output.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'.local'):raise ValueError('Fresh .local report required')
    rig=HeadParentRig();runtime=CoverageModel(SOURCE/'edited.model3.json');meshes=['ArtMeshBackHair2','ArtMeshBackHair','ArtMeshFace']
    headkeys=[]
    for param in ['ParamAngleX','ParamAngleY','ParamAngleZ']:
        entry=next(a for n in rig.W.values() for a in n['getGeometryGrid']['getAxes'] if a['getParameterId-WD9NFvw']==param)
        headkeys.append(entry['getKeys'])
    pins={str(path):digest(path) for path in [Path(__file__),GRAPH,HAIR_GRAPH,SOURCE/'edited.cmo3',SOURCE/'edited.moc3',SOURCE/'edited.model3.json',
        ROOT/'.local/authoring/sit-pose-20261004-r1/api/multipage-v2/DeformTransforms-bytecode.txt',HERE/'deformer-cascade-bytecode.txt',HERE/'deformer-cascade-full-bytecode.txt']}
    rows=[];worst=None;inverse_max=0.
    for sit,angles,hair in itertools.product([0.,.35,1.],itertools.product(*headkeys),[-1.,0.,1.]):
        parameters={'ParamSitPose':sit,'ParamHairBack':hair,**dict(zip(['ParamAngleX','ParamAngleY','ParamAngleZ'],angles))}
        runtime.update({'parameters':parameters})
        for name in meshes:
            node=rig.D[name];local=rig.mesh_local(name,parameters);predicted=rig.points(node['getParentDeformerId-lmpY1tE'],local,parameters)
            actual=runtime.source_xy(runtime.positions[name]);residual=np.linalg.norm(predicted-actual,axis=1);i=int(residual.argmax())
            row={'parameters':parameters,'drawable':name,'vertices':len(local),'maximum_residual_source_px':float(residual[i]),'worst_vertex':i,
                 'predicted_world_xy':predicted[i].tolist(),'actual_Core_world_xy':actual[i].tolist(),'passed':bool(residual[i]<.001)}
            rows.append(row)
            if worst is None or row['maximum_residual_source_px']>worst['maximum_residual_source_px']:worst=row
        # Exact affine parent inverse is independently exercised on the same
        # real parent-origin/local CP points, not a guessed rotation domain.
        world=rig.worlds(parameters)('DeformHeadRotation');points=np.asarray([[-250.,-250.],[0.,0.],[250.,250.]])
        target=world.apply(points);recovered,proof=rig.inverse('DeformHeadRotation',target,parameters)
        inverse_max=max(inverse_max,float(np.linalg.norm(recovered-points,axis=1).max()))
    if any(digest(Path(path))!=signature for path,signature in pins.items()):raise ValueError('Source changed during evaluator check')
    passed=all(r['passed'] for r in rows)
    report={'schema_version':1,'kind':'actual_saved_eight_page_public_Rotation_Warp_parent_geometry','passed':passed,'usable_within_checked_scope':passed,
            'source_pins':pins,'pose_count':243,'mesh_checks':len(rows),'tested_meshes':meshes,'head_angle_keys':headkeys,'Sit_keys':[0.,.35,1.],'HairBack_keys':[-1.,0.,1.],
            'body_parameters':'Actual Native defaults; no physics','maximum_allowed_world_error_source_px':.001,'worst':worst,'rows':rows,
            'rotation_parent_inverse_maximum_local_error':inverse_max,
            'algorithm_contract':{'rotation_origin':'local(0,0) maps to parent-applied interpolated origin','angle':'base+keyAngle−parent negative-Y-probe angle delta',
                'scale':'parent.accY*keyScale; Warp accY inherits rather than guessing bbox scale','warp':'map CP through complete immediate parent first; apply the resulting world lattice, including public perimeter extrapolation'},
            'public_APIs':['DeformerCascadeKt.buildRotationWorld','DeformTransformsKt.rotationXform','RotationXform.apply','DeformerWorld.apply'],
            'models_modified':False,'keyforms_designed':False,'approval':{'visual':'not_performed','adoption':'not_requested'},
            'limits':['Source eight-page graph only; no claim of the R9 edited body/head/cloth family.','Neutral Body only; other Body/Shift and interpolated angles need additional actual Core checks.',
                      'This supplies parent math for future HairBackFollow authoring; no floor-safe hair CP exists yet.']}
    out.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'report':str(out),'passed':passed,'worst':worst,'inverse_local_error':inverse_max},indent=2))
    if not passed:raise SystemExit(1)

if __name__=='__main__':main()
