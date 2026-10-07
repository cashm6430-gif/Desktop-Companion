"""Capabilities must distinguish names/ranges/policy from model-bound probes."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import inspect_live2d_capabilities as capabilities
import review_session


class Live2DCapabilitiesTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="companion-capabilities-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.model = self.root / capabilities.MODEL_REL
        self.ranges_path = self.root / "build/verification/ranges.json"
        self.policy_path = self.root / "art/live2d/workflow/parameter-policy.json"
        self.write(self.model / "fixture.moc3", b"model revision one")
        self.write(self.model / "texture.png", b"texture revision one")
        self.write_json(self.model / capabilities.MODEL_NAME,
                        {"FileReferences": {"Moc": "fixture.moc3", "DisplayInfo": "fixture.cdi3.json"}})
        self.write_json(self.model / "fixture.cdi3.json",
                        {"Parameters": [{"Id": "ParamKnown", "Name": "Known head angle"}],
                         "CombinedParameters": [["ParamKnown", "ParamMystery"]]})
        self.write_json(self.model / "whale-girl-layered-draft.psd2live.json",
                        {"layers": [{"source": "face", "parameter": "ParamKnown", "drawable": "Face"}]})
        self.write_json(self.root / "assets/motions/approved.motion.json",
                        {"approval": "approved", "revision": 3,
                         "tracks": {"ParamKnown": [[0, 0], [1, 1]], "ParamMystery": [[0, 0], [1, 1]]}})
        self.write(self.root / "tools/validate_live2d_assets.py",
                   "INERT_PARAMETERS={'ParamBrowLY'}\nGROUNDED_PARAMETERS={'ParamShiftY'}\n")
        self.write(self.root / "build/DesktopCompanion.exe", b"fixture renderer")
        self.write(self.root / "build/Live2DCubismCore.dll", b"fixture core")
        for source in list(self.model.rglob("*")) + list((self.root / "assets/motions").glob("*")):
            if source.is_file():
                self.write(self.root / "build" / source.relative_to(self.root), source.read_bytes())
        self.policy = {"parameters": {"ParamKnown": {"purpose": "head angle", "recommended_channel": "head"}},
                       "channels": {"head": {"current_writers": ["fixture motion player"]}}, "evidence": {}}
        self.write_json(self.policy_path, self.policy)
        self.native = {"render_backend": "cubism_native", "parameter_count": 4,
                       "parameters": {name: {"min": -1, "max": 1, "default": 0}
                                      for name in ("ParamKnown", "ParamMystery", "ParamBrowLY", "ParamShiftY")}}
        self.write_bound_ranges()
        for name, value in (("ROOT", self.root), ("BUILD", self.root / "build"), ("MODEL", self.model)):
            patcher = patch.object(review_session, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value.encode("utf8") if isinstance(value, str) else value)

    def write_json(self, path, value):
        self.write(path, json.dumps(value))

    def write_bound_ranges(self):
        self.write_json(self.ranges_path, {
            **self.native, "model_inputs": capabilities.model_snapshot(self.root),
            "model_moc_sha256": capabilities.sha256(self.model / "fixture.moc3"),
            "capture_provenance": {"kind": "native_parameter_ranges"}})

    def inspect(self):
        return capabilities.inspect_capabilities(self.root, self.ranges_path, self.policy_path)

    def add_probe(self):
        proof = self.root / "art/live2d/workflow/axis-probe.json"
        self.write_json(proof, {"parameter": "ParamKnown", "vertex_displacement": 0.1})
        self.policy["evidence"]["probe"] = {
            "kind": "native_parameter_probe", "scope": "one axis at neutral; vertex displacement only",
            "model_inputs": capabilities.model_snapshot(self.root),
            "renderer_sha256": capabilities.sha256(self.root / "build/DesktopCompanion.exe"),
            "cubism_core_sha256": capabilities.sha256(self.root / "build/Live2DCubismCore.dll"),
            "sources": [{"path": proof.relative_to(self.root).as_posix(), "sha256": capabilities.sha256(proof)}]}
        self.policy["parameters"]["ParamKnown"]["evidence"] = ["probe"]
        self.write_json(self.policy_path, self.policy)
        return proof

    def test_unknown_parameter_and_approved_reference_do_not_establish_capability(self):
        report = self.inspect()
        unknown = report["parameters"]["ParamMystery"]
        self.assertEqual(unknown["range_binding_status"], "bound_native_ranges")
        self.assertEqual(unknown["authoring_status"], "unmeasured")
        self.assertEqual(unknown["measured_status"], "unmeasured")
        self.assertEqual(unknown["recommended_channel"], "unknown")
        self.assertEqual(unknown["current_writers"], [])
        self.assertEqual(unknown["motion_references"][0]["approval"], "approved")
        self.assertEqual(unknown["allowed_combinations"]["status"], "unmeasured")

    def test_known_name_layer_and_native_limits_are_still_not_measured_effect(self):
        known = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(known["name"], "Known head angle")
        self.assertEqual(known["native_range"]["min"], -1)
        self.assertTrue(known["declared_layer_bindings"])
        self.assertTrue(known["declared_combined_parameters"])
        self.assertEqual(known["authoring_status"], "unmeasured")
        self.assertEqual(known["allowed_combinations"]["status"], "unmeasured")

    def test_unbound_external_range_dump_is_explicitly_unverified(self):
        self.write_json(self.ranges_path, self.native)
        known = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(known["range_binding_status"], "unverified_model_binding")
        self.assertEqual(known["measured_status"], "unmeasured")

    def test_model_revision_does_not_inherit_prior_probe_result(self):
        self.add_probe()
        original = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(original["measured_status"], "measured")
        self.assertEqual(original["measured_scopes"], ["one axis at neutral; vertex displacement only"])
        self.write(self.model / "fixture.moc3", b"model revision two; same parameter ID")
        self.write_bound_ranges()
        changed = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(changed["range_binding_status"], "bound_native_ranges")
        self.assertEqual(changed["measured_status"], "unmeasured")
        self.assertEqual(changed["evidence"][0]["binding_status"], "stale_model")

    def test_changed_renderer_or_probe_is_not_current_measurement(self):
        proof = self.add_probe()
        self.write(self.root / "build/DesktopCompanion.exe", b"new runtime constraints")
        known = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(known["measured_status"], "unmeasured")
        self.assertEqual(known["evidence"][0]["binding_status"], "stale_runtime")
        self.write_json(proof, {"overwritten": True})
        known = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(known["evidence"][0]["binding_status"], "missing_or_changed_evidence")

    def test_probe_with_only_model_signature_remains_historical_unbound(self):
        self.add_probe()
        probe = self.policy["evidence"]["probe"]
        for key in ("renderer_sha256", "cubism_core_sha256"):
            probe.pop(key)
        self.write_json(self.policy_path, self.policy)
        known = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(known["evidence"][0]["binding_status"], "historical_unbound")
        self.assertEqual(known["measured_status"], "unmeasured")

    def test_probe_with_changed_core_cannot_inherit_native_measurement(self):
        self.add_probe()
        self.write(self.root / "build/Live2DCubismCore.dll", b"new Cubism Core")
        known = self.inspect()["parameters"]["ParamKnown"]
        self.assertEqual(known["evidence"][0]["binding_status"], "stale_runtime")
        self.assertEqual(known["measured_status"], "unmeasured")

    def test_current_validator_policy_overrides_declared_native_ranges(self):
        report = self.inspect()
        for name in ("ParamBrowLY", "ParamShiftY"):
            with self.subTest(parameter=name):
                item = report["parameters"][name]
                self.assertTrue(item["native_present"])
                self.assertEqual(item["authoring_status"], "blocked")
                self.assertTrue(item["prohibited_reasons"])

    def test_runtime_logical_domain_is_not_replaced_by_native_gape_range(self):
        project = Path(capabilities.__file__).resolve().parents[1]
        source_policy = capabilities.read_json(project / capabilities.POLICY_REL)
        self.policy["parameters"]["ParamMouthGape"] = source_policy["parameters"]["ParamMouthGape"]
        self.write_json(self.policy_path, self.policy)
        self.native["parameters"]["ParamMouthGape"] = {"min": 1, "max": 2, "default": 1}
        self.native["parameter_count"] = len(self.native["parameters"])
        self.write_bound_ranges()
        item = self.inspect()["parameters"]["ParamMouthGape"]
        self.assertEqual((item["native_range"]["min"], item["native_range"]["max"]), (1, 2))
        self.assertEqual((item["runtime_input_domain"]["min"], item["runtime_input_domain"]["max"]), (0, 1))
        self.assertFalse(item["runtime_input_domain"]["native_range_is_authoring_domain"])
        self.assertEqual(item["resolution_contract"]["native_write"]["off"], 0)
        self.assertEqual(item["resolution_contract"]["native_write"]["on"], 1)
        self.assertFalse(item["resolution_contract"]["maps_to_native_1_2"])
        self.assertEqual(item["resolution_contract"]["post_update_drawable_opacity"]["off"], 0)

    def test_reaction_ownership_follows_current_code_instead_of_old_body_policy(self):
        project = Path(capabilities.__file__).resolve().parents[1]
        source = project / "src/ParameterMotion.cpp"
        self.write(self.root / "src/ParameterMotion.cpp", source.read_bytes())
        # The parameter-id aliases live in the shared motion header since the
        # behavior-module refactor; the inspector merges it when present.
        ids_header = project / "src/motion/MotionParameterIds.h"
        if ids_header.is_file():
            self.write(self.root / "src/motion/MotionParameterIds.h", ids_header.read_bytes())
        self.policy = capabilities.read_json(project / capabilities.POLICY_REL)
        self.write_json(self.policy_path, self.policy)
        report = self.inspect()
        for name in ("ParamBodyAngleX", "ParamBodyAngleZ", "ParamHairFront", "ParamHairBack", "ParamShiftX"):
            with self.subTest(parameter=name):
                self.assertEqual(report["parameters"][name]["reaction_ownership"], "allowed")
        for name in ("ParamBreath", "ParamArmRA", "ParamBusyLaptop", "ParamSitPose"):
            with self.subTest(parameter=name):
                self.assertEqual(report["parameters"][name]["reaction_ownership"], "reserved")
        self.assertEqual(report["parameters"]["ParamBodyAngleY"]["reaction_ownership"], "not_allowed")
        self.assertEqual(report["reaction_policy"]["source_sha256"], capabilities.sha256(source))

    def test_posture_policy_documents_coupled_runtime_drivers(self):
        project = Path(capabilities.__file__).resolve().parents[1]
        policy = capabilities.read_json(project / capabilities.POLICY_REL)
        for identifier in ("ParamBusyLaptop", "ParamSitPose"):
            self.policy["parameters"][identifier] = policy["parameters"][identifier]
        self.write_json(self.policy_path, self.policy)
        report = self.inspect()
        busy = report["parameters"]["ParamBusyLaptop"]["resolution_contract"]
        sitting = report["parameters"]["ParamSitPose"]["resolution_contract"]
        self.assertEqual(busy["dependencies"], ["ParamSitPose"])
        self.assertEqual(sitting["dependencies"], ["ParamBusyLaptop"])
        self.assertEqual(busy["native_write"], "seated_mix >= 0.5 ? 1 : 0")
        self.assertIn("seated_mix", sitting["native_write"])

    def test_fresh_capture_binds_ranges_but_not_visual_effects(self):
        def native_dump(command, **options):
            self.write_json(Path(command[2]), self.native)

        with patch.object(capabilities.subprocess, "run", side_effect=native_dump):
            captured = capabilities.capture_ranges(self.root, self.ranges_path)
        self.assertEqual(captured["model_inputs"], capabilities.model_snapshot(self.root))
        self.assertIn("renderer_sha256", captured)
        self.assertTrue(captured["capture_provenance"]["inputs_unchanged_during_capture"])
        self.assertEqual(self.inspect()["parameters"]["ParamKnown"]["measured_status"], "unmeasured")

    def test_capture_rejects_mutation_and_preserves_previous_range_evidence(self):
        before = self.ranges_path.read_bytes()

        def changed_renderer(command, **options):
            self.write_json(Path(command[2]), self.native)
            self.write(self.root / "build/DesktopCompanion.exe", b"renderer replaced mid-capture")

        with patch.object(capabilities.subprocess, "run", side_effect=changed_renderer):
            with self.assertRaisesRegex(ValueError, "CAP_CAPTURE_RENDERER_CHANGED"):
                capabilities.capture_ranges(self.root, self.ranges_path)
        self.assertEqual(self.ranges_path.read_bytes(), before)
        self.assertFalse(list(self.ranges_path.parent.glob("*.native.json")))

    def test_cli_summary_output_does_not_edit_approved_motion(self):
        motion = self.root / "assets/motions/approved.motion.json"
        before = motion.read_bytes()
        output = self.root / "art/live2d/workflow/capabilities.json"
        summary = io.StringIO()
        with patch.object(capabilities, "ROOT", self.root), redirect_stdout(summary):
            capabilities.main(["--ranges", str(self.ranges_path), "--policy", str(self.policy_path),
                               "--output", str(output), "--text"])
        self.assertIn("ParamKnown", summary.getvalue())
        self.assertIn("unmeasured", summary.getvalue())
        self.assertTrue(output.is_file())
        self.assertEqual(motion.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
