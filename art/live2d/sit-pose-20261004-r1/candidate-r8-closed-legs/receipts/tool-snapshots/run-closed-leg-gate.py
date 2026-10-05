"""Independent R8 closed-leg Core contract and standing-slice gate.

No renderer, physics, part patch, floor matrix or adoption. Dense feet geometry
uses Busy0; Busy0/1 original standing slices and explicit approved poses are
separately checked. Shoes follow the pinned author horizontal-target contract;
the contact tolerance is unchanged. Local geometry proves no Sit area crossing at the declared
ancestor samples; Core world interpolation is densely sampled, not proved.
"""
import argparse
from datetime import datetime, timezone
import importlib.util
import itertools
import json
from pathlib import Path
import sys
import time
import numpy as np

HERE = Path(__file__).resolve().parent
HELPER = HERE / 'run-sit-gate.py'
spec = importlib.util.spec_from_file_location('independent_sit_helpers', HELPER)
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)
ROOT = h.ROOT
BODY = [-10., -5., 0., 5., 10.]
BREATH = [0., .25, .5, .75, 1.]
SITS = [i / 200 for i in range(201)]
FEET = ['ArtMeshFootwearL', 'ArtMeshFootwearR']
NEW_LEGS = ['ArtMeshSitKneeFrontL', 'ArtMeshSitStockingL', 'ArtMeshSitShoeL',
            'ArtMeshSitKneeFrontR', 'ArtMeshSitStockingR', 'ArtMeshSitShoeR']
PROOF = ROOT / '.local/authoring/sit-pose-20261004-r1/geometry/folded-legs-proof-r4.json'
CONTRACT = h.GEOMETRY / 'r8-closed-leg-contact-contract.json'
AUTHOR_PLAN = h.GEOMETRY / 'full-ancestor-closed-legs-r8-plan.json'
AUTHOR_PROOF = h.GEOMETRY / 'full-ancestor-closed-legs-r8-proof.json'
CONTRACT_SHA = '66fe33c0ad50b4827af6b3c79a73265a2361a1c9cef99b81aef4e92100f52fec'
AUTHOR_PLAN_SHA = 'f0d170fa0e28c5495352428827e395f5d467030d2f955afa3a3acf8eca9ecdfc'
FIXTURE = HERE / 'r8-closed-leg-core-fixture.json'


def validate_contract(contract):
    """Validate the actual author profile; do not source gate limits from it."""
    assert contract['schema_version'] == 1
    assert contract['kind'] == 'authored_closed_knees_r8_ground_contact_contract'
    assert contract['units'] == 'source pixel, x right and y down on1254x1254 canvas'
    assert contract['logical_to_native'] == 'identity'
    assert contract['standing_Sit0_original_34_geometry_and_opacity_float32_bit_exact'] is True
    assert contract['R5_nonzero_default_slice_preservation_required'] is False
    shoe = contract['shoe_translation']
    keys = np.asarray(shoe['keys'], dtype=float)
    assert keys.ndim == 1 and len(keys) >= 2 and np.all(np.isfinite(keys))
    assert keys[0] == 0. and keys[-1] == 1. and np.all(np.diff(keys) > 0.)
    assert shoe['interpolation'] == 'piecewise linear in native ParamSitPose'
    assert shoe['dy_source_pixels'] == 0.
    assert shoe['heel_error_max_source_pixels'] == .1
    assert shoe['whole_shoe_vertex_error_max_source_pixels'] == .1
    assert set(shoe['by_side']) == {'L', 'R'}
    for values in shoe['by_side'].values():
        assert len(values) == len(keys) and np.all(np.isfinite(values)) and values[0] == 0.
    expected = {'ArtMeshSitShoeL': 'ArtMeshFootwearL', 'ArtMeshSitShoeR': 'ArtMeshFootwearR'}
    assert len(shoe['drawables']) == 2
    assert {r['id']: r['standing_target_id'] for r in shoe['drawables']} == expected
    assert contract['stocking_rotation']['same_context_Sit0_centerline_minimum_ratio'] == .995
    floor = contract['hidden_runtime_floor_reference']
    assert floor['ids'] == FEET and floor['world_X_not_modified'] is True
    assert floor['min_native_Y_drift_source_pixels_max'] == .1
    return contract


def horizontal_target(contract, mesh, sit):
    assert mesh in ['ArtMeshSitShoeL', 'ArtMeshSitShoeR'] and 0. <= sit <= 1.
    profile = contract['shoe_translation']
    return np.asarray([float(np.interp(sit, profile['keys'], profile['by_side'][mesh[-1]])), 0.])


def shoe_errors(actual, baseline, heel_vertices, heel_weights, formal_heel, shift):
    """All arguments are world source pixels, including explicit target shift."""
    expected_vertices = baseline + shift
    errors = np.linalg.norm(actual - expected_vertices, axis=1)
    heel = np.asarray(heel_weights) @ actual[heel_vertices]
    expected_heel = formal_heel + shift
    return float(np.linalg.norm(heel - expected_heel)), float(errors.max()), int(errors.argmax()), heel, expected_heel


def geometry_binding(actual, author):
    """Check the merged input retains the exact authored feet and ancestors."""
    rows = []
    fields = ['positions', 'uvs', 'indices', 'axes', 'keyforms', 'parent_id', 'part_id', 'texture_page']
    by_id = lambda plan, section: {r['id']: r for r in plan[section]}
    a, b = by_id(actual, 'insert_meshes'), by_id(author, 'insert_meshes')
    for name in NEW_LEGS:
        mismatch = [field for field in fields if a[name][field] != b[name][field]]
        rows.append({'id': name, 'section': 'insert_meshes', 'mismatched_fields': mismatch, 'passed': not mismatch})
    a, b = by_id(actual, 'mesh_grids'), by_id(author, 'mesh_grids')
    for name in FEET:
        equal = a[name] == b[name]
        rows.append({'id': name, 'section': 'mesh_grids', 'passed': equal})
    # Every source ancestor edit must remain exact; a new ancestor edit would
    # change the world target and is not an arm-only merge.
    same_deformers = actual['deformer_grids'] == author['deformer_grids']
    return {'rows': rows, 'all_deformer_edits_exact': same_deformers,
            'passed': all(r['passed'] for r in rows) and same_deformers}


def opacity_bits_equal(a, b):
    return np.float32(a).view(np.uint32) == np.float32(b).view(np.uint32)


def update(model, parameters, names):
    trace = model.update({'name': 'full-ancestor', 'parameters': parameters,
                          'part_opacities': {}})
    assert not any(t['was_clamped'] for t in trace.values()), 'Unexpected clamp'
    pointer = model.get('DrawableOpacities')
    positions = h.positions(model, names)
    opacity = {n: float(pointer[model.drawable_ids.index(n)]) for n in names}
    assert all(np.all(np.isfinite(p)) for p in positions.values()), 'Non-finite geometry'
    assert all(np.isfinite(o) for o in opacity.values()), 'Non-finite opacity'
    return positions, opacity


def vector(points, anchors):
    p = h.pixels(points)
    ends = [np.asarray(a['weights']) @ p[a['vertices']] for a in anchors]
    return ends[1] - ends[0]


def inventory(source, candidate, plan):
    inserted = {m['id']: m for m in plan['insert_meshes']}
    assert set(candidate.drawable_ids) == set(source.drawable_ids) | set(inserted), 'Unexpected drawable inventory'
    old_topology = all(np.array_equal(h.indices(source, n), h.indices(candidate, n)) for n in source.drawable_ids)
    old_uvs = all(np.array_equal(h.uvs(source, n), h.uvs(candidate, n)) and
        source.get('DrawableTextureIndices')[source.drawable_ids.index(n)] == candidate.get('DrawableTextureIndices')[candidate.drawable_ids.index(n)] for n in source.drawable_ids)
    rows = {}
    parents = candidate.get('DrawableParentPartIndices')
    for n, m in inserted.items():
        planned = np.asarray(m['indices']).reshape(-1, 3)
        actual = h.canonical_oriented_faces(h.indices(candidate, n))
        same = actual == h.canonical_oriented_faces(planned)
        reverse = actual == h.canonical_oriented_faces(planned[:, ::-1])
        uv = np.asarray(m['uvs'], dtype=np.float32).reshape(-1, 2).copy()
        uv[:, 1] = np.float32(1.) - uv[:, 1]
        i = candidate.drawable_ids.index(n)
        row = {'face_inventory_uniform_winding': same or reverse,
            'public_to_native_uniform_winding_reversal': reverse and not same,
            'vertex_count_matches': candidate.vertex_counts[i] == len(m['positions']) // 2,
            'maximum_uv_error': float(np.max(abs(h.uvs(candidate, n).astype(float) - uv.astype(float)))),
            'texture_page_matches': int(candidate.get('DrawableTextureIndices')[i]) == m['texture_page'],
            'part_matches': candidate.part_ids[parents[i]] == m['part_id']}
        row['passed'] = row['face_inventory_uniform_winding'] and row['vertex_count_matches'] and row['maximum_uv_error'] <= 2e-7 and row['texture_page_matches'] and row['part_matches']
        rows[n] = row
    return {'source_meshes': len(source.drawable_ids), 'original_topologies_exact': old_topology,
            'original_uv_texture_pages_exact': old_uvs, 'inserted': rows,
            'passed': old_topology and old_uvs and all(r['passed'] for r in rows.values())}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--contract', type=Path, default=CONTRACT)
    parser.add_argument('--fixture', type=Path, default=FIXTURE)
    parser.add_argument('--phase', choices=['targeted', 'full'], default='full')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    core = h.find_core()
    files = [h.SOURCE, h.GRAPH, core, PROOF, AUTHOR_PLAN, AUTHOR_PROOF,
             args.contract, args.fixture, args.candidate, args.plan, Path(__file__), HELPER,
             ROOT / 'tools/probe_cubism_core.py']
    pins = {str(p.resolve()): {'sha256': h.digest(p), 'bytes': p.stat().st_size} for p in files}
    for p in [Path(__file__), HELPER]:
        (args.output / p.name).write_bytes(p.read_bytes())
    plan, graph, proof = h.read(args.plan), h.read(h.GRAPH), h.read(PROOF)
    assert h.digest(args.contract) == CONTRACT_SHA, 'Unreviewed contact profile'
    assert h.digest(AUTHOR_PLAN) == AUTHOR_PLAN_SHA, 'Wrong author geometry profile'
    contract = validate_contract(h.read(args.contract))
    author_proof, fixture = h.read(AUTHOR_PROOF), h.read(args.fixture)
    assert author_proof['contact_contract_sha256'] == CONTRACT_SHA
    assert fixture['contact_contract_sha256'] == CONTRACT_SHA
    assert fixture['thresholds'] == {'heel_max_px': .1, 'whole_shoe_max_px': .1,
                                     'stocking_min_ratio': .995, 'hidden_floor_max_px': .1}
    geometry = geometry_binding(plan, h.read(AUTHOR_PLAN))
    assert geometry['passed'], 'Merged plan differs from pinned author legs/ancestors'
    support_keys = author_proof['sit_support_keys']
    assert support_keys == sorted(set(support_keys)) and support_keys[0] == 0. and support_keys[-1] == 1.
    intervals_mid = [(a + b) * .5 for a, b in zip(support_keys, support_keys[1:])]
    sits = sorted(set(SITS + support_keys + intervals_mid))
    busy_keys = [0., .35, .65, .8, .9, 1.]
    source, candidate = h.NativeModel(core, h.SOURCE), h.NativeModel(core, args.candidate)
    inv = inventory(source, candidate, plan)
    contexts = [{'ParamBodyAngleX': x, 'ParamBodyAngleY': y,
                 'ParamBodyAngleZ': z, 'ParamBreath': b}
                for x, y, z, b in itertools.product(BODY, BODY, BODY, BREATH)]
    if args.phase == 'targeted':
        contexts = fixture['targeted_contexts']
    local = h.local_polynomial(plan, graph, [p | {'ParamBusyLaptop': busy} for p in contexts for busy in [0., 1.]]) if args.phase == 'full' else []
    print(json.dumps({'stage': 'local_polynomial_complete', 'contexts': len(contexts),
                      'crossings': sum(x['crossing_occurrences'] for x in local)}), flush=True)
    contacts = {c['id']: c for c in proof['children'] if 'heel_vertices' in c}
    stockings = {c['id']: c['length_anchors'] for c in proof['children'] if 'length_anchors' in c}
    foot_proof = h.read(h.GEOMETRY / 'candidate-geometry-proof.json')['footwear']
    inserted = list(inv['inserted'])
    targets = NEW_LEGS + FEET
    topologies = {n: h.indices(candidate, n) for n in NEW_LEGS}
    zero, _ = update(candidate, {}, NEW_LEGS)
    neutral_areas = {n: h.area(zero[n].astype(float), topologies[n]) for n in NEW_LEGS}
    assert all(np.all(a != 0) for a in neutral_areas.values()), 'Degenerate neutral new leg'
    maxheel, maxshoe, minlength, maxfloor = 0., 0., float('inf'), 0.
    heel_worst, shoe_worst, length_worst, floor_worst = None, None, None, None
    inversions, minimum_area, maximum_standing_delta = 0, float('inf'), 0.
    zero_failures, failures, per_context, busy_failures = [], [], [], []
    zero_checks, busy_checks = 0, 0
    t0 = time.monotonic()
    for context_index, p in enumerate(contexts):
        for busy in [0., 1.]:
            parameters = p | {'ParamSitPose': 0., 'ParamBusyLaptop': busy}
            original, old_opacity = update(source, parameters, source.drawable_ids)
            current, current_opacity = update(candidate, parameters, candidate.drawable_ids)
            geometry_changed = [n for n in source.drawable_ids if not np.array_equal(original[n].view(np.uint32), current[n].view(np.uint32))]
            opacity_changed = [n for n in source.drawable_ids if not opacity_bits_equal(old_opacity[n], current_opacity[n])]
            visible_new = [n for n in inserted if current_opacity[n] != 0.]
            difference = max(float(np.max(abs(original[n].astype(float) - current[n].astype(float))) * 1254.) for n in source.drawable_ids)
            maximum_standing_delta = max(maximum_standing_delta, difference)
            zero_checks += 1
            if geometry_changed or opacity_changed or visible_new:
                zero_failures.append({'parameters': parameters, 'geometry_changed': geometry_changed,
                                      'opacity_changed': opacity_changed, 'visible_new': visible_new,
                                      'maximum_vertex_difference_source_px': difference})
            if busy == 0:
                baseline, baseline_original = current, original
        expected = {foot: np.asarray(foot_proof[foot]['fixed_heel_weights']) @ h.pixels(baseline_original[foot])[foot_proof[foot]['fixed_heel_vertices']] for foot in FEET}
        lengths = {n: float(np.linalg.norm(vector(baseline[n], a))) for n, a in stockings.items()}
        baseline_floor = min(float(baseline[n][:, 1].min()) for n in FEET)
        context_results = {'parameters': p, 'maximum_heel_error_source_px': 0.,
            'maximum_shoe_vertex_error_to_authored_target_source_px': 0., 'minimum_stocking_length_ratio': float('inf'),
            'maximum_hidden_legacy_floor_minY_error_source_px': 0.}
        for sit in sits:
            current, opacity = update(candidate, p | {'ParamSitPose': sit, 'ParamBusyLaptop': 0.}, targets)
            required = sit > 0.
            for n in NEW_LEGS:
                ratios = h.area(current[n].astype(float), topologies[n]) / neutral_areas[n]
                count = int(np.count_nonzero(ratios <= 0.))
                inversions += count
                minimum_area = min(minimum_area, float(ratios.min()))
                if count:
                    failures.append({'kind': 'inversion', 'mesh': n, 'parameters': p | {'ParamSitPose': sit}, 'count': count})
            for n, anchor in contacts.items():
                shift = horizontal_target(contract, n, sit)
                error, drift, vertex, heel, expected_heel = shoe_errors(
                    h.pixels(current[n]), h.pixels(baseline[n]), anchor['heel_vertices'],
                    anchor['heel_weights'], expected[anchor['standing_target_id']], shift)
                if required:
                    context_results['maximum_heel_error_source_px'] = max(context_results['maximum_heel_error_source_px'], error)
                    context_results['maximum_shoe_vertex_error_to_authored_target_source_px'] = max(context_results['maximum_shoe_vertex_error_to_authored_target_source_px'], drift)
                    if error > maxheel:
                        maxheel = error
                        heel_worst = {'mesh': n, 'parameters': p | {'ParamSitPose': sit}, 'error_source_px': error,
                                      'authored_translation_source_xy': shift.tolist(),
                                      'actual_source_xy': heel.tolist(), 'expected_source_xy': expected_heel.tolist()}
                    if drift > maxshoe:
                        maxshoe = drift
                        shoe_worst = {'mesh': n, 'parameters': p | {'ParamSitPose': sit},
                                      'maximum_vertex_error_to_authored_target_source_px': drift,
                                      'authored_translation_source_xy': shift.tolist(), 'worst_vertex': vertex,
                                      'actual_source_xy': h.pixels(current[n])[vertex].tolist(),
                                      'expected_source_xy': (h.pixels(baseline[n])[vertex] + shift).tolist()}
            for n, anchors in stockings.items():
                ratio = float(np.linalg.norm(vector(current[n], anchors))) / lengths[n]
                if required:
                    context_results['minimum_stocking_length_ratio'] = min(context_results['minimum_stocking_length_ratio'], ratio)
                    if ratio < minlength:
                        minlength = ratio
                        length_worst = {'mesh': n, 'parameters': p | {'ParamSitPose': sit}, 'ratio': ratio,
                                        'baseline_length_source_px': lengths[n]}
            floor_error = abs(min(float(current[n][:, 1].min()) for n in FEET) - baseline_floor) * 1254.
            context_results['maximum_hidden_legacy_floor_minY_error_source_px'] = max(context_results['maximum_hidden_legacy_floor_minY_error_source_px'], floor_error)
            if floor_error > maxfloor:
                maxfloor = floor_error
                floor_worst = {'parameters': p | {'ParamSitPose': sit}, 'minY_error_source_px': floor_error}
            if sit in busy_keys:
                busy, _ = update(candidate, p | {'ParamSitPose': sit, 'ParamBusyLaptop': 1.}, targets)
                busy_checks += 1
                changed = [n for n in targets if not np.array_equal(current[n], busy[n])]
                if changed:
                    busy_failures.append({'parameters': p | {'ParamSitPose': sit}, 'changed_geometry': changed})
        per_context.append(context_results)
        if (context_index + 1) % 25 == 0:
            print(json.dumps({'stage': 'core', 'contexts_done': context_index + 1,
                'contexts_total': len(contexts), 'elapsed_seconds': round(time.monotonic() - t0, 1),
                'max_heel_px': maxheel, 'max_shoe_px': maxshoe, 'min_length_ratio': minlength}), flush=True)
    assert all(h.digest(Path(p)) == row['sha256'] for p, row in pins.items()), 'Input or tool changed during gate'
    results = {'inventory_bindings_passed': inv['passed'], 'original_sit0_bitexact': not zero_failures,
        'original_sit0_opacity_float32_bits_exact': not any(r['opacity_changed'] for r in zero_failures),
        'original_sit0_opacity_value_comparisons': zero_checks * len(source.drawable_ids),
        'maximum_original_sit0_vertex_difference_source_px': maximum_standing_delta,
        'sit0_checks_busy0_and1': zero_checks, 'busy_geometry_keypose_checks': busy_checks,
        'busy_geometry_changed_checks': len(busy_failures), 'new_leg_inversion_occurrences': inversions,
        'minimum_new_leg_signed_area_ratio_to_neutral_sit0': minimum_area,
        'local_sit_interval_crossing_occurrences': sum(r['crossing_occurrences'] for r in local) if args.phase == 'full' else None,
        'maximum_shoe_heel_error_source_px': maxheel, 'worst_heel': heel_worst,
        'maximum_shoe_vertex_error_to_authored_target_source_px': maxshoe, 'worst_shoe_vertex': shoe_worst,
        'minimum_stocking_length_ratio_to_same_context_sit0': minlength, 'worst_stocking': length_worst,
        'maximum_hidden_footwear_minY_difference_to_same_context_sit0_px': maxfloor, 'worst_floor': floor_worst}
    passed = inv['passed'] and geometry['passed'] and not zero_failures and not busy_failures and not inversions and (args.phase == 'targeted' or not results['local_sit_interval_crossing_occurrences']) and maxheel < .1 and maxshoe < .1 and minlength >= .995 and maxfloor < .1
    report = {'schema_version': 1, 'kind': 'independent_closed_leg_authored_target_raw_core_gate',
        'created_utc': datetime.now(timezone.utc).isoformat(), 'inputs': pins,
        'tool_snapshots': [{'path': str((args.output / p.name).resolve()), 'sha256': h.digest(args.output / p.name)} for p in [Path(__file__), HELPER]],
        'scope': {'phase': args.phase, 'body_x_y_z': BODY, 'breath': BREATH, 'sit': sits,
            'ancestor_contexts': len(contexts), 'primary_201_sit_poses': len(contexts) * len(SITS),
            'all_poses_with_author_keys_and_midpoints': len(contexts) * len(sits),
            'dense_busy': 0., 'busy1_checked': 'All declared ancestor contexts at Sit0/.35/.65/.8/.9/1; original Sit0 opacity and geometry also compared for Busy0/1.',
            'targeted_contexts': contexts if args.phase == 'targeted' else None,
            'new_legs_core_area_targets': NEW_LEGS, 'old_standing_meshes': len(source.drawable_ids),
            'new_hand_ground_and_skirt_spread': 'Native defaults only; no arm domain expansion.',
            'shift_x_y': 'Native defaults only', 'physics': False, 'rendered_pixels': False,
            'runtime_floor_or_collar_transform': False,
            'continuous_world_core_proof': False, 'continuous_local_sit_proof': 'Real quadratic signed area extrema over float32 base+delta endpoints at every declared ancestor context.' if args.phase == 'full' else False},
        'contact_contract': contract, 'merged_geometry_binding': geometry,
        'inventory': inv, 'local_intervals': local, 'results': results,
        'engineering_passed_within_scope': passed, 'visual_review': 'pending', 'adoption': 'pending',
        'limits': ['No rendered pixel, knee/cuff/wrist alpha or arm interaction approval.',
            'Samples do not prove arbitrary real Body/Breath values or physics-driven combinations.',
            'Full common global floor translation belongs to the runtime check and is absent here.']}
    for name, value in [('summary.json', report), ('contexts.json', per_context), ('sit0-failures.json', zero_failures), ('area-failures.json', failures), ('busy-geometry-failures.json', busy_failures)]:
        (args.output / name).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf8')
    print(json.dumps({'output': str(args.output), 'engineering_passed_within_scope': passed, 'results': results}, indent=2), flush=True)


if __name__ == '__main__':
    main()
