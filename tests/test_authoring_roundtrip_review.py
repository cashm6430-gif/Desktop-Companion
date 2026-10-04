"""Orchestration/isolation tests only; Core and Editor exports are mocked."""
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import review_authoring_roundtrip as review


class AuthoringRoundtripReviewTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name) / "run"
        self.exports = self.run / "exports/editor-cmo3-roundtrip"
        self.exports.mkdir(parents=True)
        (self.run / "reports").mkdir()
        self.baseline_path = self.run / "reports/runtime-before.core.json"
        self.baseline_path.write_bytes(b"mock baseline receipt; not real Core evidence")
        self.poses = self.run / "poses.json"
        self.poses.write_text(json.dumps({"poses": [{"name": "check", "parameters": {"ParamHead": 1},
                           "part_opacities": {"PartBody": 1}}], "drawables": ["OldDrawable"]}), encoding="utf8")
        self.core = self.run / "mock-core.dll"
        self.core.write_bytes(b"mock binary; never loaded")
        self.baseline = {"pose_input": {"path": str(self.poses), "sha256": review.probe.digest(self.poses)},
                         "cubism_core": {"path": str(self.core), "sha256": review.probe.digest(self.core)}}
        self.baseline_hashes = {str(path): review.probe.digest(path)
                                for path in (self.baseline_path, self.poses, self.core)}
        self.native = SimpleNamespace(parameter_ids=["ParamHead"], part_ids=["PartBody"],
                                      drawable_ids=["CurrentDrawable", "NewDrawable"],
                                      vertex_counts=[3, 4], index_counts=[3, 6], moc_version=5)

    def export(self, name="test", moc_reference=None):
        model = self.exports / (name + ".model3.json")
        moc = self.exports / (name + ".moc3")
        moc.write_bytes(b"mock MOC; never loaded by real Core")
        model.write_text(json.dumps({"FileReferences": {"Moc": moc_reference or moc.name}}), encoding="utf8")
        return model

    def original_bytes(self):
        return {path: path.read_bytes() for path in self.run.rglob("*") if path.is_file()}

    def assert_original_bytes(self, before):
        for path, raw in before.items():
            self.assertEqual(path.read_bytes(), raw, str(path))

    def baseline_patch(self):
        return patch.object(review.comparison, "read_probe", return_value=(self.baseline, self.baseline_hashes, {}))

    def test_empty_export_only_waits_and_creates_no_reports(self):
        before = self.original_bytes()
        with self.baseline_patch() as baseline, patch.object(review.probe, "NativeModel") as native, \
                patch.object(review.probe, "run_probe") as probe:
            code, result = review.review(self.run)
        self.assertEqual(code, 3)
        self.assertEqual(result["status"], "waiting_input")
        baseline.assert_not_called()
        native.assert_not_called()
        probe.assert_not_called()
        self.assertEqual(self.original_bytes(), before)

    def test_multiple_models_require_explicit_selection_without_core(self):
        self.export("older")
        self.export("newer")
        before = self.original_bytes()
        with patch.object(review.probe, "NativeModel") as native:
            code, result = review.review(self.run)
        self.assertEqual(code, 2)
        self.assertEqual(result["status"], "selection_required")
        self.assertEqual(len(result["candidates"]), 2)
        native.assert_not_called()
        self.assertFalse(list(Path(result["report_directory"]).glob("*.core.json")))
        self.assert_original_bytes(before)

    def test_rejects_reference_escape_and_missing_requested_axes_or_parts(self):
        external = self.run / "not-exported.moc3"
        external.write_bytes(b"preserved external source")
        model = self.export(moc_reference="../../not-exported.moc3")
        with self.baseline_patch(), patch.object(review.probe, "NativeModel") as native:
            code, result = review.review(self.run)
        self.assertEqual(code, 1)
        self.assertEqual(result["status"], "failed")
        self.assertIn("outside", result["reason"])
        native.assert_not_called()
        model.write_text(json.dumps({"FileReferences": {"Moc": "test.moc3"}}), encoding="utf8")
        for field in ("parameter_ids", "part_ids"):
            with self.subTest(field=field):
                native_stub = SimpleNamespace(**vars(self.native))
                setattr(native_stub, field, [])
                before = self.original_bytes()
                with self.baseline_patch(), patch.object(review.probe, "NativeModel", return_value=native_stub), \
                        patch.object(review.probe, "run_probe") as probe:
                    code, result = review.review(self.run)
                self.assertEqual(code, 1)
                self.assertEqual(result["status"], "failed")
                self.assertTrue(result["preflight"]["missing_requested_bindings"])
                probe.assert_not_called()
                self.assertFalse((Path(result["report_directory"]) / "editor-after.core.json").exists())
                self.assert_original_bytes(before)

    def test_budget_overflow_is_incomplete_without_dropping_ids_or_poses(self):
        self.export()
        self.native.vertex_counts = [review.probe.MAX_VERTEX_SAMPLES, 1]
        before = self.original_bytes()
        with self.baseline_patch(), patch.object(review.probe, "NativeModel", return_value=self.native), \
                patch.object(review.probe, "run_probe") as probe, patch.object(review.comparison, "compare") as compare:
            code, result = review.review(self.run)
        self.assertEqual(code, 1)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["preflight"]["selected_after_drawables"], self.native.drawable_ids)
        probe.assert_not_called()
        compare.assert_not_called()
        self.assert_original_bytes(before)

    def test_success_uses_original_inputs_and_all_after_ids_in_fresh_directories(self):
        model = self.export()
        before = self.original_bytes()

        def mocked_probe(model_path, poses_path, output_path, core_path=None, drawables=None):
            self.assertEqual(model_path, model.resolve())
            self.assertEqual(poses_path, self.poses.resolve())
            self.assertEqual(core_path, self.core.resolve())
            self.assertEqual(drawables, ["CurrentDrawable", "NewDrawable"])
            Path(output_path).write_text('{"mock_test_only":true}', encoding="utf8")

        def mocked_compare(baseline, after, output):
            self.assertEqual(baseline, self.baseline_path.resolve())
            Path(output).write_text('{"mock_test_only":true}', encoding="utf8")
            return {"status": "equivalent_within_tolerance", "summary": {"difference_count": 0}}

        with self.baseline_patch(), patch.object(review.probe, "NativeModel", return_value=self.native), \
                patch.object(review.probe, "run_probe", side_effect=mocked_probe) as probe, \
                patch.object(review.comparison, "compare", side_effect=mocked_compare):
            code, first = review.review(self.run, model)
            first_receipt = Path(first["review_report"]).read_bytes()
            second_code, second = review.review(self.run, model)
        self.assertEqual((code, second_code), (0, 0))
        self.assertEqual(probe.call_count, 2)
        self.assertNotEqual(first["report_directory"], second["report_directory"])
        self.assertEqual(Path(first["review_report"]).read_bytes(), first_receipt)
        self.assertTrue(first["input_hashes_verified_before_and_after"])
        self.assert_original_bytes(before)


if __name__ == "__main__":
    unittest.main()
