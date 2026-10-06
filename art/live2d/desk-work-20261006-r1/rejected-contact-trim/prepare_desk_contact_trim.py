"""Map reviewed material keep contours to actual Native UV triangle domains.

Produces a vector contract for runtime material ownership. No PNG or model is
edited. Each polygon is intersected with the original source triangle before
barycentric UV mapping; no approximate affine extrapolation is used.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cross(a,b):
    return a[0]*b[1]-a[1]*b[0]


def clip(subject, triangle):
    sign = 1 if cross(triangle[1]-triangle[0],triangle[2]-triangle[0]) > 0 else -1
    points = list(np.asarray(subject,float))
    for a,b in zip(triangle,np.roll(triangle,-1,axis=0)):
        if not points:
            break
        output = []
        previous = points[-1]
        before = sign*cross(b-a,previous-a)
        for point in points:
            now = sign*cross(b-a,point-a)
            if (now >= -1e-9) != (before >= -1e-9):
                output.append(previous+(point-previous)*before/(before-now))
            if now >= -1e-9:
                output.append(point)
            previous, before = point, now
        points = output
    return np.asarray(points)


def inside(point, polygon):
    x,y = point
    result = False
    for a,b in zip(polygon,polygon[1:]+polygon[:1]):
        v = np.asarray(b)-a
        d = np.asarray(point)-a
        if np.dot(v,v) < 1e-12:
            continue
        if abs(cross(v,d)) < 1e-5 and np.dot(d,v) >= -1e-5 and np.dot(d,v) <= np.dot(v,v)+1e-5:
            return True
        if (a[1]>y) != (b[1]>y) and x < (b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0]:
            result = not result
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--audit',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    args = ap.parse_args()
    source, output = args.audit.resolve(strict=True), args.output.resolve()
    if output.exists():
        raise ValueError('Use a fresh vector contract output.')
    audit = json.loads(source.read_text(encoding='utf8'))
    regions, fixtures = [], []
    for name in ['sleeve_l','sleeve_r']:
        entry = audit['contracts'][name]
        assert entry['stage_r5_native_uv_indices_byte_exact']
        assert entry['all_fixture_memberships_pass']
        basis = entry['normative_exact_triangle_mapping']
        vertices = np.asarray(basis['source1254_vertices'],float)
        uv = np.asarray(basis['native_uv_v_up'],float)*[4096.,-4096.]+[0.,4096.]
        polygons = []
        for material in entry['keep_polygons']:
            for indices in basis['triangles']:
                triangle = vertices[indices]
                if abs(cross(triangle[1]-triangle[0],triangle[2]-triangle[0])) < 1e-9:
                    continue
                cut = clip(material['points'],triangle)
                if len(cut) < 3:
                    continue
                area = sum(cross(a,b) for a,b in zip(cut,np.roll(cut,-1,axis=0)))
                if abs(area) < 1e-8:
                    continue
                weights = np.column_stack((cut,np.ones(len(cut)))) @ np.linalg.inv(
                    np.column_stack((triangle,np.ones(3))))
                atlas_polygon = weights @ uv[indices]
                clean = []
                for point in atlas_polygon.round(6).tolist():
                    if not clean or np.linalg.norm(np.asarray(point)-clean[-1]) > 1e-7:
                        clean.append(point)
                if len(clean)>1 and np.linalg.norm(np.asarray(clean[0])-clean[-1])<1e-7:
                    clean.pop()
                if len(clean)>=3:
                    polygons.append(clean)
        bounds = [math.floor(uv[:,0].min())-1,math.floor(uv[:,1].min())-1,
                  math.ceil(uv[:,0].max())+1,math.ceil(uv[:,1].max())+1]
        assert min(bounds) >= 0 and max(bounds) <= 4096
        for fixture in entry['semantic_fixtures']:
            keep = any(inside(fixture['atlas_pixel_xy'],p) for p in polygons)
            assert keep == fixture['expected_keep'], (name,fixture['label'])
            fixtures.append({'drawable':entry['drawable'],'label':fixture['label'],
                'atlas_pixel_xy':fixture['atlas_pixel_xy'],'expected_keep':keep,'passed':True})
        regions.append({'drawableId':entry['drawable'],'atlas_rect':bounds,
            'keep_polygons':polygons,'source_keep_contours':entry['keep_polygons'],
            'mapping':'original triangle intersection; exact barycentric Native UV'})
    contract = {'version':1,'atlas_size':[4096,4096],
        'atlas_sha256':'e27d3aac6faa2eb0cbfc8e3275ccc57ec563aeaa685cd53a4a0cd5151ad6b90e',
        'regions':regions,'fixtures':fixtures,
        'status':'pending_native_contact_review','source_atlas_modified':False,
        'provenance':{'audit_sha256':sha(source),'producer_sha256':sha(Path(__file__)),
                      'affine_extrapolation_used':False,'new_bitmap_assets':False}}
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(contract,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print(json.dumps({'contract':str(output),'sha256':sha(output),
        'regions':[(r['drawableId'],len(r['keep_polygons']),r['atlas_rect']) for r in regions],
        'certified_fixtures':len(fixtures)}))


if __name__ == '__main__':
    main()
