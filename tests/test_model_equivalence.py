"""Synthetic equivalence-gate tests; no Cubism Core or GUI capture is run.

Temporary PNGs and receipts exercise decoded RGBA, provenance, trace and
inventory rejection. They do not attest any real model's Native equivalence.
"""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import review_model_equivalence as gate


class ModelEquivalenceSyntheticGateTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='pet-equivalence-synthetic-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.reference = self.root / 'reference'
        self.candidate = self.root / 'candidate'
        self.output = self.root / 'comparison.json'
        self.renderer = self.root / 'synthetic-renderer.exe'
        self.renderer.write_bytes(b'Synthetic renderer fixture; not an executable')
        self.fixture = self.root / 'poses.json'
        self.fixture.write_text(json.dumps({'keyframes': [{'parameters': {'ParamSitPose': 0}}]}))
        self.model = self.root / 'synthetic-model.moc3'
        self.model.write_bytes(b'Synthetic model fixture; not a MOC')
        self.shared = gate.file_snapshot([self.renderer, self.fixture])
        self.inputs = gate.file_snapshot([self.model])
        self.synthetic_plan = [
            {'kind': 'static', 'name': 'fixture', 'folder': 'static', 'frames': ['pose-00.png']},
            {'kind': 'motion', 'name': 'idle', 'folder': 'motions/idle', 'frames': ['frame-000.png']},
        ]
        # The test plan is intentionally tiny. Never treat these two fixture
        # sequences as the real full capture's motions/interaction inventory.
        plan_patch = patch.object(gate, 'capture_plan', return_value=self.synthetic_plan, create=True)
        plan_patch.start()
        self.addCleanup(plan_patch.stop)
        self.make_capture(self.reference)
        self.make_capture(self.candidate)

    def make_capture(self, folder):
        sequences = []
        for kind, name, subfolder, filename in (
            ('static', 'fixture', 'static', 'pose-00.png'),
            ('motion', 'idle', 'motions/idle', 'frame-000.png'),
        ):
            target = folder / subfolder / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            image = Image.new('RGBA', (8, 8), (0, 0, 0, 0))
            image.putpixel((1, 1), (40, 70, 120, 255))
            image.save(target)
            sequences.append({
                'kind': kind, 'name': name, 'folder': subfolder,
                'frames': [filename],
                'trace': [{'index': 0, 'parameters': {'ParamSitPose': 0}, 'frame_saved': True}],
                'captured_files': gate.file_snapshot([target]),
            })
        receipt = {
            'version': 1, 'model': str(self.model), 'fixture': str(self.fixture),
            'scope': 'full', 'raw_export_only': False, 'status': 'complete',
            'approval': 'pending', 'adoption': 'not_requested',
            'model_inputs': self.inputs, 'shared_inputs': self.shared,
            'plan': self.synthetic_plan, 'sequences': sequences, 'total_frames': 2,
        }
        self.save_receipt(folder, receipt)

    def receipt(self, folder):
        return gate.read(folder / 'capture-receipt.json')

    @staticmethod
    def save_receipt(folder, value):
        gate.write(folder / 'capture-receipt.json', value)

    def compare(self):
        with redirect_stdout(io.StringIO()):
            return gate.compare(self.reference, self.candidate, self.output)

    def change_candidate_pixel(self, point, rgba):
        path = self.candidate / 'static/pose-00.png'
        with Image.open(path) as source:
            image = source.convert('RGBA')
        image.putpixel(point, rgba)
        image.save(path)
        # This is a legitimately different candidate, not an after-capture
        # tamper. Its own input/PNG hashes remain internally consistent.
        receipt = self.receipt(self.candidate)
        receipt['sequences'][0]['captured_files'] = gate.file_snapshot([path])
        self.save_receipt(self.candidate, receipt)

    def test_identical_decoded_rgba_passes_without_granting_approval(self):
        self.assertTrue(self.compare())
        report = gate.read(self.output)
        self.assertEqual(report['frames_compared'], 2)
        self.assertEqual(report['changed_pixels'], 0)
        self.assertEqual(report['approval'], 'pending')
        self.assertEqual(report['adoption'], 'not_requested')

    def test_different_png_compression_with_identical_rgba_passes(self):
        path = self.candidate / 'static/pose-00.png'
        with Image.open(path) as source:
            image = source.convert('RGBA')
        image.save(path, compress_level=0)
        receipt = self.receipt(self.candidate)
        receipt['sequences'][0]['captured_files'] = gate.file_snapshot([path])
        self.save_receipt(self.candidate, receipt)
        self.assertTrue(self.compare())

    def test_transparent_pixel_rgb_change_fails(self):
        self.change_candidate_pixel((0, 0), (1, 0, 0, 0))
        self.assertFalse(self.compare())
        report = gate.read(self.output)
        self.assertEqual(report['changed_pixels'], 1)
        self.assertEqual(report['frame_differences'][0]['max_channel_delta'], 1)

    def test_one_pixel_one_channel_change_fails(self):
        self.change_candidate_pixel((1, 1), (40, 70, 121, 255))
        self.assertFalse(self.compare())
        report = gate.read(self.output)
        self.assertEqual(report['changed_pixels'], 1)
        self.assertEqual(report['frame_differences'][0]['bbox'], [1, 1, 2, 2])

    def test_parameter_trace_change_fails_even_with_equal_pixels(self):
        receipt = self.receipt(self.candidate)
        receipt['sequences'][0]['trace'][0]['parameters']['ParamSitPose'] = 0.65
        self.save_receipt(self.candidate, receipt)
        self.assertFalse(self.compare())
        self.assertEqual(gate.read(self.output)['trace_differences'], ['static/fixture'])

    def test_different_renderer_inputs_fail(self):
        other = self.root / 'different-renderer.exe'
        other.write_bytes(b'Another synthetic renderer')
        receipt = self.receipt(self.candidate)
        receipt['shared_inputs'] = gate.file_snapshot([other, self.fixture])
        self.save_receipt(self.candidate, receipt)
        self.assertFalse(self.compare())
        self.assertFalse(gate.read(self.output)['shared_inputs_equal'])

    def test_changed_shared_input_after_capture_is_rejected(self):
        self.renderer.write_bytes(b'Changed renderer after both captures')
        with self.assertRaisesRegex(ValueError, 'stale'):
            self.compare()
        self.assertFalse(self.output.exists())

    def test_captured_png_tamper_is_rejected(self):
        path = self.candidate / 'static/pose-00.png'
        with Image.open(path) as source:
            image = source.convert('RGBA')
        image.putpixel((1, 1), (40, 70, 121, 255))
        image.save(path)
        with self.assertRaisesRegex(ValueError, 'stale'):
            self.compare()
        self.assertFalse(self.output.exists())

    def test_missing_png_is_rejected(self):
        (self.candidate / 'static/pose-00.png').unlink()
        with self.assertRaisesRegex(ValueError, 'stale'):
            self.compare()

    def test_candidate_missing_sequence_cannot_pass(self):
        receipt = self.receipt(self.candidate)
        receipt['sequences'].pop()
        self.save_receipt(self.candidate, receipt)
        with self.assertRaises(ValueError):
            self.compare()

    def test_candidate_missing_frame_inventory_cannot_pass(self):
        receipt = self.receipt(self.candidate)
        receipt['sequences'][0]['frames'] = []
        self.save_receipt(self.candidate, receipt)
        with self.assertRaises(ValueError):
            self.compare()

    def test_both_missing_same_sequence_cannot_pass(self):
        for folder in (self.reference, self.candidate):
            receipt = self.receipt(folder)
            receipt['sequences'].pop()
            receipt['total_frames'] = 1
            self.save_receipt(folder, receipt)
        with self.assertRaises(ValueError):
            self.compare()

    def test_wrong_total_frame_count_is_rejected(self):
        for folder in (self.reference, self.candidate):
            receipt = self.receipt(folder)
            receipt['total_frames'] = 3
            self.save_receipt(folder, receipt)
        with self.assertRaises(ValueError):
            self.compare()

    def test_incomplete_capture_is_rejected(self):
        receipt = self.receipt(self.candidate)
        receipt['status'] = 'running'
        self.save_receipt(self.candidate, receipt)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            self.compare()


if __name__ == '__main__':
    unittest.main()
