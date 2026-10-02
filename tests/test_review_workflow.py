"""Regression checks for isolated review evidence; no Native renderer is run.

Run with: python -m unittest discover -s tests -p test_review_workflow.py -v
"""
from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image, ImageDraw


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import review_motion
import review_session


class ReviewWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="companion-review-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.motion = self.root / "assets/motions/idle.motion.json"
        self.model = self.root / "assets/live2d/whale-girl/model.moc3"
        self.write(self.motion, json.dumps({"approval": "approved", "revision": 4}))
        self.write(self.model, b"fixture model")
        self.deploy(self.motion)
        self.deploy(self.model)
        self.write(self.build / "DesktopCompanion.exe", b"fixture renderer")
        self.write(self.build / "Live2DCubismCore.dll", b"fixture Core")
        for name in ("review_motion.py", "review_session.py", "validate_live2d_assets.py"):
            self.write(self.root / "tools" / name, b"fixture review tool")
        for module, values in (
            (review_session, {"ROOT": self.root, "BUILD": self.build,
                              "MODEL": self.model.parent}),
            (review_motion, {"ROOT": self.root, "BUILD": self.build,
                             "EXE": self.build / "DesktopCompanion.exe",
                             "MOTIONS": self.motion.parent,
                             "OUT": self.root / "art/live2d/review"}),
        ):
            for name, value in values.items():
                patcher = patch.object(module, name, value)
                patcher.start()
                self.addCleanup(patcher.stop)

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode("utf8") if isinstance(data, str) else data)

    def deploy(self, source):
        target = self.build / source.relative_to(self.root)
        self.write(target, source.read_bytes())
        return target

    def profile(self):
        return {"name": "idle", "motion": "idle", "revision": 4,
                "frames": 2, "render": "idle", "prefix": "idle",
                "style": "sequence", "manifest": None, "sweeps": {},
                "sequence": self.build / "idle-sequence",
                "review": self.build / "idle-review"}

    def test_branch_clip_cannot_be_mislabelled_as_a_sequential_review(self):
        self.write(self.motion, json.dumps({"duration": 8.6, "interaction": {"respondEnd": 6}}))
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            review_motion.main(['idle', '--draft'])
        self.assertEqual(error.exception.code, 2)

    def start_session(self):
        records = review_session.deployment_snapshot()
        profile = review_session.new_draft_profile(self.profile())
        git_result = subprocess.CompletedProcess([], 0, stdout="fixture-commit\n")
        with patch.object(review_session.subprocess, "run", return_value=git_result):
            evidence = review_session.record_session(profile, records)
        return profile, evidence

    def test_stale_or_missing_deployment_is_rejected_before_capture(self):
        deployed = self.build / self.motion.relative_to(self.root)
        for mutation in ("stale", "missing"):
            with self.subTest(mutation=mutation):
                self.deploy(self.motion)
                if mutation == "stale":
                    self.write(deployed, b"older motion")
                else:
                    deployed.unlink()
                with self.assertRaisesRegex(SystemExit, "Review assets differ from build/assets") as error:
                    review_session.deployment_snapshot()
                self.assertIn("assets/motions/idle.motion.json", str(error.exception))

    def test_deployed_only_top_level_motion_is_rejected(self):
        stale_clip = self.build / "assets/motions/removed.motion.json"
        self.write(stale_clip, json.dumps({"blendSecondsPerParameter": {"ParamSitPose": 3}}))
        with self.assertRaisesRegex(SystemExit, "Stale deployed motions are loaded by MotionLibrary") as error:
            review_session.deployment_snapshot()
        self.assertIn("removed.motion.json", str(error.exception))

    def test_unused_deployed_model_or_nested_motion_does_not_block_capture(self):
        self.write(self.build / "assets/live2d/whale-girl/unused-atlas.png", b"unused atlas")
        self.write(self.build / "assets/motions/archive/removed.motion.json", b"not loaded by the library")
        records = review_session.deployment_snapshot()
        self.assertEqual(len(records), 2)

    def test_new_clip_is_discovered_and_profiles_its_duration(self):
        path = self.motion.parent / "head-pat.motion.json"
        self.write(path, json.dumps({"duration": 2.4, "revision": 7,
                                    "tracks": {"ParamAngleZ": [[0, 0], [2.4, 0]]}}))
        self.write(self.motion.parent / "archive/old.motion.json", "{}")
        self.write(self.motion.parent / "notes.json", "{}")
        self.write(self.motion.parent / "busy.motion.json", "{}")
        clips = review_motion.available_clips()
        self.assertIn("head-pat", clips)
        self.assertNotIn("old", clips)
        self.assertNotIn("notes", clips)
        self.assertNotIn("busy", clips)  # reserved Native state alias
        profile = review_motion.profile("head-pat")
        self.assertEqual(profile["render"], "head-pat")
        self.assertEqual(profile["motion"], "head-pat")
        self.assertEqual(profile["frames"], 36)
        self.assertEqual(profile["revision"], 7)
        self.assertEqual(profile["style"], "sequence")
        self.assertIsNone(profile["manifest"])
        self.assertEqual(profile["sequence"], self.build / "head-pat-sequence")
        self.assertEqual(review_motion.profile("head-pat", revision=8)["revision"], 8)

    def test_short_new_clip_matches_native_half_up_frame_count(self):
        self.write(self.motion.parent / "quick-glance.motion.json",
                   json.dumps({"duration": 0.3, "constants": {"ParamAngleX": 3}}))
        # The Native CLI publishes five 15 FPS frames for 0.3s (std::lround),
        # rather than Python round's four frames at the half-frame boundary.
        self.assertEqual(review_motion.profile("quick-glance")["frames"], 5)

    def test_new_clip_infers_duration_from_authored_endpoints(self):
        path = self.motion.parent / "reply.motion.json"
        motion = {"keyframes": [{"time": 0, "parameters": {"ParamAngleX": 0}},
                                 {"time": 1.0, "parameters": {"ParamAngleX": 0}}],
                  "tracks": {"ParamAngleZ": [[0, 0], [1.6, 0]]},
                  "pulses": {"ParamCheek": [{"start": 0.5, "peak": 1.8,
                                              "end": 2.2, "weight": 0.5}]}}
        self.write(path, json.dumps(motion))
        self.assertEqual(review_motion.profile("reply")["frames"], 33)
        self.assertEqual(review_motion.profile("reply")["manifest"], path)
        # An explicitly authored positive duration wins over later channel
        # endpoints, matching the Native clip parser's own duration contract.
        self.write(path, json.dumps({**motion, "duration": 1.2}))
        self.assertEqual(review_motion.profile("reply")["frames"], 18)

    def test_legacy_profiles_keep_aliases_frames_and_revision_paths(self):
        for name, data in (
            ("grass", {"duration": 6.9, "revision": 7}),
            ("busy-stand", {"duration": 12, "revision": 5}),
            ("busy-laptop", {"duration": 8, "revision": 2}),
            # Existing sequence profiles keep their approved sheet layout even
            # when their source data also happens to contain shared keyframes.
            ("idle", {"duration": 12, "revision": 1, "keyframes": [{"time": 0}]}),
            ("delete", {"duration": 3.2, "revision": 6, "keyframes": [{"time": 0}]}),
        ):
            self.write(self.motion.parent / f"{name}.motion.json", json.dumps(data))
        grass = review_motion.profile("grass")
        standing = review_motion.profile("busy-stand")
        laptop = review_motion.profile("busy-laptop", revision=3)
        self.assertEqual(grass["frames"], 120)
        self.assertEqual(grass["review"], self.build / "motion-review")
        self.assertEqual(grass["manifest"], self.motion.parent / "grass.motion.json")
        self.assertEqual(standing["render"], "busy")
        self.assertEqual(standing["frames"], 195)
        self.assertEqual(laptop["frames"], 195)
        self.assertEqual(laptop["review"], self.build / "laptop-v3-review")
        self.assertEqual(laptop["manifest"], "generate")
        for name in ("idle", "busy-stand", "delete"):
            self.assertIsNone(review_motion.profile(name)["manifest"])

    def test_new_shared_keyframes_capture_exact_native_poses_before_sequence(self):
        source = self.motion.parent / "head-pat.motion.json"
        poses = [{"time": 0, "label": "觉察", "parameters": {"ParamAngleZ": 0}},
                 {"time": 0.8, "label": "笑眼保持", "parameters": {"ParamAngleZ": 4.5}},
                 {"time": 1.6, "label": "保持闭环", "parameters": {"ParamAngleZ": 4.5}},
                 {"time": 2.2, "label": "回正", "parameters": {"ParamAngleZ": 0}}]
        self.write(source, json.dumps({"duration": 2.2, "keyframes": poses}))
        profile = review_session.new_draft_profile(review_motion.profile("head-pat"))
        self.assertEqual(profile["manifest"], source)
        with patch.object(review_motion, "run_exe") as native:
            review_motion.capture(profile, log=lambda _: None)
        self.assertEqual(native.call_count, 2)
        key_call, sequence_call = native.call_args_list
        self.assertEqual(key_call.args[0],
                         ["--review-motion", profile["review"], source, "head-pat"])
        self.assertEqual(key_call.kwargs["watch"], (profile["review"], 4, "poses"))
        self.assertEqual(sequence_call.args[0],
                         ["--render-motion", profile["sequence"], "head-pat"])
        self.assertEqual(sequence_call.kwargs["watch"], (profile["sequence"], 33, "frames"))
        self.assertEqual(json.loads(source.read_text())["keyframes"], poses)

    def test_new_keyframe_artifact_contains_all_labels_times_and_native_captures(self):
        source = self.motion.parent / "head-pat.motion.json"
        poses = [{"time": 0, "label": "初始正视", "parameters": {"ParamAngleZ": 0}},
                 {"time": 0.8, "label": "闭眼浅笑", "parameters": {"ParamAngleZ": 4.5}},
                 {"time": 1.6, "label": "松手收尾", "parameters": {"ParamAngleZ": 4.5}},
                 {"time": 2.2, "label": "完全回正", "parameters": {"ParamAngleZ": 0}}]
        self.write(source, json.dumps({"duration": 2.2, "revision": 3,
                                      "approval": "pending", "keyframes": poses}))
        profile = review_session.new_draft_profile(review_motion.profile("head-pat"))
        profile["review"].mkdir()
        colors = [(176, 52, 68, 255), (56, 132, 188, 255),
                  (68, 156, 92, 255), (176, 140, 44, 255)]
        for index, color in enumerate(colors):
            Image.new("RGBA", (640, 640), color).save(
                profile["review"] / f"head-pat-{index:02}.png")
        captions = []
        text_lines = []
        original = ImageDraw.ImageDraw.multiline_text
        original_text = ImageDraw.ImageDraw.text

        def record_caption(draw, xy, text, **kwargs):
            captions.append(text)
            return original(draw, xy, text, **kwargs)

        def record_text(draw, xy, text, *args, **kwargs):
            text_lines.append(text)
            return original_text(draw, xy, text, *args, **kwargs)

        with patch.object(review_motion, "compose_sequence") as sequence, \
                patch.object(ImageDraw.ImageDraw, "multiline_text", record_caption), \
                patch.object(ImageDraw.ImageDraw, "text", record_text):
            review_motion.compose(profile, log=lambda _: None)
        sequence.assert_called_once()
        self.assertEqual(captions, [f"{index + 1}. {pose['label']}   {pose['time']:g}s"
                                    for index, pose in enumerate(poses)])
        self.assertTrue(any(text.startswith("摸头回应 · Live2D Native") for text in text_lines))
        self.assertTrue(any("动作数据待审批 / 本次草稿待复核" in text for text in text_lines))
        artifact = profile["output"] / "head-pat-keyframes-v3.png"
        with Image.open(artifact) as sheet:
            self.assertEqual(sheet.size, (1120, 1204))
            for index, color in enumerate(colors):
                x, y = index % 2 * 560 + 280, 90 + index // 2 * 550 + 250
                self.assertEqual(sheet.getpixel((x, y)), color[:3])
        self.assertEqual(review_motion.review_label(profile, "待审批"),
                         "动作数据待审批 / 本次草稿待复核")
        self.assertEqual(json.loads(source.read_text())["approval"], "pending")

    def test_keyframe_manifest_cannot_be_composed_from_missing_or_shifted_poses(self):
        source = self.motion.parent / "reply.motion.json"
        self.write(source, json.dumps({"duration": 1, "keyframes": [
            {"time": 0, "parameters": {"ParamAngleX": 0}},
            {"time": 1, "parameters": {"ParamAngleX": 0}}]}))
        profile = review_session.new_draft_profile(review_motion.profile("reply"))
        profile["review"].mkdir()
        Image.new("RGBA", (16, 16), "red").save(profile["review"] / "reply-00.png")
        with self.assertRaisesRegex(SystemExit, "expected 2 Native key poses, found 1"):
            review_motion.compose_keyframes(profile, log=lambda _: None)
        Image.new("RGBA", (16, 16), "blue").save(profile["review"] / "reply-02.png")
        with self.assertRaisesRegex(SystemExit, "indices do not match the manifest"):
            review_motion.compose_keyframes(profile, log=lambda _: None)
        self.assertFalse((profile["output"] / "reply-keyframes-v1.png").exists())

    def test_one_frame_capture_validates_without_claiming_motion_measurement(self):
        profile = self.profile()
        profile["frames"] = 1
        profile["sequence"].mkdir(parents=True)
        frame = Image.new("RGBA", (32, 32))
        ImageDraw.Draw(frame).rectangle((8, 8, 23, 23), fill=(64, 96, 128, 255))
        frame.save(profile["sequence"] / "frame-000.png")
        with patch("builtins.print"):
            report = review_motion.validate(profile)
        self.assertEqual(report["sequence_frames"], 1)
        self.assertEqual(report["floor_drift_pixels"], 0)
        self.assertIsNone(report["legibility"]["energy"])
        self.assertIsNone(report["legibility"]["still_ratio"])

    def test_one_frame_clip_can_produce_approval_artifacts(self):
        profile = self.profile()
        profile.update(frames=1, output=self.root / "single-frame-artifacts")
        profile["sequence"].mkdir(parents=True)
        frame = Image.new("RGBA", (32, 32))
        ImageDraw.Draw(frame).rectangle((8, 8, 23, 23), fill=(64, 96, 128, 255))
        frame.save(profile["sequence"] / "frame-000.png")
        review_motion.compose(profile, log=lambda _: None)
        self.assertTrue((profile["output"] / "idle-frames-v4.png").is_file())
        self.assertTrue((profile["output"] / "idle-motion-v4.gif").is_file())

    def test_draft_capture_preserves_legacy_and_prior_run(self):
        original = self.profile()
        legacy_frame = original["sequence"] / "frame-000.png"
        approved = review_motion.OUT / "idle-motion-v4.gif"
        self.write(legacy_frame, b"previous Native frame")
        self.write(approved, b"approved GIF")
        first = review_session.new_draft_profile(original)
        second = review_session.new_draft_profile(original)

        def native_capture(args, *unused, **kwargs):
            self.write(Path(args[1]) / "frame-000.png", b"new Native frame")

        with patch.object(review_motion, "run_exe", side_effect=native_capture):
            review_motion.capture(first, log=lambda _: None)
            self.write(first["sequence"] / "frame-000.png", b"first run retained")
            review_motion.capture(second, log=lambda _: None)

        self.assertNotEqual(first["run"], second["run"])
        self.assertEqual(legacy_frame.read_bytes(), b"previous Native frame")
        self.assertEqual(approved.read_bytes(), b"approved GIF")
        self.assertEqual((first["sequence"] / "frame-000.png").read_bytes(), b"first run retained")
        self.assertEqual((second["sequence"] / "frame-000.png").read_bytes(), b"new Native frame")
        self.assertEqual(review_motion.output_folder(second), second["run"] / "artifacts")

    def test_completed_capture_does_not_inherit_authored_approval(self):
        profile, evidence = self.start_session()
        self.write(profile["output"] / "preview.gif", b"captured preview")
        review_session.finish_session(profile, evidence)
        saved = json.loads((profile["run"] / "session.json").read_text(encoding="utf8"))
        self.assertEqual(saved["capture_status"], "complete")
        self.assertEqual(saved["authored_approval"], "approved")
        self.assertEqual(saved["review_approval"], "pending")
        self.assertTrue(all(status == "pending" for status in saved["human_checks"].values()))
        self.assertIn("artifacts/preview.gif", saved["artifacts"])
        self.assertEqual(json.loads(self.motion.read_text())["approval"], "approved")

    def test_source_change_during_capture_is_rejected_even_if_redeployed(self):
        profile, evidence = self.start_session()
        self.write(self.model, b"new model during capture")
        self.deploy(self.model)
        with self.assertRaisesRegex(SystemExit, "Review inputs changed during capture"):
            review_session.finish_session(profile, evidence)
        saved = json.loads((profile["run"] / "session.json").read_text())
        self.assertNotEqual(saved["capture_status"], "complete")
        self.assertEqual(saved["review_approval"], "pending")

    def test_renderer_or_core_change_during_capture_is_rejected(self):
        for filename, error_label in (
            ("DesktopCompanion.exe", "Renderer changed during capture"),
            ("Live2DCubismCore.dll", "Cubism Core changed during capture"),
        ):
            with self.subTest(filename=filename):
                profile, evidence = self.start_session()
                self.write(self.build / filename, b"replaced executable dependency")
                with self.assertRaisesRegex(SystemExit, error_label):
                    review_session.finish_session(profile, evidence)

    def test_draft_cannot_relabel_existing_capture_as_fresh(self):
        diagnostic = io.StringIO()
        with redirect_stderr(diagnostic), self.assertRaises(SystemExit) as error:
            review_motion.main(["idle", "--draft", "--no-render"])
        self.assertEqual(error.exception.code, 2)
        self.assertIn("--draft requires fresh Native captures", diagnostic.getvalue())
        self.assertFalse((self.build / "review-runs").exists())


if __name__ == "__main__":
    unittest.main()
