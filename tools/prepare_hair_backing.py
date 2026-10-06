"""Prepare an authored pure-hair backing mesh on the R15 desk candidate CMO.

The inserted mesh copies ArtMeshBackHair's vertices, parent binding, and full
keyform grid verbatim, so it renders exactly where the legacy contaminated
backing renders in every pose, and rebinds its UVs onto a new appended page
holding the generated pure-hair wings.  The legacy backing stays untouched,
keeping every preservation proof intact.  Run edit_live2d_author.py with the
resulting plan in a fresh output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
R1 = ROOT / 'art/live2d/desk-work-20261006-r1'
R2 = ROOT / 'art/live2d/desk-work-20261006-r2'

CANDIDATE_CMO_SHA = 'e85fe232b94d279e568a819ae236062aa87eacd6a8233b0c50a6074a0c3fe6b3'
CANDIDATE_MOC_SHA = '6401fea4c24420e65b34b3534de6e1c14ccf4d24cec8e35e4eff7b77c30e0d2e'
PUBLIC_GRAPH_SHA = '3ae94e023ecfff3973593168fd91c12d36d27b4564e5bf37af8f5955ee5e490d'
PURE_HAIR_SHA = '39cffd0d010810167fca7a5fdbd710aaac4e0d4889ceee7bd6f3b8669076ad20'

# Atlas wing rects (page 0) -> source canvas via ownership offset (-2671,+550).
ATLAS_WINGS = [(2959, 8, 3274, 358), (3323, 8, 3638, 358)]
ATLAS_OFFSET = (-2671, 550)
SOURCE_WINGS = [(288, 558, 603, 908), (652, 558, 967, 908)]
# Generated pure-hair wing alpha bboxes inside the 1254x1254 page.
GEN_WINGS = [(130, 603, 593, 1114), (661, 602, 1124, 1114)]
GEN_SIZE = 1254


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-moc', type=Path, required=True,
                        help='reference-equivalent.moc3 matching the candidate CMO')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise ValueError('Use a fresh plan output; keep earlier revisions.')
    reference = args.reference_moc.resolve(strict=True)
    if sha(reference) != CANDIDATE_MOC_SHA:
        raise ValueError(f'Reference MOC mismatch: {reference}')

    graph_path = (ROOT / '.local/authoring/whole-model-migration-20261004-r1/'
                  'api-audit/moc-recovery-20261004T082620Z/export/'
                  'recovered-parent-corrected-cmo-public-graph.json')
    if sha(graph_path) != PUBLIC_GRAPH_SHA:
        raise ValueError(f'Authoritative public graph mismatch: {graph_path}')
    graph = json.loads(graph_path.read_text(encoding='utf8'))

    pure = R1 / 'arm-backing-hair-only.png'
    if sha(pure) != PURE_HAIR_SHA:
        raise ValueError(f'Pure hair material mismatch: {pure}')

    template = None
    for drawable in graph['getDrawables']:
        if drawable.get('getName') == 'arm backing':
            template = drawable
    if template is None:
        raise ValueError('arm backing drawable missing from the public graph')
    mesh = template['getMesh']
    positions = np.asarray(mesh['getPositions'], dtype=np.float64).reshape(-1, 2)
    grid = template['getGeometryGrid']
    axes = [{'parameter': a['getParameterId-WD9NFvw'], 'keys': a['getKeys']}
            for a in grid['getAxes']]
    keyforms = []
    for cell in grid['getCells']:
        keyforms.append({'coordinate': list(cell['getCoordinate']),
                         'position_deltas': list(cell['getForm']['getPositionDeltas'])})
    for row in keyforms:
        if len(row['position_deltas']) != positions.size:
            raise ValueError('Template keyform delta size differs from vertex count')

    # Vertex UV/indices come from the formal MOC (bit-exact export of this CMO).
    import ctypes
    import sys
    sys.path.insert(0, str(ROOT / 'tools'))
    from probe_cubism_core import NativeModel, find_core
    native = NativeModel(find_core(), reference)
    ids = native.drawable_ids
    index = ids.index('ArtMeshBackHair')
    count = native.vertex_counts[index]
    if count != len(positions):
        raise ValueError('MOC vertex count differs from the public graph')
    uv_ptr = native.get('DrawableVertexUvs')[index]
    uv = np.frombuffer(ctypes.string_at(uv_ptr, count * 2 * 4),
                       dtype=np.float32).astype(np.float64).reshape(-1, 2)
    idx_ptr = native.get('DrawableIndices')[index]
    index_bytes = native.index_counts[index] * 2
    indices = np.frombuffer(ctypes.string_at(idx_ptr, index_bytes),
                            dtype=np.uint16).astype(np.int64)

    # Remap each vertex UV from the contaminated page-0 wing charts onto the
    # generated pure-hair page, fitting each generated wing into its original
    # source bbox exactly as the recorded runtime registration did.
    atlas_x = uv[:, 0] * 4096.0
    atlas_y = (1.0 - uv[:, 1]) * 4096.0
    src_x = atlas_x + ATLAS_OFFSET[0]
    src_y = atlas_y + ATLAS_OFFSET[1]
    gen_x = np.zeros(len(uv))
    gen_y = np.zeros(len(uv))
    clamped = np.zeros(len(uv), dtype=np.float64)
    for (ax0, ay0, ax1, ay1), (sx0, sy0, sx1, sy1), (gx0, gy0, gx1, gy1) in zip(
            ATLAS_WINGS, SOURCE_WINGS, GEN_WINGS):
        inside = ((atlas_x >= ax0) & (atlas_x <= ax1)
                  & (atlas_y >= ay0) & (atlas_y <= ay1))
        gen_x[inside] = gx0 + (src_x[inside] - sx0) / (sx1 - sx0) * (gx1 - gx0)
        gen_y[inside] = gy0 + (src_y[inside] - sy0) / (sy1 - sy0) * (gy1 - gy0)
    outside = ~((gen_x != 0) | (gen_y != 0))
    # Border vertices sit a few pixels outside their wing chart or on the
    # inter-wing seam; clamp them to the nearest wing rectangle in source
    # space.  UV clamping only affects texel sampling at the silhouette edge.
    if outside.any():
        ox, oy = src_x[outside], src_y[outside]
        distances, targets = [], []
        for (sx0, sy0, sx1, sy1), (gx0, gy0, gx1, gy1) in zip(
                SOURCE_WINGS, GEN_WINGS):
            cx = np.clip(ox, sx0, sx1)
            cy = np.clip(oy, sy0, sy1)
            distances.append(np.hypot(ox - cx, oy - cy))
            targets.append((cx, cy, (gx0, gx1, gy0, gy1), (sx0, sx1, sy0, sy1)))
        best = np.argmin(np.stack(distances), axis=0)
        max_clamp = float(np.min(np.stack(distances), axis=0).max())
        if max_clamp > 8.0:
            raise ValueError(f'Wing-border clamp distance too large: {max_clamp:.1f}px')
        for local_k, k in enumerate(np.where(outside)[0]):
            b = best[local_k]
            sx0, sx1, sy0, sy1 = SOURCE_WINGS[b]
            gx0, gx1, gy0, gy1 = GEN_WINGS[b]
            cx = min(max(src_x[k], sx0), sx1)
            cy = min(max(src_y[k], sy0), sy1)
            gen_x[k] = gx0 + (cx - sx0) / (sx1 - sx0) * (gx1 - gx0)
            gen_y[k] = gy0 + (cy - sy0) / (sy1 - sy0) * (gy1 - gy0)
    else:
        max_clamp = 0.0
    page_u = gen_x / GEN_SIZE
    page_v = 1.0 - gen_y / GEN_SIZE
    if page_u.min() < 0 or page_u.max() > 1 or page_v.min() < 0 or page_v.max() > 1:
        raise ValueError('Remapped UV leaves the unit page')
    uvs = np.column_stack((page_u, page_v)).astype(np.float32).reshape(-1).tolist()

    r1_plan = json.loads((R1 / 'author-plan.json').read_text(encoding='utf8'))
    source_pages = list(r1_plan['source']['atlas_pages']) + list(r1_plan['append_pages'])
    if len(source_pages) != 4:
        raise ValueError('Unexpected candidate page count')
    for row in source_pages:
        if sha(row['path']) != row['sha256']:
            raise ValueError(f"Candidate page drifted: {row['path']}")

    mesh_row = {
        'id': 'ArtMeshHairBackingPure',
        'name': 'authored pure-hair wing backing (covers legacy contaminated backing)',
        'template_id': 'ArtMeshBackHair',
        'parent_id': template['getParentDeformerId-lmpY1tE'],
        'part_id': 'PartHairBack',
        'texture_page': 4,
        'positions': positions.astype(np.float32).reshape(-1).tolist(),
        'uvs': uvs,
        'indices': indices.astype(np.int64).tolist(),
        'axes': axes,
        'keyforms': keyforms,
        'channels': [],
        'draw_order': 3,
        'opacity': 1,
    }
    plan = {
        'schema_version': 2,
        'kind': 'authored_pure_hair_backing',
        'description': (
            'R19: cover the legacy contaminated arm backing with an authored mesh '
            'sharing its vertices, parent deformer, and keyform grid, textured '
            'from the generated pure-hair wings on a new page.  No existing node '
            'is modified; the backing renders directly above ArtMeshBackHair.'),
        'source': {
            'cmo_sha256': CANDIDATE_CMO_SHA,
            'moc_sha256': CANDIDATE_MOC_SHA,
            'atlas_pages': source_pages,
        },
        'parameters': [],
        'deformer_grids': [],
        'mesh_grids': [],
        'append_pages': [{'path': str(pure), 'sha256': PURE_HAIR_SHA}],
        'insert_meshes': [mesh_row],
        'coordinate_contract': {
            'parent': template['getParentDeformerId-lmpY1tE'],
            'vertex_policy': 'verbatim template ArtMeshBackHair parent-local positions',
            'keyform_policy': 'verbatim template grid deltas (identical vertices)',
            'uv_policy': ('page-0 wing charts remapped to the generated page via '
                          'atlas offset (-2671,+550), source wing bboxes, and the '
                          'recorded generated alpha bboxes (fit registration)'),
            'render_order': 'PartHairBack segment, declared draw_order 3 '
                            '(above ArtMeshBackHair 2, below headwear legacy ties)',
            'legacy_backing': 'untouched; every preservation proof remains valid',
        },
        'authoredSitPose': False,
        'provenance': {
            'producer': 'tools/prepare_hair_backing.py',
            'producer_sha256': sha(Path(__file__)),
            'public_graph_sha256': PUBLIC_GRAPH_SHA,
            'pure_hair_sha256': PURE_HAIR_SHA,
            'status': 'pending_visual_review', 'adopted': False,
        },
        'limits': [
            'Isolated candidate on the r1 desk candidate CMO; production untouched.',
            'Visual verdict pending; the R17 content question goes to the user.',
            'Duplicate ear tips and small islands still need the isolated island '
            'cleanup; they are not addressed by this plan.',
        ],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(plan, ensure_ascii=False, indent=1) + '\n',
                      encoding='utf8')
    print(json.dumps({'plan': str(output), 'sha256': sha(output),
                      'vertices': int(count), 'triangles': int(len(indices) / 3),
                      'axes': [a['parameter'] for a in axes],
                      'keyforms': len(keyforms),
                      'max_wing_clamp_px': max_clamp,
                      'uv_bbox': [float(page_u.min()), float(page_v.min()),
                                  float(page_u.max()), float(page_v.max())]},
                     ensure_ascii=False))


if __name__ == '__main__':
    main()
