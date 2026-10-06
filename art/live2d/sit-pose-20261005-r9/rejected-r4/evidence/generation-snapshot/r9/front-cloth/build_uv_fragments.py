"""Read complete clothing source -> mutually exclusive UV/topology fragments.

No raster changes, Body/arm/leg operations, CMO edits or invented target cells.
The split is executable input for the pending actual contact-profile binder.
"""
from pathlib import Path
import hashlib
import json
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
TABLE = HERE / 'uv-layer-execution-table-r2.json'
OUTPUT = HERE / 'uv-fragments-r1.json'
PROOF = HERE / 'uv-ownership-proof-r1.json'
ANCHORS = HERE / 'uv-anchors-r1.json'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding='utf8'))


def inside(x, y, poly):
    result = False
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 > y) != (y1 > y) and x < (x1 - x0) * (y - y0) / (y1 - y0) + x0:
            result = not result
    return result


def curve(points, x):
    p = np.asarray(points, dtype=float)
    return float(np.interp(x, p[:, 0], p[:, 1]))


def owner(table, x, y):
    o = table['ownership']
    if y < o['waist_band_end_y']:
        return 'ArtMeshR9WaistFoldUnder'
    for side, (x0, y0, x1, y1) in o['bow_rectangles'].items():
        if x0 <= x < x1 and y0 <= y < y1:
            return 'ArtMeshR9Bow' + side
    if inside(x, y, o['apron_outer_polygon']):
        return 'ArtMeshR9Apron'
    if y >= curve(o['lower_frill_top_seed'], x):
        tier = 'Upper' if y < curve(o['lower_tier_seam_seed'], x) else 'Lower'
        a, b = o['frill_C_x_range']
        side = 'L' if x < a else ('R' if x >= b else 'C')
        return 'ArtMeshR9Frill' + tier + side
    return 'ArtMeshR9FrontLap' + ('L' if x < 627 else 'R')


def bilinear_alpha(a, x, y):
    # Normalized UV source_x/width represents a texel-corner position. Texture
    # bilinear sampling uses pixel-centre coordinates at uv*size-.5.
    x, y = x - .5, y - .5
    ix, iy = int(np.floor(x)), int(np.floor(y))
    u, v = x - ix, y - iy
    return float(((1-u)*(1-v)*a[iy, ix] + u*(1-v)*a[iy, ix+1]
                 + (1-u)*v*a[iy+1, ix] + u*v*a[iy+1, ix+1]) / 255.)


def main():
    assert not any(p.exists() for p in [OUTPUT, PROOF, ANCHORS]), 'Use a fresh revision'
    table = read(TABLE)
    png, cmo = Path(table['source_png']), Path(table['source_CMO'])
    assert sha(png) == table['source_png_sha256']
    assert sha(cmo) == table['source_CMO_sha256']
    files = [TABLE, png, cmo, Path(__file__)]
    pins = {str(p.resolve()): {'sha256': sha(p), 'bytes': p.stat().st_size} for p in files}
    rgba = np.asarray(Image.open(png).convert('RGBA'))
    assert rgba.shape == (1254, 1254, 4)
    opaque = rgba[:, :, 3] >= 128
    x0, y0, x1, y1 = table['ownership']['material_domain']
    xs = set(range(x0, x1, 64)) | {x1, 627} | {a['source_xy'][0] for a in table['anchors']}
    ys = set(range(y0, y1, 48)) | {y1, table['ownership']['waist_band_end_y']} | {a['source_xy'][1] for a in table['anchors']}
    for rect in table['ownership']['bow_rectangles'].values():
        xs.update([rect[0], rect[2]])
        ys.update([rect[1], rect[3]])
    xs.update(table['ownership']['frill_C_x_range'])
    xs, ys = sorted(xs), sorted(ys)
    pixels = np.asarray([(x, y) for y in ys for x in xs], dtype=float)
    count_x = len(xs)
    uv = (pixels / 1254.).astype(np.float32)
    pixel_index = {tuple(p.astype(int)): i for i, p in enumerate(pixels)}
    names = [r['id'] for r in table['pieces']]
    owners = {n: [] for n in names}
    faces = []
    face_owner = []
    # This is an audit map in memory only, never a replacement/edited PNG.
    alpha_owner = np.full(opaque.shape, -1, dtype=np.int16)
    retained_cells = 0
    for yi in range(len(ys) - 1):
        for xi in range(len(xs) - 1):
            xa, xb, ya, yb = xs[xi], xs[xi+1], ys[yi], ys[yi+1]
            mask = opaque[ya:yb, xa:xb]
            if not np.any(mask):
                continue
            retained_cells += 1
            tl = yi*count_x + xi
            tr, bl, br = tl+1, tl+count_x, tl+count_x+1
            tris = [[tl, br, tr], [tl, bl, br]]
            labels = []
            for tri in tris:
                centroid = pixels[tri].mean(axis=0)
                label = owner(table, *centroid)
                assert label in owners
                f = len(faces)
                faces.append(tri)
                face_owner.append(label)
                owners[label].append(f)
                labels.append(names.index(label))
            fy = (np.arange(ya, yb)[:, None] + .5 - ya) / (yb-ya)
            fx = (np.arange(xa, xb)[None, :] + .5 - xa) / (xb-xa)
            assigned = np.where(fy <= fx, labels[0], labels[1])
            alpha_owner[ya:yb, xa:xb][mask] = assigned[mask]
    faces = np.asarray(faces, dtype=np.int32)
    p = pixels[faces]
    signed = .5 * ((p[:, 1, 0]-p[:, 0, 0])*(p[:, 2, 1]-p[:, 0, 1])
                   - (p[:, 1, 1]-p[:, 0, 1])*(p[:, 2, 0]-p[:, 0, 0]))
    assert np.all(signed < 0), 'UV degenerate/mixed orientation'
    assert np.all(alpha_owner[opaque] >= 0), 'Opaque cloth source pixel unowned'
    assert all(owners[n] for n in names), 'Empty role in the execution table'
    all_ids = [f for fids in owners.values() for f in fids]
    assert len(all_ids) == len(set(all_ids)) == len(faces), 'Source face ownership overlap'
    specs = []
    bindings = {}
    vertex_owners = {}
    piece_meta = {r['id']: r for r in table['pieces']}
    for name in names:
        fids = owners[name]
        global_ids = sorted(set(faces[fids].reshape(-1).tolist()))
        mapping = {g: i for i, g in enumerate(global_ids)}
        bindings[name] = mapping
        for g in global_ids:
            vertex_owners.setdefault(g, []).append({'piece': name, 'local_vertex': mapping[g]})
        specs.append({'id': name, 'name': piece_meta[name]['role'],
            'template_id': 'ArtMeshSitFrontSkirt', 'parent_id': table['parent_id'],
            'part_id': 'PartExtra', 'texture_page': 8,
            'draw_order': piece_meta[name]['draw_order'], 'opacity': 0.,
            'global_UV_vertex_ids': global_ids, 'global_source_face_ids': fids,
            'source_pixel_positions': pixels[global_ids].reshape(-1).tolist(),
            'uvs': uv[global_ids].reshape(-1).astype(float).tolist(),
            'indices': [mapping[v] for tri in faces[fids] for v in tri],
            'positions': None, 'axes': None, 'keyforms': None,
            'bind_state': 'UV/topology ready; actual parent-local positions/keyforms require the pinned R9 real contact profile. Not an exportable placeholder.'})
    anchors = []
    for a in table['anchors']:
        x, y = a['source_xy']
        g = pixel_index[(x, y)]
        assert g in bindings[a['piece']], (a['name'], 'wrong semantic owner')
        alpha = bilinear_alpha(rgba[:, :, 3].astype(float), x, y)
        assert alpha >= .9, (a['name'], alpha)
        anchors.append(a | {'public_UV': uv[g].astype(float).tolist(),
            'global_vertex_id': g, 'local_vertex': bindings[a['piece']][g],
            'source_RGBA_nearest': rgba[y, x].tolist(),
            'source_alpha_bilinear_texel_center': alpha,
            'source_SHA': table['source_png_sha256']})
    front = set(table['projection_groups']['visible_front_fold_over'])
    shared = [{'global_vertex_id': g, 'source_xy': pixels[g].tolist(), 'bindings': records,
               'relation': 'exact projected weld within visible-front group' if all(r['piece'] in front for r in records)
                           else 'explicit hidden under/front fold seam; separate projection islands'}
              for g, records in sorted(vertex_owners.items()) if len(records) > 1]
    for path, rec in pins.items():
        assert sha(path) == rec['sha256'], 'Source changed'
    report = {'schema_version': 1, 'kind': 'R9_UV_only_material_fragment',
        'inputs': pins, 'append_pages': [{'path': str(png.resolve()), 'sha256': sha(png)}],
        'expected_source_pages': 8, 'new_page': 8,
        'insert_mesh_specs': specs, 'shared_UV_seams': shared,
        'anchor_receipt': str(ANCHORS.resolve()),
        'allowed_existing_mesh_operation': {'id': 'ArtMeshSitFrontSkirt', 'channel': 'OPACITY',
            'rule': 'Only Extra old-front visibility; preserve its exact Sit0 value. Source34/Body/arm/legs not listed or changed.'},
        'actual_contact_binding': 'pending neck R9 world profile; no invented old BodyDrop120',
        'export_ready': False, 'visual_review': 'pending Native', 'adoption': 'pending'}
    opaque_per_piece = {n: int(np.count_nonzero(alpha_owner == i)) for i, n in enumerate(names)}
    proof = {'schema_version': 1, 'kind': 'R9_source_UV_ownership_only_proof', 'inputs': pins,
        'source_png_unchanged': True, 'original_CMO_unchanged': True,
        'triangle_count': len(faces), 'retained_UV_cells': retained_cells,
        'global_UV_grid_vertices': len(pixels), 'fragment_vertex_count_with_shared_seams': sum(len(s['global_UV_vertex_ids']) for s in specs),
        'source_triangle_owner_count': {n: len(owners[n]) for n in names},
        'main_source_alpha128_pixel_count': int(opaque.sum()),
        'main_source_alpha128_pixels_unowned': int(np.count_nonzero(opaque & (alpha_owner < 0))),
        'source_triangle_owner_overlap_count': len(all_ids) - len(set(all_ids)),
        'alpha128_pixels_per_piece': opaque_per_piece,
        'all_source_UV_faces_strict_original_sign': bool(np.all(signed < 0)),
        'UV_area_min_source_pixel_squared': float(abs(signed).min()),
        'anchors_actual_bilinear_alpha_min': min(a['source_alpha_bilinear_texel_center'] for a in anchors),
        'source_low_alpha_outside_material_domain': int(np.count_nonzero((rgba[:,:,3] > 0) &
            ((np.indices(opaque.shape)[0] < y0) | (np.indices(opaque.shape)[0] >= y1)))),
        'limits': ['Ownership/UV is verified; no Core, posed material-contact, opacity curve, Native render or visual acceptance is implied.',
                   'Faint isolated source-alpha outside the material domain is not mapped; original PNG bytes remain untouched.',
                   'Shared UV vertices only weld within a projection group; real rear waist and visible fold-over are not a single inverted sheet.'],
        'UV_ownership_passed': True, 'actual_contact_binding': 'pending', 'visual_review': 'pending Native', 'adoption': 'pending'}
    for path, value in [(OUTPUT, report), (PROOF, proof), (ANCHORS, {'kind':'R9_new_cloth_actual_UV_anchor_receipt', 'inputs':pins, 'anchors':anchors})]:
        with path.open('x', encoding='utf8') as f:
            f.write(json.dumps(value, indent=2, allow_nan=False) + '\n')
    print(json.dumps({'fragment':str(OUTPUT), 'sha256':sha(OUTPUT), 'proof':str(PROOF),
        'pieces':len(specs), 'vertices':proof['fragment_vertex_count_with_shared_seams'],
        'triangles':len(faces), 'alpha128_unowned':proof['main_source_alpha128_pixels_unowned'],
        'overlap':proof['source_triangle_owner_overlap_count'], 'min_anchor_alpha':proof['anchors_actual_bilinear_alpha_min'],
        'actual_contact_binding':'pending'}, indent=2))


if __name__ == '__main__':
    main()
