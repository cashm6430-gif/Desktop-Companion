"""Joint diagnostics must distinguish a bent centerline from a rotating rod."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import analyze_authoring_probe as analysis


class AuthoringProbeAnalysisTest(unittest.TestCase):
    def test_a_rotated_and_translated_rigid_rod_is_not_flexible_bending(self):
        measured = analysis.centerline_metrics([[2, 3], [2, 8], [2, 13]])
        self.assertFalse(measured['measurable_bending'])
        self.assertEqual(measured['polyline_arc_length_px'], 10)
        self.assertEqual(measured['root_tip_chord_length_px'], 10)
        self.assertEqual(measured['max_distance_from_chord_px'], 0)

    def test_curved_centerline_separates_arc_length_from_landmark_chords(self):
        measured = analysis.centerline_metrics([[0, 0], [3, 4], [6, 4], [9, 0]])
        self.assertTrue(measured['measurable_bending'])
        self.assertEqual(measured['polyline_arc_length_px'], 13)
        self.assertEqual(measured['root_tip_chord_length_px'], 9)
        self.assertEqual(measured['max_distance_from_chord_px'], 4)
        neutral = [[0, 0], [10, 0], [10, 10], [0, 10]]
        posed = [[0, 0], [10, 0], [10, 20], [0, 10]]
        indices = [0, 1, 2, 0, 2, 3]
        split = analysis.mesh_split_centerline([[0, 4], [5, 4], [10, 4]], neutral, indices)
        self.assertIn([4, 4], split)
        material_points = [analysis.sample(posed, analysis.locate(neutral, indices, point)) for point in split]
        self.assertAlmostEqual(analysis.centerline_metrics(material_points)['polyline_arc_length_px'], 32 ** 0.5 + 6)

    def test_triangle_reflection_and_collapse_are_detected_independently(self):
        neutral = [[0, 0], [10, 0], [0, 10]]
        reflected = analysis.triangle_health(neutral, [[0, 0], [-10, 0], [0, 10]], [0, 1, 2])
        self.assertEqual(reflected['flipped'], [0])
        self.assertEqual(reflected['collapsed'], [])
        collapsed = analysis.triangle_health(neutral, [[0, 0], [10, 0], [20, 0]], [0, 1, 2])
        self.assertEqual(collapsed['flipped'], [])
        self.assertEqual(collapsed['collapsed'], [0])

    def test_changed_export_rejects_analysis_without_replacing_evidence(self):
        with tempfile.TemporaryDirectory(prefix='authoring-analysis-') as directory:
            root = Path(directory)
            model = root / 'fixture.moc3'
            model.write_bytes(b'original exported model')
            core = root / 'core.dll'
            core.write_bytes(b'fixture core')
            poses = root / 'poses.json'
            poses.write_text('{"poses":[{"name":"neutral"}]}', encoding='utf8')
            fixture = root / 'fixture.json'
            fixture.write_text('{"canvas":[640,512],"layers":[]}', encoding='utf8')
            report = root / 'core.json'
            report.write_text(json.dumps({
                'scope': {'raw_export_only': True}, 'input_hashes_verified_after_probe': True,
                'pose_input': {'sha256': analysis.digest(poses)}, 'canvas': {'size': [640, 512]},
                'model_inputs': [{'path': str(model), 'sha256': analysis.digest(model)}],
                'cubism_core': {'path': str(core), 'sha256': analysis.digest(core)}}), encoding='utf8')
            output = root / 'analysis.json'
            output.write_bytes(b'previous valid evidence')
            model.write_bytes(b'export overwritten after Core probe')
            with self.assertRaisesRegex(ValueError, 'Model/Core input missing or changed'):
                analysis.analyze(report, fixture, poses, output, 'skeleton')
            self.assertEqual(output.read_bytes(), b'previous valid evidence')
            self.assertEqual(model.read_bytes(), b'export overwritten after Core probe')


if __name__ == '__main__':
    unittest.main()
