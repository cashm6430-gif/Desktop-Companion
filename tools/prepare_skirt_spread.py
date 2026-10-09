# -*- coding: utf-8 -*-
"""Build the ParamSkirtSpread author plan (insert-mesh route).

The exported MOC keeps the original 4-axis binding for an edited mesh and
drops appended axes (verified with PSD2LiveMocBinding), so the spread form
cannot live on the existing skirt grid. Instead this plan inserts a copy of
the busy-skirt mesh that carries the new axis:

  ArtMeshObjects7Spread, same parent deformer / part / texture / topology.
  geometry axes = [ParamSitPose, ParamBusyLaptop, ParamSkirtSpread] (8 cells)
    sit=0 cells: zero deltas, opacity 0  (never visible while standing)
    sit=1 spread=0: zero deltas, opacity 0  (the original skirt shows)
    sit=1 spread=1: hem-outward/down deltas, opacity 1  (covers the original)
  OPACITY channel = ParamSitPose x ParamSkirtSpread.

Because the copy shares the original's parent deformer, every deformer-chain
deformation (sit fold, rock, visibility) applies to both meshes identically;
the overlay only adds the spread delta on top. At spread=1 the displaced
copy fully covers the original skirt region.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SPREAD_X = 0.030     # parent-local outward hem travel at weight 1
HEM_DROP = 0.011     # parent-local downward hem travel at weight 1


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, required=True,
                        help='PSD2LiveMeshDump JSON of ArtMeshObjects7')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--spread-x', type=float, default=SPREAD_X)
    parser.add_argument('--hem-drop', type=float, default=HEM_DROP)
    parser.add_argument('--uniform-dy', type=float, default=None,
                        help='Calibration probe: replace the delta field with a uniform dy')
    args = parser.parse_args()

    cmo = ROOT / 'art/live2d/desk-work-20261006-r2/author-source/edited.cmo3'
    moc = ROOT / 'art/live2d/desk-work-20261006-r2/author-source/edited.moc3'
    pages = [ROOT / f'assets/live2d/whale-girl/whale-girl-layered-draft.4096/texture_{i:02d}.png'
             for i in range(5)]

    dump = json.loads(args.dump.read_text())[0]
    positions = dump['positions']
    xs = positions[0::2]
    ys = positions[1::2]
    cx = (min(xs) + max(xs)) / 2.0
    # Parent-local y points UP (native): max y is the hip line, min y the hem.
    # Calibrated with a uniform-dy probe render: +dy moves the mesh up.
    y_hem, y_hip = min(ys), max(ys)
    span = y_hip - y_hem
    if span <= 0:
        raise ValueError('Degenerate skirt y span')

    deltas = []
    if args.uniform_dy is not None:
        deltas = [0.0, args.uniform_dy] * len(xs)
    else:
        for x, y in zip(xs, ys):
            weight = smooth((y_hip - y) / span)
            dx = (1.0 if x >= cx else -1.0) * args.spread_x * weight
            dy = -args.hem_drop * weight
            deltas += [round(dx, 6), round(dy, 6)]
    zero = [0.0] * len(deltas)

    # Geometry cells: [sit, laptop, spread]; the original mesh carries no
    # keyform deltas of its own, so the zero cells coincide with it exactly.
    forms = []
    for sit in (0, 1):
        for laptop in (0, 1):
            for spread in (0, 1):
                forms.append({
                    'coordinate': [sit, laptop, spread],
                    'position_deltas': deltas if (sit == 1 and spread == 1) else zero,
                })
    opacity_forms = [
        {'coordinate': [sit, spread], 'value': 1.0 if (sit == 1 and spread == 1) else 0.0}
        for sit in (0, 1) for spread in (0, 1)
    ]

    plan = {
        'schema_version': 2,
        'kind': 'seated_skirt_spread_overlay',
        'description': ('Inserted busy-skirt copy carrying ParamSkirtSpread: the '
                        'seated spread form fades in over the untouched original '
                        'skirt only while seated (SitPose=0 cells stay invisible), '
                        'driven at runtime by the sit transition tail.'),
        'source': {
            'cmo_sha256': sha(cmo),
            'moc_sha256': sha(moc),
            'atlas_pages': [{'path': str(p.resolve()), 'sha256': sha(p)} for p in pages],
        },
        'parameters': [
            {'id': 'ParamSkirtSpread', 'name': 'SkirtSpread', 'min': 0.0,
             'max': 1.0, 'default': 0.0},
        ],
        'deformer_grids': [],
        'mesh_grids': [],
        'append_pages': [],
        'insert_meshes': [
            {
                'id': 'ArtMeshObjects7Spread',
                'name': 'busy skirt spread overlay',
                'template_id': 'ArtMeshObjects7',
                'parent_id': dump['parent'],
                'part_id': 'PartExtra',
                'texture_page': 0,
                'positions': positions,
                'uvs': dump['uvs'],
                'indices': dump['indices'],
                'axes': [
                    {'parameter': 'ParamSitPose', 'keys': [0.0, 1.0]},
                    {'parameter': 'ParamBusyLaptop', 'keys': [0.0, 1.0]},
                    {'parameter': 'ParamSkirtSpread', 'keys': [0.0, 1.0]},
                ],
                'keyforms': forms,
                'channels': [
                    {
                        'channel': 'OPACITY',
                        'initial_value': 0,
                        'append_axes': [
                            {'parameter': 'ParamSitPose', 'keys': [0.0, 1.0]},
                            {'parameter': 'ParamSkirtSpread', 'keys': [0.0, 1.0]},
                        ],
                        'keyforms': opacity_forms,
                    },
                ],
                'draw_order': dump['draw_order'] + 1,
                'opacity': 0,
            },
        ],
        'coordinate_contract': {
            'parent': dump['parent'],
            'mesh_vertex_count': dump['vertex_count'],
            'skirt_x_extent': [min(xs), max(xs)],
            'skirt_y_extent': [y_hem, y_hip],
            'center_x': round(cx, 6),
            'spread_x_parent_local': args.spread_x,
            'hem_drop_parent_local': args.hem_drop,
            'delta_field': 'y points up; dx=sign(x-cx)*spread_x*smooth((y_hip-y)/span); dy=-hem_drop*smooth(...)',
            'overlay_layering': 'inserted copy draws above the original (draw_order+1) and covers it at spread=1',
        },
        'authoredSitPose': None,
        'limits': {
            'max_spread_x': 0.06,
            'max_hem_drop': 0.03,
            'note': 'spread form must keep the hem at or above the floor line (808 px @840 frame)',
        },
        'provenance': {
            'producer': 'tools/prepare_skirt_spread.py',
            'producer_sha256': sha(Path(__file__)),
            'geometry_dump_sha256': sha(args.dump),
            'status': 'pending_visual_review',
            'adopted': False,
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        raise ValueError(f'Use a fresh plan output; keep earlier revisions: {args.output}')
    args.output.write_text(json.dumps(plan, indent=1) + '\n', encoding='utf8')
    print(json.dumps({'plan': str(args.output), 'vertices': dump['vertex_count'],
                      'center_x': round(cx, 4), 'spread_x': args.spread_x,
                      'hem_drop': args.hem_drop, 'draw_order': dump['draw_order'] + 1}))


if __name__ == '__main__':
    main()
