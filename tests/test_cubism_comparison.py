"""Roundtrip comparisons need current exports and actual raw geometry evidence."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import compare_cubism_probes as comparison


class CubismComparisonTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cubism-comparison-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.core = self.root / 'core.dll'
        self.core.write_bytes(b'test Core binary')
        self.poses = self.root / 'poses.json'
        self.poses.write_text(json.dumps({'poses': [{'name': 'default', 'parameters': {}}]}), encoding='utf8')

    def exported_report(self, name, moc_bytes=b'exported moc', vertices=None, selected=True, opacity=1, compress=9):
        directory = self.root / name
        directory.mkdir()
        moc = directory / 'model.moc3'
        moc.write_bytes(moc_bytes)
        atlas = directory / 'atlas.png'
        with Image.new('RGBA', (8, 8), (12, 34, 56, 200)) as image:
            image.save(atlas, compress_level=compress)
        model = directory / 'model.model3.json'
        model.write_text(json.dumps({'FileReferences': {'Moc': moc.name, 'Textures': [atlas.name]}}), encoding='utf8')
        values = vertices or [[0, 0], [0.1, 0], [0, 0.1]]
        uvs, indices = [[0, 0], [1, 0], [0, 1]], [0, 1, 2]
        topology = {'index': 0, 'vertex_count': 3, 'index_count': 3, 'texture_index': 0,
                    'parent_part_index': 0, 'parent_part': 'PartBody', 'constant_flags': 0, 'masks': [],
                    'default_opacity': 1, 'uvs_sha256': comparison.buffer_hash(uvs, 'f', 'little'),
                    'indices_sha256': comparison.buffer_hash(indices, 'H', 'little')}
        drawable = {'opacity': opacity, 'visible_flag': opacity > 0, 'dynamic_flags': 1,
                    'draw_order': 0, 'render_order': 0, 'vertices_sha256': comparison.buffer_hash(values, 'f', 'little')}
        if selected:
            topology.update(uvs=uvs, indices=indices)
            drawable['vertices'] = values
        else:
            # The production probe always selects at least one drawable.
            # A second identical raw guard makes this mesh genuinely omitted.
            pass
        report = {'schema_version': 1, 'kind': 'cubism_core_export_probe', 'status': 'complete',
                  'scope': {'raw_export_only': True, 'cubism_canvas_used': False, 'physics_evaluated': False},
                  'input_hashes_verified_after_probe': True, 'model': str(model),
                  'model_moc_sha256': comparison.digest(moc),
                  'model_inputs': [{'path': str(path), 'sha256': comparison.digest(path), 'roles': [role]}
                                   for path, role in ((model, 'model3'), (moc, 'Moc'), (atlas, 'Textures[0]'))],
                  'cubism_core': {'path': str(self.core), 'sha256': comparison.digest(self.core)},
                  'pose_input': {'path': str(self.poses), 'sha256': comparison.digest(self.poses)},
                  'probe_tool_sha256': comparison.digest(Path(comparison.__file__).with_name('probe_cubism_core.py')),
                  'canvas': {'size': [100, 100], 'origin': [50, 50], 'pixels_per_unit': 100},
                  'array_hash_encoding': {'vertices_uv': 'float32', 'indices': 'uint16', 'byte_order': 'little'},
                  'parameter_count': 1, 'parameters': {'ParamMouth': {'min': 0, 'max': 1, 'default': 0, 'type': 0, 'default_in_range': True}},
                  'parts': {'PartBody': {'default_opacity': 1, 'parent_index': -1}},
                  'drawable_count': 1, 'selected_drawables': ['Mouth'], 'topology': {'Mouth': topology},
                  'poses': [{'name': 'default', 'parameters': {'ParamMouth': {'raw': 0, 'clamped': 0, 'native_input': 0, 'native_value': 0,
                                                                         'specified': False, 'was_clamped': False}},
                             'part_opacities': {'PartBody': 1}, 'drawables': {'Mouth': drawable}}]}
        if not selected:
            report['topology']['Guard'] = deepcopy(topology)
            report['topology']['Guard'].update(index=1, uvs=uvs, indices=indices)
            report['poses'][0]['drawables']['Guard'] = {**deepcopy(drawable), 'vertices': [[0, 0], [0.1, 0], [0, 0.1]]}
            report['poses'][0]['drawables']['Guard']['vertices_sha256'] = comparison.buffer_hash(report['poses'][0]['drawables']['Guard']['vertices'], 'f', 'little')
            report['drawable_count'] = 2
            report['selected_drawables'] = ['Guard']
        path = directory / 'core.json'
        path.write_text(json.dumps(report), encoding='utf8')
        return path, report

    def save_report(self, path, report):
        path.write_text(json.dumps(report), encoding='utf8')

    def test_changed_moc_and_png_encoding_do_not_fail_equivalent_native_data(self):
        before, _ = self.exported_report('before', moc_bytes=b'old serialization', compress=0)
        after, _ = self.exported_report('after', moc_bytes=b'new serialization', compress=9)
        result = comparison.compare(before, after, self.root / 'comparison.json')
        self.assertEqual(result['status'], 'equivalent_within_tolerance')
        self.assertEqual(result['summary']['difference_count'], 0)
        self.assertIn('Moc', {row['role'] for row in result['file_byte_changes_are_not_semantic_failure']})
        self.assertNotEqual(result['textures']['before'][0]['file_sha256'], result['textures']['after'][0]['file_sha256'])
        self.assertEqual(result['textures']['before'][0]['rgba_sha256'], result['textures']['after'][0]['rgba_sha256'])

    def test_actual_opacity_geometry_range_and_topology_changes_are_reported(self):
        before, _ = self.exported_report('before')
        after, report = self.exported_report('after', vertices=[[0.02, 0], [0.1, 0], [0, 0.1]], opacity=0)
        report['parameters']['ParamMouth']['max'] = 2
        report['topology']['Mouth']['indices'] = [0, 2, 1]
        report['topology']['Mouth']['indices_sha256'] = comparison.buffer_hash([0, 2, 1], 'H', 'little')
        self.save_report(after, report)
        result = comparison.compare(before, after, self.root / 'comparison.json')
        self.assertEqual(result['status'], 'changed')
        categories = {row['category'] for row in result['differences']}
        self.assertTrue({'parameters', 'topology', 'pose_drawables', 'pose_geometry'} <= categories)
        self.assertAlmostEqual(result['summary']['max_geometry_error_px'], 2)

    def test_changed_unselected_geometry_is_incomplete_not_equivalent(self):
        before, _ = self.exported_report('before', selected=False)
        after, _ = self.exported_report('after', selected=False, vertices=[[0.02, 0], [0.1, 0], [0, 0.1]])
        result = comparison.compare(before, after, self.root / 'comparison.json')
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(result['summary']['difference_count'], 0)
        self.assertEqual(result['incomplete'][0]['item'], 'default.Mouth')

    def test_complete_raw_arrays_with_different_topology_counts_report_the_actual_gap(self):
        before, _ = self.exported_report('before')
        after, report = self.exported_report('after', vertices=[[0, 0], [0.1, 0], [0, 0.1], [0.1, 0.1]])
        report['topology']['Mouth'].update(vertex_count=4, uvs=[[0, 0], [1, 0], [0, 1], [1, 1]])
        report['topology']['Mouth']['uvs_sha256'] = comparison.buffer_hash(report['topology']['Mouth']['uvs'], 'f', 'little')
        self.save_report(after, report)
        result = comparison.compare(before, after, self.root / 'comparison.json')
        self.assertEqual(result['status'], 'changed')
        gap = result['incomplete'][0]
        self.assertEqual(gap['reason_code'], 'topology_vertex_count_mismatch')
        self.assertEqual((gap['before_vertex_count'], gap['after_vertex_count']), (3, 4))
        self.assertNotIn('select this drawable', gap['reason'])
        evidence = result['geometry_and_opacity'][0]
        self.assertEqual(evidence['geometry_evidence'], 'topology_vertex_count_mismatch')
        self.assertIsNone(evidence['max_error_px'])

    def test_source_pixel_tolerance_handles_small_float_changes(self):
        before, _ = self.exported_report('before')
        after, _ = self.exported_report('after', vertices=[[0.00001, 0], [0.1, 0], [0, 0.1]])
        result = comparison.compare(before, after, self.root / 'comparison.json')
        self.assertEqual(result['status'], 'equivalent_within_tolerance')
        self.assertAlmostEqual(result['summary']['max_geometry_error_px'], 0.001)

    def test_stale_export_rejects_comparison_without_overwriting_previous_evidence(self):
        before, _ = self.exported_report('before')
        after, report = self.exported_report('after')
        output = self.root / 'comparison.json'
        output.write_bytes(b'previous valid evidence')
        moc = Path(next(row['path'] for row in report['model_inputs'] if 'Moc' in row['roles']))
        moc.write_bytes(b'changed after probe')
        with self.assertRaisesRegex(comparison.ComparisonError, 'Current input missing or changed'):
            comparison.compare(before, after, output)
        self.assertEqual(output.read_bytes(), b'previous valid evidence')
        self.assertEqual(moc.read_bytes(), b'changed after probe')

    def test_output_cannot_replace_a_model_input(self):
        before, _ = self.exported_report('before')
        after, report = self.exported_report('after')
        target = Path(report['model'])
        original = target.read_bytes()
        with self.assertRaisesRegex(comparison.ComparisonError, 'must not replace any input'):
            comparison.compare(before, after, target)
        self.assertEqual(target.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
