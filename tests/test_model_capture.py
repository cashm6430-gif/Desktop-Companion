"""Synthetic saved-capture binding tests; no Native/Core or GUI is run.

Fake source files and tiny generated PNGs exercise receipt/hash/path gates.
Passing these tests does not prove any real model's equivalence or appearance.
"""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_model_capture as gate
from review_neck import file_snapshot


class ModelCaptureSyntheticBindingTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='pet-capture-synthetic-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.run = self.root / 'capture'
        self.inputs = self.root / 'inputs'
        self.inputs.mkdir()
        self.model = self.inputs / 'synthetic.model3.json'
        self.model.write_text('{}', encoding='utf8')
        self.fixture = self.inputs / 'poses.json'
        self.fixture.write_text(json.dumps({'keyframes': [{'parameters': {'ParamSitPose': 0}}]}),
                                encoding='utf8')
        self.shared = [self.fixture]
        for name in ('DesktopCompanion.exe', 'Live2DCubismCore.dll', 'review_model_equivalence.py'):
            path = self.inputs / name
            path.write_bytes(b'Synthetic source fixture; never executed. ' + name.encode())
            self.shared.append(path)
        self.plan = [
            {'kind': 'static', 'name': 'fixture', 'folder': 'static', 'frames': ['pose-00.png']},
            {'kind': 'interaction', 'name': 'synthetic-input', 'folder': 'interactions/synthetic-input',
             'frames': ['frame-000.png', 'frame-001.png']},
        ]
        # The real capture plan contains all motions/interactions. A tiny test
        # plan isolates binding logic and must never stand in for that inventory.
        plan_patch = patch.object(gate, 'capture_plan', return_value=deepcopy(self.plan))
        self.plan_mock = plan_patch.start()
        self.addCleanup(plan_patch.stop)
        self.make_capture(self.run)

    @staticmethod
    def image(path, empty=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        image = Image.new('RGBA', (8, 8), (0, 0, 0, 0))
        if not empty:
            image.putpixel((1, 1), (40, 70, 120, 255))
        image.save(path)

    def make_capture(self, run):
        sequences = deepcopy(self.plan)
        for sequence in sequences:
            paths = [run / sequence['folder'] / filename for filename in sequence['frames']]
            for path in paths:
                self.image(path)
            sequence['trace'] = [dict(index=index, frame_saved=True)
                                 for index in range(len(paths))]
            sequence['captured_files'] = file_snapshot(paths)
        self.save_receipt({
            'version': 1, 'model': str(self.model), 'fixture': str(self.fixture),
            'scope': 'full', 'raw_export_only': False, 'status': 'complete',
            'approval': 'pending', 'adoption': 'not_requested',
            'model_inputs': file_snapshot([self.model]), 'shared_inputs': file_snapshot(self.shared),
            'plan': deepcopy(self.plan), 'sequences': sequences, 'total_frames': 3,
        }, run)

    def receipt(self, run=None):
        return json.loads(((run or self.run) / 'capture-receipt.json').read_text(encoding='utf8'))

    def save_receipt(self, receipt, run=None):
        ((run or self.run) / 'capture-receipt.json').write_text(
            json.dumps(receipt, indent=2) + '\n', encoding='utf8')

    def assert_rejected(self, receipt, pattern=None):
        self.save_receipt(receipt)
        context = (self.assertRaisesRegex(ValueError, pattern) if pattern
                   else self.assertRaises(ValueError))
        with context:
            gate.verify_capture_binding(self.run)

    def test_valid_capture_pins_saved_version_and_hash_without_approval(self):
        before = (self.run / 'capture-receipt.json').read_bytes()
        report = gate.verify_capture_binding(self.run)
        self.assertEqual(report['status'], 'passed')
        self.assertEqual(report['receipt_version'], 1)
        self.assertEqual(report['receipt_sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(report['verifier_tool_sha256'], gate.digest(gate.__file__))
        self.assertEqual(report['total_frames'], 3)
        self.assertEqual(report['sequence_count'], 2)
        self.assertEqual(report['planned_motion_count'], 0)
        self.assertEqual(report['planned_interaction_count'], 1)
        self.assertEqual(report['sequences'][1]['frames'], 2)
        self.assertFalse(report['native_rendering_rerun'])
        self.assertFalse(report['model_equivalence_tested'])
        self.assertEqual(report['visual_approval'], 'pending')
        self.assertEqual(report['adoption'], 'not_requested')
        self.assertEqual(before, (self.run / 'capture-receipt.json').read_bytes())
        self.plan_mock.assert_called_once_with(self.fixture.resolve(), False, False)

    def test_cleared_png_hashes_are_rejected(self):
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'] = []
        self.assert_rejected(receipt, 'nonempty')

    def test_duplicate_png_hash_path_is_rejected(self):
        receipt = self.receipt()
        receipt['sequences'][1]['captured_files'].append(
            deepcopy(receipt['sequences'][1]['captured_files'][0]))
        self.assert_rejected(receipt, 'duplicate')

    def test_valid_foreign_png_hash_cannot_replace_a_run_frame(self):
        outside = self.root / 'foreign/pose-00.png'
        self.image(outside)
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'] = file_snapshot([outside])
        self.assert_rejected(receipt, 'one-to-one')

    def test_valid_other_sequence_hash_cannot_replace_a_frame(self):
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'] = [
            deepcopy(receipt['sequences'][1]['captured_files'][0])]
        self.assert_rejected(receipt, 'one-to-one')

    def test_relative_png_hash_path_is_rejected(self):
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'][0]['path'] = 'static/pose-00.png'
        self.assert_rejected(receipt, 'absolute')

    def test_both_runs_losing_same_sequence_are_individually_rejected(self):
        second = self.root / 'candidate'
        self.make_capture(second)
        for run in (self.run, second):
            receipt = self.receipt(run)
            receipt['sequences'].pop()
            receipt['plan'].pop()
            receipt['total_frames'] = 1
            self.save_receipt(receipt, run)
            with self.assertRaisesRegex(ValueError, 'complete recomputed plan'):
                gate.verify_capture_binding(run)

    def test_wrong_total_count_is_rejected(self):
        receipt = self.receipt()
        receipt['total_frames'] = 2
        self.assert_rejected(receipt, 'total_frames')

    def test_short_interaction_trace_is_rejected(self):
        receipt = self.receipt()
        receipt['sequences'][1]['trace'].pop()
        self.assert_rejected(receipt, 'Trace count')

    def test_extra_interaction_frame_is_rejected(self):
        self.image(self.run / 'interactions/synthetic-input/frame-002.png')
        with self.assertRaisesRegex(ValueError, 'extra planned capture'):
            gate.verify_capture_binding(self.run)

    def test_separate_diagnostic_png_does_not_expand_frame_inventory(self):
        self.image(self.run / 'interactions/synthetic-input/tip-region-diagnostic.png')
        self.assertEqual(gate.verify_capture_binding(self.run)['total_frames'], 3)

    def test_png_hash_tampering_is_rejected(self):
        path = self.run / 'static/pose-00.png'
        with Image.open(path) as picture:
            image = picture.convert('RGBA')
        image.putpixel((1, 1), (40, 70, 121, 255))
        image.save(path)
        with self.assertRaises(ValueError):
            gate.verify_capture_binding(self.run)

    def test_source_hash_tampering_is_rejected(self):
        self.model.write_text('{"changed": true}', encoding='utf8')
        with self.assertRaises(ValueError):
            gate.verify_capture_binding(self.run)

    def test_unpinned_declared_model_is_rejected(self):
        outside = self.inputs / 'unrecorded.model3.json'
        outside.write_text('{}', encoding='utf8')
        receipt = self.receipt()
        receipt['model'] = str(outside)
        self.assert_rejected(receipt, 'not pinned')

    def test_missing_pinned_core_is_rejected(self):
        receipt = self.receipt()
        receipt['shared_inputs'] = [row for row in receipt['shared_inputs']
                                    if Path(row['path']).name != 'Live2DCubismCore.dll']
        self.assert_rejected(receipt, 'Cubism Core')

    def test_unreadable_image_with_matching_hash_is_rejected(self):
        path = self.run / 'static/pose-00.png'
        path.write_bytes(b'This is not an image')
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'] = file_snapshot([path])
        self.save_receipt(receipt)
        with self.assertRaises(OSError):
            gate.verify_capture_binding(self.run)

    def test_empty_alpha_png_with_matching_hash_is_rejected(self):
        path = self.run / 'static/pose-00.png'
        self.image(path, empty=True)
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'] = file_snapshot([path])
        self.assert_rejected(receipt, 'visually empty')

    def test_non_png_image_with_matching_hash_is_rejected(self):
        path = self.run / 'static/pose-00.png'
        Image.new('RGB', (8, 8), (40, 70, 120)).save(path, format='JPEG')
        receipt = self.receipt()
        receipt['sequences'][0]['captured_files'] = file_snapshot([path])
        self.assert_rejected(receipt, 'positive-size PNG')

    def test_boolean_receipt_version_is_rejected(self):
        receipt = self.receipt()
        receipt['version'] = True
        self.assert_rejected(receipt, 'version')

    def test_incomplete_receipt_is_rejected(self):
        receipt = self.receipt()
        receipt['status'] = 'running'
        self.assert_rejected(receipt, 'complete saved capture')

    def test_changed_saved_receipt_during_verification_is_rejected(self):
        original = gate.verify_snapshot
        calls = 0

        def mutate_during_snapshot(records):
            nonlocal calls
            original(records)
            calls += 1
            if calls == 5:
                receipt = self.receipt()
                receipt['unrelated_note'] = 'receipt edited during verification'
                self.save_receipt(receipt)

        with patch.object(gate, 'verify_snapshot', side_effect=mutate_during_snapshot):
            with self.assertRaisesRegex(ValueError, 'receipt changed'):
                gate.verify_capture_binding(self.run)

    def test_reparse_ancestor_is_rejected_before_resolve(self):
        original = Path.lstat
        root = self.run.absolute()

        def attributes(path, *args, **kwargs):
            if path == root:
                return SimpleNamespace(st_mode=stat.S_IFDIR,
                                       st_file_attributes=getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))
            return original(path, *args, **kwargs)

        with patch.object(Path, 'lstat', attributes):
            with self.assertRaisesRegex(ValueError, 'junction or reparse'):
                gate.verify_capture_binding(self.run)

    def test_symlink_frame_is_rejected_before_resolve(self):
        original = Path.lstat
        frame = self.run / 'static/pose-00.png'

        def attributes(path, *args, **kwargs):
            if path == frame:
                return SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
            return original(path, *args, **kwargs)

        with patch.object(Path, 'lstat', attributes):
            with self.assertRaisesRegex(ValueError, 'Symlink'):
                gate.verify_capture_binding(self.run)


if __name__ == '__main__':
    unittest.main()
