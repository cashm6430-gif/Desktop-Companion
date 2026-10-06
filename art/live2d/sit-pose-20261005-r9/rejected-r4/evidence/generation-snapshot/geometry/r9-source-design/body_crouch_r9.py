"""R9 source-bound body CP evaluator; actual saved-CMO graph, no old W tables."""
from pathlib import Path
import json
import sys
import itertools
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
GEOMETRY=HERE.parent
sys.path[:0]=[str(GEOMETRY),str(ROOT/'tools')]
from full_context_geometry import warp_apply_jacobian,generic_warp_inverse
from probe_cubism_core import digest
from verify_component_coverage import CoverageModel

G=ROOT/'.local/authoring/sit-pose-20261004-r1'
GRAPH=G/'api/public-readback-v1/readback-20261005-r2-zero-slices/reports/saved-CMO-public-graph.json'
SOURCE=G/'api/standing-restore-v1/probe-20261005T065840Z-77b66c6f/export'
PARAMETERS=['ParamBodyAngleX','ParamBodyAngleY','ParamBodyAngleZ','ParamBreath']


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def weights(keys,value):
    if value<=keys[0]:return [(0,1.)]
    if value>=keys[-1]:return [(len(keys)-1,1.)]
    upper=int(np.searchsorted(keys,value));t=(value-keys[upper-1])/(keys[upper]-keys[upper-1])
    return [(upper-1,1-t),(upper,t)]


class BodyRig:
    def __init__(self):
        self.graph=read(GRAPH)
        self.W={w['getId-u_t8HeU']:w for w in self.graph['getDeformers']}
        self.D={d['getId-bteaJEs']:d for d in self.graph['getDrawables']}
        self.default={p['getId-WD9NFvw']:p['getDefault'] for p in self.graph['getParameters']}
        self.axes={};self.CP={}
        for name,node in self.W.items():
            if 'getColumns' not in node:continue
            grid=node['getGeometryGrid'];self.axes[name]=[(a['getParameterId-WD9NFvw'],a['getKeys']) for a in grid['getAxes']]
            self.CP[name]={tuple(c['getCoordinate']):np.asarray(c['getForm']['getControlPoints']).reshape(-1,2) for c in grid['getCells']}
        self.deep_keys=[0.,.35,.65,.8,.92,1.]
        self.deep_axes=[(name,self.deep_keys if name=='ParamSitPose' else keys) for name,keys in self.axes['DeformBodyXY']]
        self.edits={(bx,by,i):self.CP['DeformBodyXY'][(bx,by,old)].copy()
                    for bx,by,i in itertools.product(range(3),range(3),range(6))
                    for old in [[0,1,2,2,3,3][i]]}
        self.body_keys={name:keys for name,keys in self.axes['DeformBodyXY']}
        zb={name:keys for name,keys in self.axes['DeformBodyZBreath']}
        self.context_keys=[self.body_keys['ParamBodyAngleX'],self.body_keys['ParamBodyAngleY'],zb['ParamBodyAngleZ'],zb['ParamBreath']]
        self.deep_head_drop=None

    def cp(self,name,parameters,edited):
        cells=self.edits if edited and name=='DeformBodyXY' else self.CP[name]
        selected=[([],1.)]
        axes=self.deep_axes if edited and name=='DeformBodyXY' else self.axes[name]
        for parameter,keys in axes:
            selected=[(coordinate+[index],w*coefficient) for coordinate,w in selected
                      for index,coefficient in weights(keys,parameters.get(parameter,self.default[parameter]))]
        return sum(w*cells[tuple(c)] for c,w in selected)

    def worlds(self,parameters,edited=False):
        cache={}
        def resolve(name):
            if name in cache:return cache[name]
            node=self.W[name]
            if 'getColumns' not in node:raise ValueError('Rotation must use its public affine evaluator; do not pretend a warp.')
            cp=self.cp(name,parameters,edited);parent=node['getParent-lmpY1tE']
            if parent:cp=warp_apply_jacobian(self.W[parent],resolve(parent),cp)[0]
            cache[name]=cp;return cp
        return resolve

    def points(self,parent,local,parameters,edited=False):
        return warp_apply_jacobian(self.W[parent],self.worlds(parameters,edited)(parent),local)[0]

    def inverse(self,parent,target,parameters,edited=False):
        return generic_warp_inverse(self.W[parent],self.worlds(parameters,edited)(parent),target)

    def context(self,coordinate,sit=0):
        return {**{p:keys[c] for p,keys,c in zip(PARAMETERS,self.context_keys,coordinate)},'ParamSitPose':sit,
                'ParamArmLA':0.,'ParamArmRA':0.}

    def mesh_local(self,name,parameters):
        node=self.D[name];grid=node['getGeometryGrid']
        if grid.get('_omitted_cells') and not grid.get('getCells'):raise ValueError('Readback deliberately omitted this grid; never assume zero.')
        selected=[([],1.)]
        for axis in grid['getAxes']:
            p=axis['getParameterId-WD9NFvw'];selected=[(c+[i],w*coef) for c,w in selected for i,coef in weights(axis['getKeys'],parameters.get(p,self.default[p]))]
        cells={tuple(c['getCoordinate']):np.asarray(c['getForm']['getPositionDeltas']).reshape(-1,2) for c in grid['getCells']}
        base=np.asarray(node['getMesh']['getPositions'],np.float32).reshape(-1,2)
        delta=sum(w*cells[tuple(c)] for c,w in selected).astype(np.float32)
        return (base+delta).astype(np.float32).astype(float)

    def set_deep_profile(self,drop):
        """Move head rigidly, fold torso, retain a positive parent Jacobian.

        SourceY865 is a design projection guide, not a measured hip bone.
        All nondeep source CP cells, including complete Sit0, remain untouched.
        """
        self.deep_head_drop=float(drop)
        body=self.W['DeformBodyXY'];parent=body['getParent-lmpY1tE']
        neutral=self.worlds({'ParamSitPose':0})(body['getId-u_t8HeU'])
        reference_y=neutral.reshape(body['getRows']+1,body['getColumns']+1,2)[:,:,1]
        def profile(y):
            return np.where(y<=625,y+drop,np.interp(y,[625,865,1045,1254.329975],[625+drop,1030,1135,1254.329975]))
        displacement=profile(reference_y)-reference_y
        for bx,by in itertools.product(range(3),repeat=2):
            parameters={'ParamBodyAngleX':self.context_keys[0][bx],'ParamBodyAngleY':self.context_keys[1][by],'ParamSitPose':0}
            source_cp=self.CP['DeformBodyXY'][(bx,by,0)]
            source_world=warp_apply_jacobian(self.W[parent],self.worlds(parameters)(parent),source_cp)[0]
            target=source_world.copy().reshape(displacement.shape+(2,));target[:,:,1]+=displacement
            cp,proof=self.inverse(parent,target.reshape(-1,2),parameters)
            self.edits[(bx,by,2)]=cp.astype(np.float32).astype(float)
            self.edits[(bx,by,3)]=cp.astype(np.float32).astype(float)


def main():
    output=HERE/'r9-body-CP-and-contact-targets-v2.json'
    if output.exists():raise ValueError('Fresh source target output required.')
    original_tool_sha=digest(Path(__file__))
    rig=BodyRig();runtime=CoverageModel(SOURCE/'edited.model3.json')
    receipts=read(HERE/'source-material-anchors.json');arm=read(HERE/'arm-reach-feasibility.json')['joint_material_receipts']
    contexts=list(itertools.product(range(3),repeat=4));standing={};bone_local={};waist_local={};source_forward_max=0.
    for coordinate in contexts:
        p=rig.context(coordinate);runtime.update({'parameters':p})
        row={};locals_={}
        for side in ['L','R']:
            bones={name:runtime.source_xy(runtime.anchor({key:a[key] for key in ['drawable','vertices','weights']}))
                   for name,a in arm[side].items()}
            lengths=[np.linalg.norm(bones['elbow']-bones['shoulder']),np.linalg.norm(bones['wrist']-bones['elbow'])]
            shoulder=arm[side]['shoulder'];node=rig.D[shoulder['drawable']]
            local=rig.mesh_local(shoulder['drawable'],p);point=np.asarray(shoulder['weights'])@local[shoulder['vertices']]
            forward=rig.points(node['getParentDeformerId-lmpY1tE'],[point],p)[0]
            source_forward_error=float(np.linalg.norm(forward-bones['shoulder']));source_forward_max=max(source_forward_max,source_forward_error)
            if source_forward_error>.001:raise ValueError(('Actual current Core/public source shoulder differs.',coordinate,side,forward.tolist(),bones['shoulder'].tolist(),source_forward_error))
            row[side]={'bones':bones,'lengths':lengths};locals_[side]=(node['getParentDeformerId-lmpY1tE'],point)
        standing[coordinate]=row;bone_local[coordinate]=locals_
        waist_local[coordinate]={}
        for key,a in receipts['waist_cloth_anchors'].items():
            parent=rig.D[a['drawable']]['getParentDeformerId-lmpY1tE']
            point=runtime.source_xy(runtime.anchor({k:a[k] for k in ['drawable','vertices','weights']}))
            local,proof=rig.inverse(parent,[point],p)
            waist_local[coordinate][key]=(parent,local[0])
    trials=[]
    for drop in range(313,351):
        rig.set_deep_profile(drop);maximum_ratio=0.
        for coordinate in contexts:
            p=rig.context(coordinate,.65)
            for side in ['L','R']:
                parent,local=bone_local[coordinate][side];shoulder=rig.points(parent,[local],p,True)[0]
                wrist=np.asarray([shoulder[0]+(-24 if side=='L' else 23),1147.2])
                maximum_ratio=max(maximum_ratio,float(np.linalg.norm(wrist-shoulder)/sum(standing[coordinate][side]['lengths'])))
        trials.append({'head_drop_source_px':drop,'maximum_full_context_arm_reach_ratio':maximum_ratio})
        if maximum_ratio<=.985:break
    else:raise ValueError('No body profile reaches floor naturally within design search; request an altered contact gesture.')
    table=[]
    for coordinate in contexts:
        for sit in [0.,.35,.65,1.]:
            p=rig.context(coordinate,sit);row={'coordinate':list(coordinate),'parameters':p,'waist_real_cloth':{},'sides':{}}
            for key,a in receipts['waist_cloth_anchors'].items():
                parent,point=waist_local[coordinate][key]
                row['waist_real_cloth'][key]=rig.points(parent,[point],p,True)[0].tolist()
            for side in ['L','R']:
                parent,local=bone_local[coordinate][side];shoulder=rig.points(parent,[local],p,True)[0]
                L1,L2=standing[coordinate][side]['lengths']
                if sit==0:wrist=standing[coordinate][side]['bones']['wrist']
                elif sit==.35:wrist=np.array([shoulder[0]+(-24 if side=='L' else 23),879.])
                elif sit==.65:wrist=np.array([shoulder[0]+(-24 if side=='L' else 23),1147.2])
                else:
                    runtime.update({'parameters':{**p,'ParamBusyLaptop':1}})
                    receipt=read(G/'api/ground-handoff-r1/ground-handoff-proof.json')['busy_wrist_anchors'][side]
                    wrist=runtime.source_xy(runtime.anchor({key:receipt[key] for key in ['drawable','vertices','weights']}))
                d=wrist-shoulder;distance=float(np.linalg.norm(d));direction=d/distance;normal=np.array([-direction[1],direction[0]])
                a=(L1**2-L2**2+distance**2)/(2*distance);h=np.sqrt(max(0.,L1**2-a**2))
                elbow=shoulder+a*direction+(1 if side=='L' else -1)*h*normal
                row['sides'][side]={'shoulder':shoulder.tolist(),'elbow':elbow.tolist(),'wrist':wrist.tolist(),
                    'L1':float(L1),'L2':float(L2),'reach':distance,'reachable':bool(abs(L1-L2)<=distance<=L1+L2),
                    'actual_ik_segment_lengths':[float(np.linalg.norm(elbow-shoulder)),float(np.linalg.norm(wrist-elbow))],
                    'visible_shin_orientation':'world vertical at .35/.65; source length preserved; final slight inward from approved closed legs.'}
            table.append(row)
    pins={str(p):digest(p) for p in [GRAPH,SOURCE/'edited.cmo3',SOURCE/'edited.moc3',SOURCE/'edited.model3.json',HERE/'source-material-anchors.json',Path(__file__)]}
    payload={'kind':'R9_source_bound_body_crouch_and_short_arm_IK_targets','adopted':False,'source':pins,
        'deep_head_drop_source_px':rig.deep_head_drop,'reach_profile_search':trials,
        'actual_original_Core_public_shoulder_forward_max_residual_source_px':source_forward_max,
        'omitted_front_skirt_grid_policy':'Waist local material points are inverse-registered from actual Core same-context Sit0 barycentrics; no missing grid is assumed neutral.',
        'body_CP_overrides':{'id':'DeformBodyXY',
            'replace_axes':[{'parameter':'ParamSitPose','keys':rig.deep_keys,'source_key_indices':[0,1,2,2,3,3]}],
            'keyforms':[{'coordinate':list(c),'control_points':cp.reshape(-1).tolist()}
            for c,cp in rig.edits.items()]},
        'other_CP_Sit0_035_1_source_float32_bytes_unchanged':True,
        'body_projection_guide':{'source_y865_world_y1030':'Design torso/pelvis projection guide, not an existing measured anatomical hip.',
                                'head':'Parent upper rows SourceY<=625 translate rigidly; actual Core Face/cranium and all Angle combinations must verify.',
                                'hair':'Long hanging back hair needs separate floor-safe fold/tuck. Full rigid long hair is not approved.'},
        'cloth_surface_ownership':'Projected real cloth waist may be below knee crown: back waist connector/fold-under separate from visible front lap, no flipped whole-sheet interpolation.',
        'contexts':81,'key_targets':table,'required_next':'Compile BodyCP+leg inverse+new cloth+same-trajectory short-arm geometry; inspect3Nativekeys before dense gate.'}
    if digest(Path(__file__))!=original_tool_sha:raise ValueError('Body author tool changed during generation.')
    output.write_text(json.dumps(payload,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'output':str(output),'sha256':digest(output),'deep_head_drop':rig.deep_head_drop,'contexts':81,
        'neutral_keys':[r for r in table if r['coordinate']==[1,1,1,0]],
        'all_key_arm_reachable':all(v['reachable'] for r in table for v in r['sides'].values())},indent=2))


if __name__=='__main__':main()
