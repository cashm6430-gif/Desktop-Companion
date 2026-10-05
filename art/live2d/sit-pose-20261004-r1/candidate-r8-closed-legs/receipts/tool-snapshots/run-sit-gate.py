"""Independent raw Core candidate gate; no rendering, runtime patch or adoption.

Run with --sit-count 4 for exporter/approved-key diagnostics, or 201 for the
dense engineering sample gate. Even 201 is not a continuous Core proof.
"""
import argparse
from datetime import datetime, timezone
import itertools
import json
from pathlib import Path
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from probe_cubism_core import NativeModel, find_core, digest

SOURCE = ROOT / '.local/authoring/whole-model-migration-20261004-r1/api-audit/moc-recovery-20261004T082620Z/export/cmo-parent-corrected.moc3'
GRAPH = SOURCE.with_name('recovered-parent-corrected-cmo-public-graph.json')
GEOMETRY = ROOT / '.local/authoring/sit-pose-20261004-r1/geometry'
BODY = [-10., -5., 0., 5., 10.]


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def area(points, tris):
    a, b, c = points[tris[:, 0]], points[tris[:, 1]], points[tris[:, 2]]
    d, e = b - a, c - a
    return d[:, 0] * e[:, 1] - d[:, 1] * e[:, 0]


def indices(model, name):
    i = model.drawable_ids.index(name)
    return np.ctypeslib.as_array(model.get('DrawableIndices')[i],
                                shape=(model.index_counts[i],)).copy().reshape(-1, 3).astype(int)


def positions(model, names):
    pointer = model.get('DrawableVertexPositions')
    return {name: np.ctypeslib.as_array(pointer[model.drawable_ids.index(name)],
             shape=(model.vertex_counts[model.drawable_ids.index(name)],)).view(
             np.float32).reshape(-1, 2).copy() for name in names}


def uvs(model, name):
    i = model.drawable_ids.index(name)
    return np.ctypeslib.as_array(model.get('DrawableVertexUvs')[i],
        shape=(model.vertex_counts[i],)).view(np.float32).reshape(-1, 2).copy()


def update(model, sit, busy, bx, by, names, part_body=1.):
    trace = model.update({'name': 'independent-sit', 'parameters': {
        'ParamSitPose': sit, 'ParamBusyLaptop': busy, 'ParamBodyAngleX': bx,
        'ParamBodyAngleY': by}, 'part_opacities': {'PartBody': part_body}})
    assert not any(row['was_clamped'] for row in trace.values()), 'Parameter clamp'
    opacity = model.get('DrawableOpacities')
    return positions(model, names), {n: float(opacity[model.drawable_ids.index(n)]) for n in names}


def pixels(points):
    return np.asarray(points, dtype=float) * [1254., -1254.] + [627., 627.]


def canonical_oriented_faces(triangles):
    # Export may cycle vertex starts and reverse all face winding to convert
    # public source-down coordinates into Native source-up coordinates.
    return sorted(min(tuple(t), tuple(np.roll(t, 1)), tuple(np.roll(t, 2)))
                  for t in triangles)


def axis_weights(keys, value):
    if value <= keys[0]:
        return [(0, 1.)]
    if value >= keys[-1]:
        return [(len(keys) - 1, 1.)]
    upper = int(np.searchsorted(keys, value))
    lower = upper - 1
    t = (value - keys[lower]) / (keys[upper] - keys[lower])
    return [(lower, 1. - t), (upper, t)]


def local_polynomial(plan, graph, parameter_contexts=None):
    """Reconstruct endpoint FloatArray sums; evaluate exact real area quadratics.

    Tests local parameter interpolation at all 25 Body contexts. World Core
    warps are separately sampled. No generator proof is imported.
    """
    nodes = {n['getId-bteaJEs']: n for n in graph['getDrawables']}
    defaults = {p['getId-WD9NFvw']: p['getDefault'] for p in graph['getParameters']}
    records = []
    for edit in list(plan.get('mesh_grids', [])) + list(plan.get('insert_meshes', [])):
        if 'keyforms' not in edit:
            # Channel-only hide edits preserve geometry and have no local
            # area interval to reconstruct.
            continue
        name = edit['id']
        inserted = name not in nodes
        if inserted:
            base = np.asarray(edit['positions'], dtype=np.float32).reshape(-1, 2)
            tris = np.asarray(edit['indices'], dtype=int).reshape(-1, 3)
            axes = [(a['parameter'], a['keys']) for a in edit['axes']]
            # Public insert() seeds a full Cartesian zero-delta grid before
            # applying the explicit rows. Front-skirt Sit0 is intentionally
            # omitted and remains its undeformed cloned mesh base.
            cells = {coord: np.zeros_like(base) for coord in itertools.product(
                *[range(len(k)) for _, k in axes])}
        else:
            node = nodes[name]
            base = np.asarray(node['getMesh']['getPositions'], dtype=np.float32).reshape(-1, 2)
            tris = np.asarray(node['getMesh']['getIndices'], dtype=int).reshape(-1, 3)
            source_axes = [(a['getParameterId-WD9NFvw'], a['getKeys']) for a in node['getGeometryGrid']['getAxes']]
            replacement = {a['parameter']: a for a in edit.get('replace_axes', [])}
            axes = [(n, replacement.get(n, {}).get('keys', k)) for n, k in source_axes]
            axes += [(a['parameter'], a['keys']) for a in edit.get('append_axes', [])]
            old_cells = {tuple(c['getCoordinate']): np.asarray(c['getForm']['getPositionDeltas'], dtype=np.float32).reshape(-1, 2)
                         for c in node['getGeometryGrid']['getCells']}
            cells = {}
            for coord in itertools.product(*[range(len(k)) for _, k in axes]):
                original = []
                for axis_index, (n, _) in enumerate(source_axes):
                    original.append(replacement[n]['source_key_indices'][coord[axis_index]] if n in replacement else coord[axis_index])
                cells[coord] = old_cells[tuple(original)]
        cells.update({tuple(c['coordinate']): np.asarray(c['position_deltas'], dtype=np.float32).reshape(-1, 2)
                      for c in edit['keyforms']})
        points = {c: (base + d).astype(np.float32).astype(float) for c, d in cells.items()}
        assert len(points) == np.prod([len(k) for _, k in axes]), (name, 'Incomplete local grid')
        if not any(n == 'ParamSitPose' for n, _ in axes):
            continue
        sit_keys = next(k for n, k in axes if n == 'ParamSitPose')
        def at(params):
            selection = [((), 1.)]
            for n, keys in axes:
                selection = [(coord + (i,), w * coefficient) for coord, w in selection
                             for i, coefficient in axis_weights(keys, params.get(n, defaults.get(n, 0.)))]
            return sum(w * points[c] for c, w in selection)
        minimum, worst, crossings = float('inf'), None, 0
        contexts = parameter_contexts or [{'ParamBusyLaptop': busy, 'ParamBodyAngleX': bx, 'ParamBodyAngleY': by}
            for busy, bx, by in itertools.product([0., 1.], BODY, BODY)]
        active = [n for n, _ in axes if n != 'ParamSitPose']
        distinct = {tuple(p.get(n, defaults.get(n, 0.)) for n in active): p for p in contexts}
        for parameters in distinct.values():
            forms = [at(parameters | {'ParamSitPose': s}) for s in sit_keys]
            baseline = area(forms[0], tris)
            assert np.all(baseline != 0), (name, 'Degenerate local Sit0')
            for segment, (left, right) in enumerate(zip(forms[:-1], forms[1:])):
                c, end, half = area(left, tris), area(right, tris), area((left + right) / 2, tris)
                qa = 2 * (end + c - 2 * half)
                qb = end - c - qa
                ratios = np.minimum(c / baseline, end / baseline)
                t = np.divide(-qb, 2 * qa, out=np.full_like(qa, np.nan), where=abs(qa) > 1e-16)
                good = (t > 0) & (t < 1)
                ratios[good] = np.minimum(ratios[good], (qa[good] * t[good] ** 2 + qb[good] * t[good] + c[good]) / baseline[good])
                crossings += int(np.count_nonzero(ratios <= 0))
                i = int(np.argmin(ratios))
                if ratios[i] < minimum:
                    minimum = float(ratios[i])
                    worst = {'parameters': parameters,
                             'sit_interval': sit_keys[segment:segment + 2], 'triangle': i,
                             'vertices': tris[i].tolist(), 'ratio': minimum}
        records.append({'mesh': name, 'minimum_signed_area_ratio': minimum,
                        'crossing_occurrences': crossings, 'worst': worst})
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--proof', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sit-count', type=int, choices=[4, 201], default=201)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    core = find_core()
    paths = [SOURCE, GRAPH, GEOMETRY / 'candidate-geometry-proof.json', args.candidate,
             args.plan, args.proof, core, Path(__file__)]
    inputs = {str(p.resolve()): {'sha256': digest(p), 'bytes': p.stat().st_size} for p in paths}
    tool_snapshot = args.output / 'gate-tool-snapshot.py'
    tool_snapshot.write_bytes(Path(__file__).read_bytes())
    plan, proof, graph = read(args.plan), read(args.proof), read(GRAPH)
    original_proof = read(GEOMETRY / 'candidate-geometry-proof.json')
    source, candidate = NativeModel(core, SOURCE), NativeModel(core, args.candidate)
    source_ids, candidate_ids = source.drawable_ids, candidate.drawable_ids
    inserted = {c['id']: c for c in plan.get('insert_meshes', [])}
    assert set(candidate_ids) == set(source_ids) | set(inserted), 'Drawable inventory differs from plan'
    topologies = {name: indices(candidate, name) for name in candidate_ids}
    original_topologies_identical = all(np.array_equal(topologies[n], indices(source, n)) for n in source_ids)
    inserted_topology = {}
    for n, record in inserted.items():
        planned = np.asarray(record['indices']).reshape(-1, 3)
        compiled = canonical_oriented_faces(topologies[n])
        same = compiled == canonical_oriented_faces(planned)
        reversed_all = compiled == canonical_oriented_faces(planned[:, ::-1])
        inserted_topology[n] = {'faces_and_counts_match_with_uniform_winding': same or reversed_all,
                               'export_uniformly_reversed_public_winding': reversed_all and not same,
                               'public_triangles': len(planned), 'core_triangles': len(topologies[n]),
                               'public_vertices': len(record['positions']) // 2,
                               'core_vertices': candidate.vertex_counts[candidate_ids.index(n)]}
    inserted_topologies_identical = all(r['faces_and_counts_match_with_uniform_winding'] and
        r['public_vertices'] == r['core_vertices'] for r in inserted_topology.values())
    parent = candidate.get('DrawableParentPartIndices')
    inserted_parts = {n: candidate.part_ids[parent[candidate_ids.index(n)]] for n in inserted}
    part_bindings = all(inserted_parts[n] == row['part_id'] for n, row in inserted.items())
    inserted_uvs = {}
    for n, row in inserted.items():
        expected_uvs = np.asarray(row['uvs'], dtype=np.float32).reshape(-1, 2).copy()
        expected_uvs[:, 1] = np.float32(1.) - expected_uvs[:, 1]
        difference = float(np.max(abs(uvs(candidate, n).astype(float) - expected_uvs.astype(float))))
        inserted_uvs[n] = {'maximum_native_public_flipped_uv_difference': difference,
            'texture_page_matches': int(candidate.get('DrawableTextureIndices')[candidate_ids.index(n)]) == row['texture_page']}
    uv_bindings = (all(np.array_equal(uvs(source, n), uvs(candidate, n)) and
        source.get('DrawableTextureIndices')[source_ids.index(n)] == candidate.get('DrawableTextureIndices')[candidate_ids.index(n)] for n in source_ids)
        and all(r['maximum_native_public_flipped_uv_difference'] <= 2e-7 and r['texture_page_matches'] for r in inserted_uvs.values()))
    contacts = {}
    stockings = {}
    rigid_shoes = []
    for child in proof.get('children', []):
        if 'heel_vertices' in child:
            contacts[child['id']] = {k: child[k] for k in ['heel_vertices', 'heel_weights', 'standing_target_id']}
        if 'length_anchors' in child:
            stockings[child['id']] = child['length_anchors']
        if child.get('rigid_all_material_vertices'):
            rigid_shoes.append(child['id'])
    assert contacts, 'Missing declared child heel anchors'
    targets = list(inserted) + ['ArtMeshBottomwear', 'ArtMeshFootwearL', 'ArtMeshFootwearR',
                                'ArtMeshObjects5', 'ArtMeshObjects6', 'ArtMeshObjects7']
    neutral, _ = update(candidate, 0., 0., 0., 0., targets)
    neutral_areas = {n: area(neutral[n].astype(float), topologies[n]) for n in targets}
    assert all(np.all(a != 0) for a in neutral_areas.values()), 'Degenerate candidate neutral geometry'
    sit_values = [0., .35, .65, 1.] if args.sit_count == 4 else [i / 200 for i in range(201)]
    local = local_polynomial(plan, graph)
    rows, contexts, zero_plane, max_contact, worst_contact = [], [], [], 0., None
    new_flips, inherited_flips, source_flips_total = 0, 0, 0
    min_ratio, old_hidden_violations, inserted_visible_zero = float('inf'), 0, 0
    minimum_stocking_ratio, minimum_stocking_130_ratio = float('inf'), float('inf')
    worst_stocking, maximum_rigid_shoe_difference, worst_rigid_shoe = None, 0., None
    hidden_from_sit = proof.get('legacy_hidden_from_native_sit', .35)
    def stocking_vector(mesh_points, anchors):
        points = pixels(mesh_points)
        ends = [np.asarray(a['weights']) @ points[a['vertices']] for a in anchors]
        return ends[1] - ends[0]
    for busy, bx, by in itertools.product([0., 1.], BODY, BODY):
        source_zero, source_opacity = update(source, 0., busy, bx, by, source_ids)
        candidate_zero, zero_opacity = update(candidate, 0., busy, bx, by, candidate_ids)
        changed = [n for n in source_ids if not np.array_equal(source_zero[n], candidate_zero[n])]
        difference = max(float(np.max(abs(source_zero[n].astype(float) - candidate_zero[n].astype(float))) * 1254) for n in source_ids)
        opacity_changed = [n for n in source_ids if source_opacity[n] != zero_opacity[n]]
        visible_zero = {n: zero_opacity[n] for n in inserted if zero_opacity[n] != 0.}
        inserted_visible_zero += len(visible_zero)
        zero_plane.append({'busy': busy, 'body_x': bx, 'body_y': by, 'changed_original_meshes': changed,
                           'maximum_vertex_difference_source_px': difference,
                           'changed_original_opacities': opacity_changed, 'inserted_visible': visible_zero})
        expected = {}
        for foot in ['ArtMeshFootwearL', 'ArtMeshFootwearR']:
            p = original_proof['footwear'][foot]
            expected[foot] = np.asarray(p['fixed_heel_weights']) @ pixels(source_zero[foot])[p['fixed_heel_vertices']]
        original_stocking_lengths = {n: float(np.linalg.norm(stocking_vector(candidate_zero[n], anchors)))
                                     for n, anchors in stockings.items()}
        context = {'busy': busy, 'body_x': bx, 'body_y': by,
                   'original_sit0_heels_source_xy': {n: p.tolist() for n, p in expected.items()},
                   'maximum_required_heel_error_source_px': 0.}
        for sit in sit_values:
            current, opacity = update(candidate, sit, busy, bx, by, targets)
            original, _ = update(source, sit, busy, bx, by, ['ArtMeshBottomwear', 'ArtMeshFootwearL', 'ArtMeshFootwearR', 'ArtMeshObjects5', 'ArtMeshObjects6', 'ArtMeshObjects7'])
            for name in list(inserted) + ['ArtMeshBottomwear']:
                ratio = area(current[name].astype(float), topologies[name]) / neutral_areas[name]
                flips = set(np.where(ratio <= 0)[0].tolist())
                inherited = set()
                if name in source_ids:
                    baseline_area = area(source_zero[name].astype(float), topologies[name])
                    original_flips = set(np.where(area(original[name].astype(float), topologies[name]) / baseline_area <= 0)[0].tolist())
                    source_flips_total += len(original_flips)
                    inherited = flips & original_flips
                new_flips += len(flips - inherited)
                inherited_flips += len(inherited)
                min_ratio = min(min_ratio, float(np.min(ratio)))
                if flips or float(np.min(ratio)) < .10:
                    rows.append({'kind': 'area', 'mesh': name, 'sit': sit, 'busy': busy,
                                 'body_x': bx, 'body_y': by, 'inverted_triangles': sorted(flips),
                                 'inherited_triangles': sorted(inherited), 'minimum_ratio': float(np.min(ratio))})
            for name, contact in contacts.items():
                actual = np.asarray(contact['heel_weights']) @ pixels(current[name])[contact['heel_vertices']]
                target = expected[contact['standing_target_id']]
                error = float(np.linalg.norm(actual - target))
                required = opacity[name] > 1e-6
                if required:
                    context['maximum_required_heel_error_source_px'] = max(context['maximum_required_heel_error_source_px'], error)
                    if error > max_contact:
                        max_contact = error
                        worst_contact = {'mesh': name, 'sit': sit, 'busy': busy, 'body_x': bx,
                            'body_y': by, 'actual_source_xy': actual.tolist(), 'target_source_xy': target.tolist(),
                            'error_source_xy': (actual - target).tolist(), 'error_source_px': error}
                if error >= .1 and required:
                    rows.append(dict(worst_contact or {}, kind='contact-failure', mesh=name, sit=sit,
                                     busy=busy, body_x=bx, body_y=by, error_source_px=error))
            for n, anchors in stockings.items():
                length = float(np.linalg.norm(stocking_vector(current[n], anchors)))
                ratio = length / original_stocking_lengths[n]
                if opacity[n] > 1e-6:
                    minimum_stocking_130_ratio = min(minimum_stocking_130_ratio, length / 130.)
                    if ratio < minimum_stocking_ratio:
                        minimum_stocking_ratio = ratio
                        worst_stocking = {'mesh': n, 'sit': sit, 'busy': busy, 'body_x': bx,
                            'body_y': by, 'length_source_px': length,
                            'same_context_sit0_length_source_px': original_stocking_lengths[n], 'ratio': ratio}
            for n in rigid_shoes:
                drift = float(np.max(np.linalg.norm(pixels(current[n]) - pixels(candidate_zero[n]), axis=1)))
                if opacity[n] > 1e-6 and drift > maximum_rigid_shoe_difference:
                    maximum_rigid_shoe_difference = drift
                    worst_rigid_shoe = {'mesh': n, 'sit': sit, 'busy': busy, 'body_x': bx,
                        'body_y': by, 'maximum_vertex_drift_source_px': drift}
            if sit >= hidden_from_sit:
                for n in ['ArtMeshFootwearL', 'ArtMeshFootwearR', 'ArtMeshObjects5', 'ArtMeshObjects6']:
                    if opacity[n] > 1e-6:
                        old_hidden_violations += 1
                        rows.append({'kind': 'legacy-visible', 'mesh': n, 'sit': sit, 'busy': busy, 'body_x': bx, 'body_y': by, 'opacity': opacity[n]})
        contexts.append(context)
    # Explicitly diagnose real runtime's final PartBody=0 selection.
    _, runtime_opacity = update(candidate, 1., 1., 0., 0., targets, part_body=0.)
    assert all(digest(Path(path)) == value['sha256'] for path, value in inputs.items()), 'Input/tool changed during gate'
    geometry_passed = (original_topologies_identical and inserted_topologies_identical and uv_bindings and part_bindings and
        not any(r['changed_original_meshes'] or r['changed_original_opacities'] for r in zero_plane) and
        not inserted_visible_zero and not old_hidden_violations and not new_flips and max_contact < .1 and
        not any(r['crossing_occurrences'] for r in local) and
        (not stockings or minimum_stocking_ratio >= .995) and
        (not rigid_shoes or maximum_rigid_shoe_difference < .1))
    report = {'schema_version': 1, 'kind': 'independent_raw_core_sit_gate',
        'created_utc': datetime.now(timezone.utc).isoformat(), 'inputs': inputs,
        'tool_snapshot': {'path': str(tool_snapshot.resolve()), 'sha256': digest(tool_snapshot)},
        'scope': {'raw_core': True, 'parameter_defaults_reset_each_sample': True,
            'sit': sit_values, 'body_x_y': BODY, 'busy': [0., 1.], 'contexts': len(contexts),
            'poses_per_model': len(contexts) * len(sit_values), 'source_original_meshes': len(source_ids),
            'inserted_meshes': list(inserted), 'geometry_targets': targets,
            'new_axes_explicit_sweeps': False, 'new_axes_state': 'Native default only',
            'core_world_continuous_proof': False, 'rendered_pixels': False, 'physics': False,
            'runtime_pose_remap_ground_collar': False,
            'local_continuous_proof': 'Float32 base+delta endpoints; real interpolation area quadratic minima over every local Sit interval at sampled Body contexts.'},
        'contact_contract': {'expected': 'Original source Footwear Sit0 barycentric heel XY in the same BodyXY/Busy context.',
                             'tolerance_euclidean_source_px': .1, 'required_when': 'Child opacity > 1e-6', 'child_anchors': contacts},
        'results': {'geometry_passed_within_scope': geometry_passed, 'original_topology_unchanged': original_topologies_identical,
            'inserted_topology_matches_plan': inserted_topologies_identical,
            'inserted_topology_export_conversion': inserted_topology, 'inserted_parts': inserted_parts,
            'part_bindings_match_plan': part_bindings, 'original_and_inserted_uv_texture_bindings_match': uv_bindings,
            'inserted_uv_texture_bindings': inserted_uvs,
            'new_inversion_occurrences': new_flips, 'inherited_inversion_occurrences': inherited_flips,
            'source_inversion_occurrences_in_comparable_targets': source_flips_total,
            'minimum_signed_area_ratio_to_neutral_sit0': min_ratio,
            'maximum_required_heel_error_source_px': max_contact, 'worst_contact': worst_contact,
            'minimum_stocking_length_ratio_to_same_context_sit0': minimum_stocking_ratio if stockings else None,
            'minimum_stocking_length_ratio_to_neutral_130px': minimum_stocking_130_ratio if stockings else None,
            'worst_stocking_length': worst_stocking,
            'maximum_rigid_shoe_vertex_drift_source_px': maximum_rigid_shoe_difference if rigid_shoes else None,
            'worst_rigid_shoe_vertex_drift': worst_rigid_shoe,
            'legacy_hidden_violations': old_hidden_violations, 'inserted_sit0_visible_count': inserted_visible_zero,
            'runtime_final_part_body_zero_opacities': runtime_opacity},
        'local_intervals': local, 'sit0_old_plane': zero_plane, 'contexts': contexts,
        'limitations': ['No pixel or visual acceptance; triangle/contact checks cannot establish anatomical quality or rear-cloth coverage.',
            'Stocking length/ankle angle use separately declared anchors; no triangle inversion is not a visual acceptance criterion.',
            'Raw Core PartBody=1 samples are author geometry; final PartBody=0 is diagnosed separately, not a full Qt render.'],
        'visual_review': 'pending', 'adoption': 'pending'}
    for name, value in [('summary.json', report), ('failures.json', rows)]:
        (args.output / name).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf8')
    print(json.dumps({'output': str(args.output), 'poses_per_model': len(contexts) * len(sit_values), 'results': report['results']}, indent=2))


if __name__ == '__main__':
    main()
