"""External-export Core evidence must preserve inputs and expose raw behavior."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import probe_cubism_core as probe


class CubismProbeInputTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='cubism-core-probe-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value.encode('utf8') if isinstance(value, str) else value)
        return path

    def test_pose_validation_rejects_ambiguous_or_nonfinite_evidence(self):
        for value in ([], {}, [{'name': 'same'}, {'name': 'same'}],
                      [{'parameters': {'P': True}}], [{'parameters': {'P': float('nan')}}],
                      [{'part_opacities': {'Part': float('inf')}}],
                      {'poses': [{}], 'drawables': ['M', 'M']},
                      [{'name': ''}], [{}] * (probe.MAX_POSES + 1)):
            with self.subTest(value=value), self.assertRaises(probe.ProbeError):
                probe.normalize_poses(value)
        poses, selected = probe.normalize_poses({'poses': [
            {'name': 'closed', 'parameters': {'Gape': 0}},
            {'name': 'open', 'parameters': {'Gape': 1}, 'part_opacities': {'Body': 0.4}}],
            'drawables': ['Mouth']})
        self.assertEqual(selected, ['Mouth'])
        self.assertEqual(poses[1]['part_opacities'], {'Body': 0.4})

    def test_family_provenance_covers_nested_files_and_export_metadata(self):
        model = self.write('fixture.model3.json', json.dumps({'FileReferences': {
            'Moc': 'fixture.moc3', 'Textures': ['texture.png'], 'Physics': 'physics.json',
            'DisplayInfo': 'display.json', 'Motions': {'Idle': [{'File': 'idle.motion3.json'}]},
            'Expressions': [{'Name': 'Smile', 'File': 'smile.exp3.json'}]}}))
        for name in ('fixture.moc3', 'texture.png', 'physics.json', 'display.json',
                     'idle.motion3.json', 'smile.exp3.json', 'fixture.psd2live.json', 'fixture.cmo3'):
            self.write(name, name)
        moc, family = probe.model_family(model)
        self.assertEqual(moc, (self.root / 'fixture.moc3').resolve())
        self.assertEqual(len(family), 9)
        for record in family:
            self.assertEqual(record['sha256'], probe.digest(record['path']))
        old = next(record['sha256'] for record in family if record['relative_path'] == 'texture.png')
        self.write('texture.png', b'changed texture')
        _, changed = probe.model_family(model)
        self.assertNotEqual(old, next(record['sha256'] for record in changed if record['relative_path'] == 'texture.png'))
        (self.root / 'smile.exp3.json').unlink()
        with self.assertRaisesRegex(probe.ProbeError, 'Missing model reference'):
            probe.model_family(model)

    def test_missing_core_is_explicit_and_does_not_replace_existing_evidence(self):
        model = self.write('fixture.model3.json', '{"FileReferences":{"Moc":"fixture.moc3"}}')
        self.write('fixture.moc3', b'fixture not loaded without Core')
        poses = self.write('poses.json', '[{"name":"neutral"}]')
        output = self.write('report.json', b'previous reviewed evidence')
        error = io.StringIO()
        with redirect_stderr(error), redirect_stdout(io.StringIO()):
            result = probe.main(['--model', str(model), '--poses', str(poses), '--output', str(output),
                                 '--core', str(self.root / 'missing-core.dll')])
        self.assertEqual(result, 2)
        self.assertIn('Cubism Core missing', error.getvalue())
        self.assertEqual(output.read_bytes(), b'previous reviewed evidence')

    def test_output_cannot_overwrite_any_referenced_input(self):
        model = self.write('fixture.model3.json', '{"FileReferences":{"Moc":"fixture.moc3"}}')
        self.write('fixture.moc3', b'fixture not loaded before output check')
        poses = self.write('poses.json', '[{"name":"neutral"}]')
        core = self.write('fake-core.dll', b'fixture not loaded before output check')
        before = {path: path.read_bytes() for path in (model, poses, core, self.root / 'fixture.moc3')}
        for output in before:
            with self.subTest(output=output), self.assertRaisesRegex(probe.ProbeError, 'must not overwrite'):
                probe.run_probe(model, poses, output, core_path=core)
        self.assertTrue(all(path.read_bytes() == data for path, data in before.items()))

    def test_changes_between_json_parsing_and_hash_binding_are_rejected(self):
        model = self.write('fixture.model3.json', '{"FileReferences":{"Moc":"fixture.moc3"}}')
        self.write('fixture.moc3', b'fixture not loaded before provenance check')
        poses = self.write('poses.json', '[{"name":"original"}]')
        core = self.write('fake-core.dll', b'fixture not loaded before provenance check')
        actual_digest = probe.digest
        def change_model(path):
            if Path(path) == model:
                model.write_text('{"FileReferences":{"Moc":"fixture.moc3"},"changed":true}', encoding='utf8')
            return actual_digest(path)
        with patch.object(probe, 'digest', side_effect=change_model), self.assertRaisesRegex(probe.ProbeError, 'Model JSON changed'):
            probe.model_family(model)
        def change_poses(path):
            if Path(path) == poses:
                poses.write_text('[{"name":"changed"}]', encoding='utf8')
            return actual_digest(path)
        with patch.object(probe, 'digest', side_effect=change_poses), patch.object(probe, 'NativeModel') as native:
            with self.assertRaisesRegex(probe.ProbeError, 'inputs changed while initializing'):
                probe.run_probe(model, poses, self.root / 'report.json', core_path=core)
            native.assert_not_called()
        self.assertFalse((self.root / 'report.json').exists())


class CubismProbeNativeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.core = probe.find_core()
        except probe.ProbeError as error:
            raise unittest.SkipTest(str(error))
        cls.model = ROOT / 'assets/live2d/whale-girl/whale-girl-layered-draft.model3.json'
        if not cls.model.is_file():
            raise unittest.SkipTest('Project model is unavailable; no models are installed by the test.')

    def test_actual_core_reports_clamping_raw_mouth_and_independent_pose_geometry(self):
        with tempfile.TemporaryDirectory(prefix='cubism-core-native-') as directory:
            root = Path(directory)
            poses = root / 'poses.json'
            poses.write_text(json.dumps({'drawables': ['ArtMeshFace', 'ArtMeshMouthOpen'], 'poses': [
                {'name': 'default'},
                {'name': 'out-of-range', 'parameters': {'ParamAngleX': 100000, 'ParamMouthGape': -1}},
                {'name': 'default-again'}]}), encoding='utf8')
            output = root / 'report.json'
            moc, family = probe.model_family(self.model)
            originals = {Path(item['path']): probe.digest(item['path']) for item in family}
            report = probe.run_probe(self.model, poses, output, core_path=self.core)
            self.assertTrue(report['scope']['raw_export_only'])
            self.assertFalse(report['scope']['cubism_canvas_used'])
            self.assertFalse(report['scope']['physics_evaluated'])
            self.assertTrue(report['input_hashes_verified_after_probe'])
            self.assertEqual(report['model_moc_sha256'], originals[moc])
            self.assertEqual(report['cubism_core']['sha256'], probe.digest(self.core))
            self.assertEqual(report['pose_input']['sha256'], probe.digest(poses))
            self.assertGreater(report['drawable_count'], 0)
            extreme = report['poses'][1]
            for name, bound in (('ParamAngleX', 'max'), ('ParamMouthGape', 'min')):
                parameter = extreme['parameters'][name]
                self.assertTrue(parameter['was_clamped'])
                self.assertEqual(parameter['clamped'], report['parameters'][name][bound])
                self.assertEqual(parameter['native_value'], parameter['native_input'])
            self.assertGreater(extreme['drawables']['ArtMeshFace']['max_displacement_from_default'], 0)
            for name in report['topology']:
                first = report['poses'][0]['drawables'][name]
                last = report['poses'][2]['drawables'][name]
                self.assertEqual(first['vertices_sha256'], last['vertices_sha256'])
                self.assertEqual(first['opacity'], last['opacity'])
                self.assertEqual(first['visible_flag'], last['visible_flag'])
                self.assertEqual('vertices' in first, name in report['selected_drawables'])
                if name in report['selected_drawables']:
                    topology = report['topology'][name]
                    self.assertEqual(len(first['vertices']), topology['vertex_count'])
                    self.assertEqual(len(topology['uvs']), topology['vertex_count'])
                    self.assertEqual(len(topology['indices']), topology['index_count'])
                    self.assertTrue(all(index < topology['vertex_count'] for index in topology['indices']))
            self.assertEqual(json.loads(output.read_text(encoding='utf8')), report)
            self.assertTrue(all(probe.digest(path) == expected for path, expected in originals.items()))
            # Unknown channels must never silently turn a failed binding into
            # a plausible-looking unchanged model report.
            poses.write_text('[{"parameters":{"NotBound":1}}]', encoding='utf8')
            old_report = output.read_bytes()
            with self.assertRaisesRegex(probe.ProbeError, 'unbound parameter IDs'):
                probe.run_probe(self.model, poses, output, core_path=self.core)
            self.assertEqual(output.read_bytes(), old_report)


if __name__ == '__main__':
    unittest.main()
