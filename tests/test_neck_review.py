import json
from pathlib import Path
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import review_neck as neck


class NeckReviewTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding='utf8')
        return path

    def fixture(self):
        return self.write('fixture.json', {'neck_crop_normalized': [0.2, 0.3, 0.5, 0.3],
            'keyframes': [{'name': 'seated', 'phase': 'seated',
                           'parameters': {'ParamBusyLaptop': 0.834999, 'ParamSitPose': 0.95}}]})

    def capture_context(self, model, raw=True):
        return {'model_manifest_path': str(model), 'raw_export_only': raw,
                'runtime_patches_applied': not raw, 'isolated_model_override': True,
                'render_backend': 'cubism_native', 'adoption': 'not_requested',
                'scope': 'static_requested_pose_native_capture', 'physics_evaluated': False}

    def test_authored_keys_preserve_raw_values_and_bind_sources_without_adopting(self):
        fixture = self.fixture()
        source = self.write('motions/working.motion.json', {'approval': 'approved',
            'keyframes': [{'time': 0.4, 'label': 'look',
                           'parameters': {'ParamBusyLaptop': 0.835001, 'ParamMouthGape': 0}}]})
        before = {path: path.read_bytes() for path in (fixture, source)}
        manifest, inputs = neck.pose_manifest(fixture, source.parent)
        self.assertEqual([row['parameters']['ParamBusyLaptop'] for row in manifest['keyframes']],
                         [0.834999, 0.835001])
        authored = manifest['keyframes'][1]
        self.assertEqual(authored['parameters']['ParamMouthGape'], 0)
        self.assertEqual(authored['source'], {'path': str(source.resolve()),
                         'sha256': neck.digest(source), 'keyframe': 0})
        self.assertEqual(authored['name'], 'working/look')
        self.assertEqual(manifest['approval'], 'pending')
        self.assertEqual(manifest['adoption'], 'not_requested')
        self.assertEqual(set(inputs), set(before))
        self.assertEqual({path: path.read_bytes() for path in before}, before)

    def test_candidate_component_masks_and_rig_are_pinned_and_input_changes_refused(self):
        for name in ('main.moc3', 'main.png', 'body.png', 'ownership.json',
                     'neck/neck.moc3', 'neck/neck.png', 'neck/neck.rig.json'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture-' + name.encode())
        model = self.write('main.model3.json', {'FileReferences': {'Moc': 'main.moc3', 'Textures': ['main.png']}})
        self.write('neck/neck.model3.json', {'FileReferences': {'Moc': 'neck.moc3', 'Textures': ['neck.png']}})
        self.write('main.psd2live.json', {'runtimeMaterialSeparation': {'bodyMask': 'body.png', 'ownership': 'ownership.json'},
            'runtimeNeckConnection': {'independentSurface': {'model': 'neck/neck.model3.json', 'rig': 'neck/neck.rig.json'}}})
        records = neck.model_inputs(model)
        self.assertTrue({str((self.root / name).resolve()) for name in
                         ('body.png', 'ownership.json', 'neck/neck.model3.json', 'neck/neck.rig.json',
                          'neck/neck.moc3', 'neck/neck.png')} <= {row['path'] for row in records})
        neck.verify_snapshot(records)
        (self.root / 'neck/neck.moc3').write_bytes(b'changed-export')
        with self.assertRaisesRegex(ValueError, 'changed.*stale'):
            neck.verify_snapshot(records)

    def test_static_evidence_requires_actual_model_and_frozen_raw_scope(self):
        model = self.root / 'candidate.model3.json'
        evidence = self.capture_context(model)
        neck.verify_capture(evidence, model, True, True, static=True)
        for field, wrong_value in [('model_manifest_path', str(self.root / 'formal.model3.json')),
                                  ('runtime_patches_applied', True), ('physics_evaluated', True),
                                  ('render_backend', 'fallback')]:
            changed = dict(evidence, **{field: wrong_value})
            with self.subTest(field=field), self.assertRaises(ValueError):
                neck.verify_capture(changed, model, True, True, static=True)

    def test_static_capture_rejects_substituted_source_pose_even_when_png_exists(self):
        manifest, _ = neck.pose_manifest(self.fixture())
        model = self.root / 'candidate.model3.json'
        image = Image.new('RGBA', (30, 30))
        image.paste((70, 80, 90, 255), (10, 10, 20, 20))
        image.save(self.root / 'neck-00.png')
        evidence = self.capture_context(model)
        evidence['captures'] = [{'index': 0, 'file': 'neck-00.png', 'frame_saved': True,
                                 'source_keyframe': manifest['keyframes'][0]}]
        self.write('neck.capture.json', evidence)
        result = neck.validate_static(self.root, manifest, model, True, True)
        self.assertEqual(result['native_poses'], 1)
        self.assertEqual(result['alpha_border_failures'], 0)
        evidence['captures'][0]['source_keyframe']['parameters']['ParamBusyLaptop'] = 1
        self.write('neck.capture.json', evidence)
        fresh_manifest, _ = neck.pose_manifest(self.root / 'fixture.json')
        with self.assertRaisesRegex(ValueError, 'source keyframe'):
            neck.validate_static(self.root, fresh_manifest, model, True, True)

    def test_invalid_numeric_pose_and_raw_runtime_scene_are_refused_before_launch(self):
        fixture = self.fixture()
        value = neck.read_json(fixture)
        value['keyframes'][0]['parameters']['ParamSitPose'] = True
        self.write('fixture.json', value)
        with self.assertRaisesRegex(ValueError, 'Invalid keyframe'):
            neck.pose_manifest(fixture)
        with self.assertRaisesRegex(ValueError, 'scenes none'):
            neck.review(raw_export=True, scenes='all')


if __name__ == '__main__':
    unittest.main()
