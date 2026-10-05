"""Check named material coverage at moving, barycentric joint fixtures.

Uses actual Cubism Core vertices/UVs/opacity (including Parts), PNG alpha and
Core clipping masks. Coverage from unrelated hair/props cannot satisfy a
required_drawable_group. This checks material availability, not visual quality.

Example:
  python tools/verify_component_coverage.py --model staged.model3.json \
    --fixture joints.json --output coverage.json

Fixture poses contain parameters/part_opacities and checks. A check has an
anchor {drawable, vertices:[i,j,k], weights:[a,b,c]}, offsets_source_pixels,
required_drawable_group, minimum_alpha and minimum_fraction. Optional
roi_offsets {x:[min,max],y:[min,max],step} generates a grid. Offsets are right/
down source pixels. A source_xy anchor is supported for fixed diagnostic ROIs.
Material metadata is optional and applies the actual main texture ownership
cuts. Independent runtime neck geometry and renderer-only transforms must be
checked by Native screenshots or supplied by a separate mapped-geometry gate.
An offset_frame.toward_anchor rotates ROI offsets with an actual joint: local
X is perpendicular and local Y points toward that anchor. A segment_samples
check samples the line to end_anchor, with transverse offsets in source pixels.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import itertools
import json
from pathlib import Path

import numpy as np
from PIL import Image

from probe_cubism_core import NativeModel, Vec2, digest, find_core, model_family


def sample_texture_alpha(alpha, uv):
    """GPU-equivalent bilinear alpha with clamp-to-edge; Native UV V is up."""
    alpha=np.asarray(alpha,dtype=float)
    uv=np.asarray(uv,dtype=float).reshape(-1,2)
    height,width=alpha.shape
    xy=np.column_stack((uv[:,0]*width-.5,(1-uv[:,1])*height-.5))
    xy=np.clip(xy,[0,0],[width-1,height-1])
    lo=np.floor(xy).astype(int);hi=np.minimum(lo+1,[width-1,height-1]);f=xy-lo
    x,y=lo.T;xx,yy=hi.T;u,v=f.T
    return (alpha[y,x]*(1-u)*(1-v)+alpha[y,xx]*u*(1-v)
            +alpha[yy,x]*(1-u)*v+alpha[yy,xx]*u*v)


def mesh_texture_alpha(points,positions,uvs,indices,alpha,double_sided=True):
    """Interpolate UV in actual triangles. A mesh rectangle is never coverage."""
    points=np.asarray(points,dtype=float).reshape(-1,2)
    positions=np.asarray(positions,dtype=float).reshape(-1,2)
    uvs=np.asarray(uvs,dtype=float).reshape(-1,2)
    result=np.zeros(len(points),dtype=float)
    for tri in np.asarray(indices,dtype=int).reshape(-1,3):
        a,b,c=positions[tri];e=b-a;f=c-a
        det=e[0]*f[1]-e[1]*f[0]
        if abs(det)<1e-18 or (not double_sided and det<0):continue
        d=points-a
        w1=(d[:,0]*f[1]-d[:,1]*f[0])/det
        w2=(e[0]*d[:,1]-e[1]*d[:,0])/det
        w0=1-w1-w2
        inside=(w0>=-1e-8)&(w1>=-1e-8)&(w2>=-1e-8)
        if not np.any(inside):continue
        uv=w0[inside,None]*uvs[tri[0]]+w1[inside,None]*uvs[tri[1]]+w2[inside,None]*uvs[tri[2]]
        result[inside]=np.maximum(result[inside],sample_texture_alpha(alpha,uv))
    return np.clip(result,0,1)


def alpha_union(values,count=None):
    values=list(values)
    if not values:return np.zeros(count or 0)
    return 1-np.prod(1-np.asarray(values),axis=0)


def clipped_alpha(raw,opacity,mask=None,inverted=False):
    if mask is None:mask=np.ones_like(raw)
    elif inverted:mask=1-mask
    return np.clip(np.asarray(raw)*float(opacity)*mask,0,1)


def load_alpha_pages(model,metadata=None):
    model=Path(model).resolve();document=json.loads(model.read_text(encoding='utf-8-sig'))
    paths=[(model.parent/path).resolve() for path in document['FileReferences'].get('Textures',[])]
    if not paths:raise ValueError('The model needs actual referenced PNG textures.')
    pages=[np.asarray(Image.open(path).convert('RGBA'))[:,:,3].astype(float)/255 for path in paths]
    pins={str(path):digest(path) for path in paths}
    if metadata:
        metadata=Path(metadata).resolve();data=json.loads(metadata.read_text(encoding='utf-8-sig'));pins[str(metadata)]=digest(metadata)
        separation=data.get('runtimeMaterialSeparation')
        if separation:
            ownership=(metadata.parent/separation['ownership']).resolve()
            info=json.loads(ownership.read_text(encoding='utf8'));pins[str(ownership)]=digest(ownership)
            index=separation['textureIndex'];alpha=pages[index]
            atlas=paths[index]
            moc=(model.parent/document['FileReferences']['Moc']).resolve()
            if digest(moc)!=info['moc_sha256']:raise ValueError('Ownership model hash is stale.')
            if digest(atlas)!=info['atlas_sha256']:raise ValueError('Ownership atlas hash is stale.')
            masks={}
            for field,hash_field in [('bodyMask','body_mask_sha256'),('duplicateMask','duplicate_mask_sha256'),('neckEraseMask','neck_erase_mask_sha256')]:
                if field not in separation:continue
                path=(metadata.parent/separation[field]).resolve();pins[str(path)]=digest(path)
                if pins[str(path)]!=info[hash_field]:raise ValueError('Ownership mask hash is stale: '+field)
                mask=np.asarray(Image.open(path).convert('L')).astype(float)/255
                if mask.shape!=alpha.shape:raise ValueError('Mask/atlas dimensions differ.')
                masks[field]=mask
            pages[index]=alpha*(1-np.maximum(masks['bodyMask'],masks['duplicateMask']))
            if 'neckEraseMask' in masks:pages[index]*=1-masks['neckEraseMask']
    return pages,pins


class CoverageModel:
    def __init__(self,model,core=None,metadata=None):
        self.model_path=Path(model).resolve();moc,_=model_family(self.model_path)
        self.pages,self.pins=load_alpha_pages(self.model_path,metadata)
        core_path=find_core(core)
        self.core=NativeModel(core_path,moc);self.pins[str(core_path)]=digest(core_path)
        self.pins[str(moc)]=digest(moc)
        self.pins[str(self.model_path)]=digest(self.model_path)
        self.lookup={name:i for i,name in enumerate(self.core.drawable_ids)}
        self.topology={};uvs=self.core.get('DrawableVertexUvs');indices=self.core.get('DrawableIndices')
        textures=self.core.get('DrawableTextureIndices');flags=self.core.get('DrawableConstantFlags')
        masks=self.core.get('DrawableMasks');counts=self.core.get('DrawableMaskCounts')
        for name,index in self.lookup.items():
            count=self.core.vertex_counts[index]
            uv=np.ctypeslib.as_array(uvs[index],shape=(count,)).view(np.float32).reshape(-1,2).copy()
            triangles=np.ctypeslib.as_array(indices[index],shape=(self.core.index_counts[index],)).copy().reshape(-1,3)
            texture=int(textures[index])
            if not 0<=texture<len(self.pages):raise ValueError('Missing texture page: '+name)
            self.topology[name]={'uvs':uv,'indices':triangles,'texture':texture,
                                 'double_sided':bool(flags[index]&4),'inverted_mask':bool(flags[index]&8),
                                 'masks':[self.core.drawable_ids[int(masks[index][j])] for j in range(counts[index])]}
        size,origin=Vec2(),Vec2();ppu=ctypes.c_float()
        self.core.api['ReadCanvasInfo'](self.core.model,ctypes.byref(size),ctypes.byref(origin),ctypes.byref(ppu))
        self.ppu=float(ppu.value);self.origin=np.asarray([origin.X,origin.Y],dtype=float)
        self.material_pages={}

    def add_material_mask(self,name,path):
        """Optional author-reviewed ownership mask for a mixed-material chart."""
        if name not in self.lookup:raise ValueError('Unknown masked material ID '+name)
        path=Path(path).resolve();mask=np.asarray(Image.open(path).convert('L')).astype(float)/255
        page=self.pages[self.topology[name]['texture']]
        if mask.shape!=page.shape:raise ValueError('Material mask/texture dimensions differ.')
        self.material_pages[name]=page*mask;self.pins[str(path)]=digest(path)

    def update(self,pose):
        trace=self.core.update({'name':pose.get('name','coverage'),'parameters':pose.get('parameters',{}),
                                'part_opacities':pose.get('part_opacities',{})})
        if any(x['was_clamped'] for x in trace.values()):raise ValueError('Fixture parameter was clamped.')
        self.positions={name:np.asarray(points,dtype=float) for name,points in zip(self.core.drawable_ids,self.core.positions())}
        self.opacity={name:float(self.core.get('DrawableOpacities')[i]) for name,i in self.lookup.items()}
        self.visible={name:bool(self.core.get('DrawableDynamicFlags')[i]&1) for name,i in self.lookup.items()}

    def source_xy(self,native):return np.asarray(native)*[self.ppu,-self.ppu]+self.origin

    def anchor(self,definition):
        if 'source_xy' in definition:return (np.asarray(definition['source_xy'])-self.origin)/[self.ppu,-self.ppu]
        name=definition['drawable']
        if name not in self.lookup:raise ValueError('Unknown anchor drawable '+name)
        vertices=definition['vertices'];weights=np.asarray(definition['weights'],dtype=float)
        if len(vertices)!=len(weights) or abs(weights.sum()-1)>1e-7:raise ValueError('Invalid anchor barycentrics.')
        if not np.all(np.isfinite(weights)):raise ValueError('Nonfinite anchor weights.')
        if weights.min() < -1e-7 and not definition.get('allow_extrapolation',False):
            raise ValueError('An anchor outside its triangle needs explicit allow_extrapolation; it is not material coverage proof.')
        if any(not 0<=v<len(self.positions[name]) for v in vertices):raise ValueError('Invalid anchor vertex.')
        return weights@self.positions[name][vertices]

    def raw_alpha(self,name,points):
        t=self.topology[name]
        page=self.material_pages.get(name,self.pages[t['texture']])
        return mesh_texture_alpha(points,self.positions[name],t['uvs'],t['indices'],page,t['double_sided'])

    def coverage(self,names,points):
        unknown=set(names)-set(self.lookup)
        if unknown:raise ValueError('Unknown required material IDs: '+str(sorted(unknown)))
        raw={};values={}
        for name in names:
            if name not in raw:raw[name]=self.raw_alpha(name,points)
            t=self.topology[name]
            for mask in t['masks']:
                if mask not in raw:raw[mask]=self.raw_alpha(mask,points)
            mask=alpha_union([raw[m] for m in t['masks']]) if t['masks'] else None
            # The bundled Cubism mask shader uses geometry+texture alpha,
            # not the mask drawable's opacity. DrawableOpacities already
            # include evaluated ancestor Part opacity: never multiply twice.
            values[name]=clipped_alpha(raw[name],self.opacity[name] if self.visible[name] else 0,
                                        mask,t['inverted_mask'])
        return alpha_union(values.values(),len(points)),values


def fixture_offsets(check):
    if 'roi_offsets' in check:
        roi=check['roi_offsets'];step=float(roi.get('step',1))
        if step<=0:raise ValueError('ROI step must be positive.')
        offsets=list(itertools.product(np.arange(roi['x'][0],roi['x'][1]+step*.1,step),
                                      np.arange(roi['y'][0],roi['y'][1]+step*.1,step)))
    else:offsets=check.get('offsets_source_pixels',[[0,0]])
    if not 1<=len(offsets)<=4096:raise ValueError('Each ROI must sample 1..4096 points.')
    return np.asarray(offsets,dtype=float).reshape(-1,2)


def fixture_points(runtime,check):
    """Sample a moving joint in a geometry-derived frame, never a fixed pose."""
    anchor=runtime.anchor(check['anchor']);origin=runtime.source_xy(anchor)
    if 'segment_samples' in check:
        end=runtime.source_xy(runtime.anchor(check['end_anchor']))
        direction=end-origin;length=float(np.linalg.norm(direction))
        if length < 1e-8:raise ValueError('Segment anchors coincide.')
        normal=np.asarray([-direction[1],direction[0]])/length
        spec=check['segment_samples']
        fractions=np.asarray(spec.get('fractions',[0,.25,.5,.75,1]),dtype=float)
        transverse=np.asarray(spec.get('transverse_source_pixels',[0]),dtype=float)
        if np.any(fractions<0) or np.any(fractions>1):raise ValueError('Segment fractions must be in 0..1.')
        offsets=np.asarray([f*direction+t*normal for f in fractions for t in transverse])
    else:
        offsets=fixture_offsets(check)
        frame=check.get('offset_frame')
        if frame:
            direction=runtime.source_xy(runtime.anchor(frame['toward_anchor']))-origin
            length=float(np.linalg.norm(direction))
            if length < 1e-8:raise ValueError('Offset frame anchors coincide.')
            tangent=direction/length;normal=np.asarray([tangent[1],-tangent[0]])
            offsets=offsets[:,0,None]*normal+offsets[:,1,None]*tangent
    if not 1<=len(offsets)<=4096 or not np.all(np.isfinite(offsets)):
        raise ValueError('A fixture needs 1..4096 finite sample offsets.')
    return anchor,offsets,anchor+offsets/[runtime.ppu,-runtime.ppu]


def run(model,fixture,output,core=None,metadata=None):
    fixture=Path(fixture).resolve();output=Path(output).resolve()
    data=json.loads(fixture.read_text(encoding='utf-8-sig'));runtime=CoverageModel(model,core,metadata)
    for name,path in data.get('drawable_material_masks',{}).items():
        runtime.add_material_mask(name,(fixture.parent/path).resolve())
    runtime.pins[str(fixture)]=digest(fixture);runtime.pins[str(Path(__file__).resolve())]=digest(Path(__file__))
    if str(output) in runtime.pins:raise ValueError('The report cannot overwrite any model/texture/fixture input.')
    poses=data['poses']
    if not 1<=len(poses)<=128:raise ValueError('Use 1..128 explicit representative poses.')
    expected=data.get('expected_topology',{})
    for name,definition in expected.items():
        if name not in runtime.topology:raise ValueError('Fixture requires missing component '+name)
        topology=runtime.topology[name]
        if len(topology['uvs'])!=definition['vertices'] or len(topology['indices'])!=definition['triangles']:
            raise ValueError('Fixture topology changed; rebuild/review its anchors: '+name)
        canonical=np.sort(topology['indices'],axis=1).astype('<u4').tobytes()
        if 'triangle_membership_sha256' in definition and hashlib.sha256(canonical).hexdigest()!=definition['triangle_membership_sha256']:
            raise ValueError('Fixture indexed triangle membership changed; rebuild/review its anchors: '+name)
    rows=[]
    for pose in poses:
        runtime.update(pose)
        for check in pose['checks']:
            anchor,offsets,points=fixture_points(runtime,check)
            total,components=runtime.coverage(check['required_drawable_group'],points)
            threshold=float(check.get('minimum_alpha',.5));fraction=float(np.mean(total>=threshold))
            missing=np.where(total<threshold)[0]
            rows.append({'pose':pose['name'],'check':check['name'],'anchor_source_xy':runtime.source_xy(anchor).tolist(),
                          'required_drawable_group':check['required_drawable_group'],
                          'samples':len(points),'minimum_alpha':threshold,'covered_fraction':fraction,
                          'minimum_fraction':float(check.get('minimum_fraction',1.0)),
                          'actual_minimum_alpha':float(total.min()),'passed':fraction>=check.get('minimum_fraction',1.0),
                          'failed_sample_offsets_source_px':offsets[missing[:32]].tolist(),
                          'failed_sample_source_xy':runtime.source_xy(points[missing[:32]]).tolist(),
                          'sample_frame':'geometry_segment' if 'segment_samples' in check else ('geometry_oriented' if 'offset_frame' in check else 'source_axes'),
                          'effective_core_opacities':{name:runtime.opacity[name] for name in components},
                          'per_component_maximum_sample_alpha':{name:float(alpha.max()) for name,alpha in components.items()}})
    for path,pin in runtime.pins.items():
        if digest(path)!=pin:raise ValueError('An input changed while checking coverage: '+path)
    report={'schema_version':1,'kind':'core_barycentric_material_alpha_coverage','passed':all(row['passed'] for row in rows),
             'inputs':runtime.pins,'checks':rows,
             'scope':{'actual_core_vertices':True,'actual_uv_alpha':True,'effective_part_and_mesh_opacity':True,
                      'clipping_masks':True,'material_ownership_main_alpha':bool(metadata),'whole_model_png_alpha':False,
                      'semantic_material_masks':list(runtime.material_pages),
                      'runtime_neck_vertex_mapping':False,'renderer_post_transforms':False,'visual_approval':False},
             'limit':'Material coverage of named groups only. Physics, independent runtime neck UV mapping, draw-order aesthetics, GPU finite-resolution clip buffers and beauty remain Native/visual gates.'}
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model',type=Path,required=True);parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--core',type=Path)
    parser.add_argument('--material-metadata',type=Path)
    args=parser.parse_args();report=run(args.model,args.fixture,args.output,args.core,args.material_metadata)
    print(json.dumps({'passed':report['passed'],'checks':len(report['checks']),
                      'failed':[row['check']+'@'+row['pose'] for row in report['checks'] if not row['passed']]}))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
