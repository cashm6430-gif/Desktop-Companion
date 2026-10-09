"""Synthetic checker regression probes; these are not a candidate approval."""
import copy
import importlib.util
import json
from pathlib import Path

ROOT = Path('E:/projects/Desktop-Companion')
REVIEW = ROOT / '.local/authoring/whale-head-20261009-gpu-r4'
OUTPUT = ROOT / '.local/authoring/host-behavior-20261009-r1'
spec = importlib.util.spec_from_file_location('head_check', ROOT/'tools/check_whale_head_candidate.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
contract = checker.read_json(ROOT/'art/3d/whale-girl-head-r1/rig-contract.json')
# Supply a synthetic contract matching the older captured GLB solely to exercise
# semantic checks. The real current contract intentionally rejects this review.
contract['glb_sha256'] = checker.fingerprint(REVIEW/'project/assets/character.glb')['sha256']
fixture_contract = OUTPUT/'synthetic-head-contract.json'
fixture_contract.write_text(json.dumps(contract), encoding='utf8')
receipt = checker.read_json(REVIEW/'review-receipt.json')
runtime = checker.read_json(REVIEW/'captures/report.json')
real_read = checker.read_json
cases = []

def run(name, mutate=None, expected_failure=None):
    review_receipt, review_runtime = copy.deepcopy(receipt), copy.deepcopy(runtime)
    if mutate:
        mutate(review_receipt, review_runtime)
    review_receipt['runtime'] = review_runtime
    def fake_read(path):
        if path == REVIEW/'review-receipt.json': return copy.deepcopy(review_receipt)
        if path == REVIEW/'captures/report.json': return copy.deepcopy(review_runtime)
        return real_read(path)
    checker.read_json = fake_read
    result = checker.check_review(REVIEW, fixture_contract)
    assert result['character_art_approved'] is False and result['visual_approval']=='pending'
    assert result['passed'] is (expected_failure is None), (name, result['failed_checks'])
    if expected_failure:
        assert expected_failure in result['failed_checks'], (name, result['failed_checks'])
    cases.append({'case':name,'passed':True,'failed_checks':result['failed_checks']})

run('synthetic_existing_capture_baseline')
run('recorded_headless_cannot_claim_GPU', lambda r,u:u.update(headless=True), 'actual_gpu')
run('wrong_angle_cannot_claim_left35', lambda r,u:u['captures'][1]['pose'].update(character_yaw_degrees=0), 'frozen_six_view_angles_and_capture_clock')
run('blink_cannot_move_smile_channel', lambda r,u:u['captures'][4]['pose']['blend_shapes'].update({'FaceFeatures/Smile':1}), 'isolated_runtime_expression_channels')
run('observed_focus_cannot_be_erased', lambda r,u:u['focus_events'].append({'has_focus':True}), 'focus_observations_and_guard')
run('missing_view_cannot_claim_six', lambda r,u:u['captures'].append(copy.deepcopy(u['captures'][0])), 'six_gpu_pose_inventory')
run('wrong_receipt_image_hash', lambda r,u:r['images'][0].update(sha256='0'*64), 'gpu_image_integrity_and_margins')
run('staged_renderer_mismatch', lambda r,u:r['sources']['godot/toon-prototype/prototype.gd'].update(sha256='0'*64), 'frozen_renderer_source_identity')
original_accessor = checker.Glb.accessor
def white_color_accessor(self, index):
    color_accessors = {primitive['attributes']['COLOR_0'] for mesh in self.document['meshes'] for primitive in mesh['primitives'] if 'COLOR_0' in primitive['attributes']}
    if index in color_accessors:
        return [(1.0,1.0,1.0,1.0)] * self.document['accessors'][index]['count']
    return original_accessor(self, index)
checker.Glb.accessor = white_color_accessor
run('COLOR_0_field_with_white_placeholder_is_rejected', expected_failure='actual_hair_COLOR_0_matches_height_palette_and_source_audit')
checker.Glb.accessor = original_accessor
checker.read_json = real_read
report={'status':'passed','cases':cases,'scope':'synthetic checker regressions only; current real contract still rejects older capture hash','art_approval':False}
(OUTPUT/'head-checker-negative-tests.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print(json.dumps({'status':'passed','cases':len(cases)}))
