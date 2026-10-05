"""Inventory actual Core UV triangles, PNG alpha edges, Parts and clip masks.

Run on any staged final candidate; no author graph or C++ renderer is needed.
An opaque mesh rectangle is not proof that its articulated material is present.
This inventory supplies structural/texture evidence; joint coverage remains a
separate verify_component_coverage.py fixture and Native visual review.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from probe_cubism_core import digest
from verify_component_coverage import CoverageModel, sample_texture_alpha


def uv_triangle_union(uvs,indices,width,height):
    """Rasterize texel centres in the actual UV triangles, including alpha holes."""
    uv=np.asarray(uvs,dtype=float).reshape(-1,2)
    pixels=uv*[width,-height]+[0,height]
    low=np.maximum(np.floor(pixels.min(0)).astype(int),[0,0])
    high=np.minimum(np.ceil(pixels.max(0)).astype(int),[width,height])
    high=np.maximum(high,low)
    mask=np.zeros((int(high[1]-low[1]),int(high[0]-low[0])),dtype=bool)
    for triangle in np.asarray(indices,dtype=int).reshape(-1,3):
        a,b,c=pixels[triangle];e=b-a;f=c-a;det=e[0]*f[1]-e[1]*f[0]
        if abs(det)<1e-14:continue
        begin=np.maximum(np.floor(np.minimum(np.minimum(a,b),c)).astype(int),low)
        end=np.minimum(np.ceil(np.maximum(np.maximum(a,b),c)).astype(int),high)
        if np.any(end<=begin):continue
        xs=np.arange(begin[0],end[0])+.5;ys=np.arange(begin[1],end[1])+.5
        xx,yy=np.meshgrid(xs,ys);dx=xx-a[0];dy=yy-a[1]
        u=(dx*f[1]-dy*f[0])/det;v=(e[0]*dy-e[1]*dx)/det
        inside=(u>=-1e-9)&(v>=-1e-9)&(u+v<=1+1e-9)
        mask[begin[1]-low[1]:end[1]-low[1],begin[0]-low[0]:end[0]-low[0]]|=inside
    return low,high,mask


def chart_inventory(uvs,indices,alpha):
    uv=np.asarray(uvs,dtype=float).reshape(-1,2)
    triangles=np.asarray(indices,dtype=int).reshape(-1,3)
    height,width=np.asarray(alpha).shape
    valid_indices=bool(np.all(triangles>=0)&np.all(triangles<len(uv)))
    finite=bool(np.all(np.isfinite(uv)))
    result={'vertices':len(uv),'triangles':len(triangles),'finite_uvs':finite,
            'indices_in_range':valid_indices,'uv_range':None,
            'uv_vertices_outside_0_1':0,'rectangular_chart_is_material_coverage':False}
    if not finite or not valid_indices:return result
    result['uv_range']=[uv.min(0).tolist(),uv.max(0).tolist()]
    result['uv_vertices_outside_0_1']=int(np.any((uv<0)|(uv>1),axis=1).sum())
    edge1=uv[triangles[:,1]]-uv[triangles[:,0]];edge2=uv[triangles[:,2]]-uv[triangles[:,0]]
    areas=edge1[:,0]*edge2[:,1]-edge1[:,1]*edge2[:,0]
    result['degenerate_uv_triangles']=np.where(np.abs(areas)<1e-14)[0].tolist()
    low,high,chart=uv_triangle_union(uv,triangles,width,height)
    crop=np.asarray(alpha)[low[1]:high[1],low[0]:high[0]]
    opaque=crop>=.5;nonzero=crop>0
    ys,xs=np.nonzero(opaque&chart)
    interior=chart.copy()
    if chart.size:
        padded=np.pad(chart,1)
        interior&=padded[:-2,1:-1]&padded[2:,1:-1]&padded[1:-1,:-2]&padded[1:-1,2:]
    boundary=chart&~interior
    touches=[]
    if chart.size:
        for side,hit in [('left',low[0]==0 and bool((opaque[:,0]&chart[:,0]).any())),
                         ('right',high[0]==width and bool((opaque[:,-1]&chart[:,-1]).any())),
                         ('top',low[1]==0 and bool((opaque[0]&chart[0]).any())),
                         ('bottom',high[1]==height and bool((opaque[-1]&chart[-1]).any()))]:
            if hit:touches.append(side)
    vertex_alpha=sample_texture_alpha(alpha,uv)
    result.update({'chart_bbox_texture_pixels':[int(low[0]),int(low[1]),int(high[0]),int(high[1])],
                   'actual_uv_triangle_union_texels':int(chart.sum()),
                   'opaque_texels_in_uv_triangles':int((opaque&chart).sum()),
                   'transparent_texels_in_uv_triangles':int((~nonzero&chart).sum()),
                   'AA_texels_alpha_0_to_128_in_uv_triangles':int(((crop>0)&(crop<.5)&chart).sum()),
                   'opaque_bbox_in_uv_triangles':([int(low[0]+xs.min()),int(low[1]+ys.min()),int(low[0]+xs.max()+1),int(low[1]+ys.max()+1)] if len(xs) else None),
                   'opaque_texels_in_chart_rectangle_outside_uv_triangles':int((opaque&~chart).sum()),
                   'opaque_texels_on_uv_triangle_union_boundary':int((opaque&boundary).sum()),
                   'opaque_texture_page_edges_touched':touches,
                   'clamp_to_edge_sampler':True,
                   'uv_vertex_alpha_min_max':[float(vertex_alpha.min()),float(vertex_alpha.max())],
                   'boundary_note':'Opaque texels at UV boundaries and outside triangle union are diagnostics. They are not automatically missing material; silhouettes, overlaps and neighbouring atlas charts must be reviewed.'})
    return result


def run(model,output,core=None,metadata=None):
    runtime=CoverageModel(model,core,metadata);runtime.update({'name':'inventory-default'})
    output=Path(output).resolve();runtime.pins[str(Path(__file__).resolve())]=digest(Path(__file__))
    runtime.pins[str(Path(__file__).with_name('verify_component_coverage.py').resolve())]=digest(Path(__file__).with_name('verify_component_coverage.py'))
    if str(output) in runtime.pins:raise ValueError('Inventory output cannot overwrite an input.')
    document=json.loads(runtime.model_path.read_text(encoding='utf-8-sig'))
    texture_rows=[]
    for index,reference in enumerate(document['FileReferences']['Textures']):
        path=(runtime.model_path.parent/reference).resolve()
        with Image.open(path) as image:
            alpha=np.asarray(image.convert('RGBA'))[:,:,3]
            texture_rows.append({'index':index,'path':str(path),'sha256':digest(path),'mode':image.mode,
                                 'size':list(image.size),'has_alpha_channel':'A' in image.getbands() or 'transparency' in image.info,
                                 'opaque_pixels_ge128':int((alpha>=128).sum()),
                                 'alpha_1_to_7_pixels':int(((alpha>=1)&(alpha<=7)).sum()),
                                 'alpha_nonzero_pixels':int((alpha>0).sum()),
                                 'opaque_page_border_pixels':{'left':int((alpha[:,0]>=128).sum()),'right':int((alpha[:,-1]>=128).sum()),
                                                               'top':int((alpha[0]>=128).sum()),'bottom':int((alpha[-1]>=128).sum())}})
    part_parents=runtime.core.get('PartParentPartIndices');draw_parts=runtime.core.get('DrawableParentPartIndices')
    components=[]
    for name,index in runtime.lookup.items():
        topology=runtime.topology[name];parent=int(draw_parts[index]);chain=[];seen=set()
        while parent>=0:
            if parent>=len(runtime.core.part_ids) or parent in seen:raise ValueError('Invalid/cyclic Part parent chain: '+name)
            seen.add(parent);chain.append(runtime.core.part_ids[parent]);parent=int(part_parents[parent])
        stats=chart_inventory(topology['uvs'],topology['indices'],runtime.pages[topology['texture']])
        components.append({'id':name,'texture_page':topology['texture'],'parent_parts':chain,
                           'effective_default_opacity':runtime.opacity[name],'default_visible':runtime.visible[name],
                           'double_sided':topology['double_sided'],
                           'clipping':{'mask_ids':topology['masks'],'mode':('inverted' if topology['inverted_mask'] else 'normal') if topology['masks'] else 'none',
                                       'inverted_flag':topology['inverted_mask'],'all_mask_ids_exist':all(m in runtime.lookup for m in topology['masks']),
                                       'self_mask':name in topology['masks'],'mask_drawable_opacity_is_not_applied':True},**stats})
    structural=all(c['finite_uvs'] and c['indices_in_range'] and not c['uv_vertices_outside_0_1'] and c['clipping']['all_mask_ids_exist'] for c in components)
    report={'schema_version':1,'kind':'actual_core_component_uv_alpha_clip_inventory','structural_passed':structural,
            'inputs':runtime.pins,'texture_pages':texture_rows,'parts':[{'id':name,'default_opacity':runtime.core.part_defaults[i],'parent_index':int(part_parents[i])} for i,name in enumerate(runtime.core.part_ids)],
            'components':components,'summary':{'components':len(components),'texture_pages':len(texture_rows),
                                               'normal_mask_components':[c['id'] for c in components if c['clipping']['mode']=='normal'],
                                               'inverted_mask_components':[c['id'] for c in components if c['clipping']['mode']=='inverted'],
                                               'components_without_opaque_uv_texels':[c['id'] for c in components if c.get('opaque_texels_in_uv_triangles',0)==0],
                                               'components_touching_opaque_texture_page_edges':[c['id'] for c in components if c.get('opaque_texture_page_edges_touched')]},
            'scope':{'actual_native_core_topology':True,'texel_centre_triangle_union':True,'actual_png_alpha':True,
                     'main_runtime_ownership_cuts_applied':bool(metadata),'GPU_clipbuffer_quantization':False,'draw_order_beauty':False,'runtime_independent_neck_mapping':False},
            'next_gates':['Barycentric named semantic material joint coverage','Dense Core contact/length/orientation','Native continuous motion and independent neck mapped-surface review']}
    for path,pin in runtime.pins.items():
        if digest(path)!=pin:raise ValueError('Input changed during inventory: '+path)
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf8')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--core',type=Path);parser.add_argument('--material-metadata',type=Path)
    args=parser.parse_args();report=run(args.model,args.output,args.core,args.material_metadata)
    print(json.dumps({'structural_passed':report['structural_passed'],**report['summary']}))
    return 0 if report['structural_passed'] else 1


if __name__=='__main__':raise SystemExit(main())
