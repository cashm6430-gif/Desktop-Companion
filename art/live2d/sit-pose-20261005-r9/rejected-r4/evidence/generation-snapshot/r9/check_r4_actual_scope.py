"""Actual R4 Core evidence with the rejected old-contract result preserved.

The existing strict audit supplies standing and the complete 201-Sit sweep.
Its input hashes are checked before its results are reused. This script makes
additional actual-Core queries for cranium, folded hair and preserved shoes.
Neither a new 3D leg contract nor a visual/model-adoption gate is invented.
"""
from pathlib import Path
import argparse
import itertools
import json
import numpy as np
from check_structural_prototype import RawModel, SOURCE, ROOT, find_core, digest, fit_rigid

CRANIUM = ['ArtMeshFace', 'ArtMeshFrontHair', 'ArtMeshHeadwear']
HAIR = ['ArtMeshBackHair2', 'ArtMeshBackHair']
SHOES = ['ArtMeshSitShoeL', 'ArtMeshSitShoeR']
FLOOR = ['ArtMeshAuthorFloorReferenceL', 'ArtMeshAuthorFloorReferenceR']


def bbox(points):
    return [*points.min(0).tolist(), *points.max(0).tolist()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--old-audit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    target = args.model.resolve(strict=True)
    audit_path = args.old_audit.resolve(strict=True)
    out = args.output.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'.local'):
        raise ValueError('Fresh .local report required.')
    audit = json.loads(audit_path.read_text(encoding='utf8'))
    if Path(audit['candidate_model']) != target or Path(audit['source_model']) != SOURCE:
        raise ValueError('Strict audit refers to another candidate or source.')
    pins = dict(audit['inputs'])
    pins[str(audit_path)] = digest(audit_path)
    pins[str(Path(__file__).resolve())] = digest(Path(__file__))
    if any(digest(Path(path)) != signature for path, signature in pins.items()):
        raise ValueError('Old actual-Core audit inputs are stale.')
    core = find_core()
    source, candidate = RawModel(SOURCE, core), RawModel(target, core)
    if source.ppu != candidate.ppu or not np.array_equal(source.origin, candidate.origin):
        raise ValueError('Actual source/candidate canvas convention changed.')

    source.update({'ParamSitPose': 0.})
    base = {name: source.world(name).copy() for name in CRANIUM}
    cranium = []
    for sit in [.65, .8]:
        candidate.update({'ParamSitPose': sit})
        pose = {name: candidate.world(name).copy() for name in CRANIUM}
        common = fit_rigid(np.concatenate(list(base.values())), np.concatenate(list(pose.values())))
        passed = common['best_rigid_maximum_source_px'] <= .01 and abs(common['diagnostic_best_uniform_scale']-1) <= 1e-4
        cranium.append({'parameters': {'ParamSitPose': sit}, 'actual_native_ids': CRANIUM,
                        'best_common_rigid_fit': common,
                        'per_surface_fits': {name: fit_rigid(base[name], pose[name]) for name in CRANIUM},
                        'passed': bool(passed)})

    hair_rows, support_rows = [], []
    hair_max = None
    support_worst = None
    for sit in [.65, .8]:
        # New fold was authored for these HeadXY keys and default HeadZ/Body.
        # Hair key tests do not certify cranial rigidity at all head angles.
        for angle_x, angle_y, hair in itertools.product([-45., 0., 45.], [-30., 0., 30.], [-1., 0., 1.]):
            p = {'ParamSitPose': sit, 'ParamAngleX': angle_x, 'ParamAngleY': angle_y, 'ParamHairBack': hair}
            candidate.update(p)
            for name in HAIR:
                points = candidate.world(name)
                row = {'parameters': p, 'drawable': name, 'raw_all_vertex_bbox_source_px': bbox(points),
                       'lowest_world_y_source_px': float(points[:, 1].max()),
                       'finite': bool(np.all(np.isfinite(points))),
                       'cap1180_passed': bool(np.all(np.isfinite(points)) and points[:, 1].max() <= 1180.)}
                hair_rows.append(row)
                if hair_max is None or row['lowest_world_y_source_px'] > hair_max['lowest_world_y_source_px']:
                    hair_max = row
        p = {'ParamSitPose': sit}
        source.update(p); candidate.update(p)
        for name in SHOES+FLOOR:
            a, b = source.world(name), candidate.world(name)
            if a.shape != b.shape: raise ValueError('Preserved support topology changed '+name)
            delta = np.linalg.norm(a-b, axis=1)
            i = int(delta.argmax())
            row = {'parameters': p, 'drawable': name,
                   'source_raw_all_vertex_bbox_source_px': bbox(a), 'candidate_raw_all_vertex_bbox_source_px': bbox(b),
                   'source_lowest_world_y_source_px': float(a[:, 1].max()),
                   'candidate_lowest_world_y_source_px': float(b[:, 1].max()),
                   'maximum_same_context_source_world_error_px': float(delta[i]), 'worst_vertex': i,
                   'maximum_allowed_error_source_px': .1, 'passed': bool(delta[i] <= .1)}
            support_rows.append(row)
            if support_worst is None or row['maximum_same_context_source_world_error_px'] > support_worst['maximum_same_context_source_world_error_px']:
                support_worst = row
    sweep = audit['neutral_positive_sit_sweep']
    winding_pass = not sweep['nonfinite'] and not sweep['new_zero_triangle_count'] and not sweep['orientation_changes_vs_source_same_pose']
    passed = (audit['standing']['passed'] and all(row['passed'] for row in cranium)
              and all(row['cap1180_passed'] for row in hair_rows)
              and all(row['passed'] for row in support_rows) and winding_pass)
    if any(digest(Path(path)) != signature for path, signature in pins.items()):
        raise ValueError('Actual model, strict evidence or inputs changed during scope check.')
    report = {'schema_version': 1, 'kind': 'R4_actual_Core_scoped_evidence_not_overall_model_gate',
              'candidate_model': str(target), 'source_model': str(SOURCE), 'input_sha256': pins,
              'scoped_checks_passed': bool(passed), 'overall_model_pass': False,
              'strict_old_contract': {'evidence_path': str(audit_path), 'sha256': digest(audit_path),
                  'passed': audit['passed_within_declared_scope'],
                  'old_eight_leg_world_contract': audit['leg_floor_neutral'],
                  'old_four_surface_common_rigid_contract': audit['head_neutral_only'],
                  'failures_preserved': True, 'tolerance_changed': False,
                  'explanation': 'Projected knee/stocking author poses changed old world targets; BackHair2 is now deliberately folded. Neither old failure is converted to a pass.'},
              'standing_original34_and_all_new_hidden': audit['standing'],
              'cranium_only_rigid_neutral': {'ids': CRANIUM, 'max_residual_px': .01,
                  'uniform_scale_tolerance': 1e-4, 'rows': cranium,
                  'folded_hair_excluded_from_this_new_scope': HAIR,
                  'old_four_surface_result_is_preserved_above': True, 'head_angle_rigidity_tested': False},
              'actual_hair_deep_bbox': {'checked_pose_count': 54, 'rows': hair_rows, 'worst_lowest_world_y': hair_max,
                  'source_pixel_y_direction': 'down', 'allowed_deep_raw_vertex_cap_y': 1180.,
                  'BodyXYZ_Breath_Shift_and_HeadZ': 'Native defaults',
                  'visible_alpha_contour_or_renderer_crop_certified': False},
              'shoe_and_hidden_floor_preservation_neutral': {'rows': support_rows, 'worst': support_worst,
                  'expected': 'Actual frozen source8 same Sit and neutral-body context, complete raw shoe/floor vertices.',
                  'Core_support_trajectory_preserved': all(row['passed'] for row in support_rows),
                  'common_floor_MVP_visual_contact_certified': False},
              'actual_positive_sit_winding': {'evidence_path': str(audit_path), **sweep, 'passed': winding_pass},
              'pending_contracts': ['New projected 3D knee/stocking length, UV/textile deformation and full-domain authored pose contract.',
                  'Native alpha contour, hair appearance, apron/fold layering, skirt/knee contact and source-source seams.',
                  'Full-domain Body/Arm/Typing/Rock/head angles/physics and stable whole-model regression.'],
              'approval': {'Native_visual': 'pending_root_review', 'adoption': 'not_adopted'}}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf8')
    print(json.dumps({'report': str(out), 'scoped_checks_passed': passed, 'old_contract_passed': audit['passed_within_declared_scope'],
                      'standing_poses': audit['standing']['poses'], 'cranium_worst_px': max(x['best_common_rigid_fit']['best_rigid_maximum_source_px'] for x in cranium),
                      'actual_hair_max_y': hair_max['lowest_world_y_source_px'],
                      'support_worst_error_px': support_worst['maximum_same_context_source_world_error_px'],
                      'positive_Sit_samples': sweep['sample_count'], 'overall_model_pass': False}, indent=2))
    if not passed: raise SystemExit(1)


if __name__ == '__main__':
    main()
