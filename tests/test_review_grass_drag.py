"""Evidence validation rejects incomplete or misleading candidate captures."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("review_grass_drag", TOOLS / "review_grass_drag.py")
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class GrassEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pet-grass-evidence-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.path = self.folder / "scene.json"
        drag = {"enabled": False, "pressed": False, "moved": False, "active": False,
                "settling": False, "offset": 0, "normal": [0, 0], "span": 0,
                "geometry": {"valid": True, "root": [10, 10], "mid": [10, 20],
                             "tip": [10, 30], "pinned_vertices": [[i, i] for i in range(37)]}}
        events = {3: "start_approved_grass", 15: "real_background_turn_start",
                  58: "default_tip_press", 63: "qt_tip_tap_release", 147: "real_background_turn_stop"}
        frames = []
        for i in range(156):
            name = f"frame-{i:03}.png"
            (self.folder / name).write_bytes(b"fixture-frame")
            state = copy.deepcopy(drag)
            state["pressed"] = 58 <= i < 63
            frames.append({"frame": i, "file": name, "time": (i + 1) / 15, "dt": 1 / 15,
                "event": events.get(i, ""), "state": 3 if i < 106 else (1 if i < 147 else 0),
                "active_turns": int(15 <= i < 147),
                "grass_phase": "enter" if i < 57 else ("hold" if i == 57 else
                    ("respond" if i < 83 else ("release" if i < 106 else ""))),
                "grass_time": 0,
                "parameters": {"ParamGrassSwing": 0, "ParamGrassTipBend": 0,
                    "ParamHandRGrip": 1, "ParamGrassVisible": 1, "ParamArmRA": 1,
                    "ParamElbowRA": 2, "ParamWristRA": 3},
                "pointer": {"dragging": False, "gesture": "none", "preview_enabled": False},
                "drag": state, "press_point": [10, 30], "mouse_point": [10, 30]})
        keys = ("native_backend", "isolated_settings", "candidate_opt_in_only", "grass_started",
                "background_start_keeps_grass", "actual_tip_hit_found", "grass_does_not_drag_window",
                "hold_and_press_seen", "real_turn_finished_idle", "final_tracking_clear",
                "candidate_or_default_entry", "only_taps_respond", "drag_threshold_or_tap",
                "release_rebounds", "timeout_branch", "background_resumes")
        self.data = {"scenario": "grass-drag-default", "scope": review.SCOPE, "adoption": "pending",
            "passed": True, "failure": "", "render_backend": "cubism_native", "render_error": "",
            "checks": dict.fromkeys(keys, True), "privacy": {"fixture_only": True,
                "isolated_settings": True, "default_endpoint_used": False,
                "real_hooks_used": False, "desktop_files_changed": False},
            "frame_count": 156, "frames": frames, "final_drag": drag}

    def verify(self):
        self.path.write_text(json.dumps(self.data), encoding="utf8")
        return review.check_scene(self.path)

    def test_complete_default_retains_approved_click(self):
        self.assertEqual(self.verify()["scenario"], "grass-drag-default")

    def test_renderer_error_or_self_reported_failure_is_rejected(self):
        for field, value in (("render_error", "failed"), ("passed", False), ("adoption", "approved")):
            original = self.data[field]
            self.data[field] = value
            with self.assertRaises(ValueError):
                self.verify()
            self.data[field] = original

    def test_missing_frame_or_hold_geometry_is_rejected(self):
        self.data["frames"][57]["drag"]["geometry"]["valid"] = False
        with self.assertRaisesRegex(ValueError, "Hold geometry"):
            self.verify()
        self.data["frames"][57]["drag"]["geometry"]["valid"] = True
        (self.folder / "frame-020.png").unlink()
        with self.assertRaisesRegex(ValueError, "Missing Native frame"):
            self.verify()

    def test_unbounded_or_nonfinite_motion_is_rejected(self):
        frame = self.data["frames"][57]
        for target, key, value in ((frame["drag"], "offset", .351),
                                   (frame["parameters"], "ParamGrassSwing", 1.01),
                                   (frame["parameters"], "ParamGrassTipBend", float("nan"))):
            original = target[key]
            target[key] = value
            with self.assertRaises(ValueError):
                self.verify()
            target[key] = original

    def test_missing_pin_or_controller_event_is_rejected(self):
        geometry = self.data["frames"][57]["drag"]["geometry"]
        removed = geometry["pinned_vertices"].pop()
        with self.assertRaisesRegex(ValueError, "pinned control"):
            self.verify()
        geometry["pinned_vertices"].append(removed)
        self.data["frames"][15]["event"] = ""
        with self.assertRaisesRegex(ValueError, "controller events"):
            self.verify()

    def test_released_press_cannot_reappear(self):
        self.data["frames"][100]["drag"]["pressed"] = True
        with self.assertRaisesRegex(ValueError, "replayed after release"):
            self.verify()

    def test_required_check_cannot_be_omitted(self):
        del self.data["checks"]["candidate_opt_in_only"]
        with self.assertRaisesRegex(ValueError, "checks failed or missing"):
            self.verify()

    def test_production_event_source_is_rejected(self):
        self.data["privacy"]["real_hooks_used"] = True
        with self.assertRaisesRegex(ValueError, "production events"):
            self.verify()

    def test_simultaneous_controls_detect_a_moved_palm(self):
        first = copy.deepcopy(self.data)
        second = copy.deepcopy(self.data)
        for scene in (first, second):
            for frame in scene["frames"]:
                frame["grass_phase"] = "hold"
        scenes = {"grass-drag-right": first, "grass-drag-left": second}
        self.assertTrue(review.compare_pins(scenes)["performed"])
        second["frames"][70]["drag"]["geometry"]["pinned_vertices"][0][0] += .1
        with self.assertRaisesRegex(ValueError, "palm/root"):
            review.compare_pins(scenes)


if __name__ == "__main__":
    unittest.main()
