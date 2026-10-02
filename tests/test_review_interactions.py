"""Validate branch evidence before a Native scene is delivered for approval."""
import copy
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from review_interactions import check_grass_scene


class GrassSceneEvidenceTest(unittest.TestCase):
    def evidence(self):
        phases = ['enter'] * 18 + ['hold'] * 45 + ['respond'] * 24 + ['release'] * 24 + [''] * 69
        frames = []
        for i, phase in enumerate(phases):
            frames.append(dict(time=i / 15, grass_phase=phase, tip_center_hit=phase == 'hold',
                               foot_not_tip=True, active_turns=1 if i < 80 else 0,
                               state=3 if phase else 0,
                               event='body_click_and_double_click_miss' if i == 18 else
                               'tip_click_twice' if i == 63 else '',
                               parameters=dict(ParamHandRGrip=1, ParamArmRA=60, ParamElbowRA=-20,
                                               ParamWristRA=20, ParamGrassReach=1, ParamGrassVisible=1 if phase else 0)))
        return dict(frames=frames)

    def test_correct_click_branch_is_accepted(self):
        report = check_grass_scene(self.evidence(), 'grass-touch')
        self.assertEqual(report['qt_pointer_arbitration'], 'passed')

    def test_approved_entry_requires_default_input_without_previews(self):
        evidence = self.evidence()
        for frame in evidence['frames']:
            frame['preview_enabled'] = False
        evidence['frames'][0]['event'] = 'default_double_click'
        check_grass_scene(evidence, 'grass-approved')
        evidence['frames'][0]['preview_enabled'] = True
        with self.assertRaisesRegex(AssertionError, 'draft previews'):
            check_grass_scene(evidence, 'grass-approved')

    def test_both_branches_in_one_sequence_are_rejected(self):
        evidence = self.evidence()
        evidence['frames'][88]['grass_phase'] = 'timeout'
        with self.assertRaises(AssertionError):
            check_grass_scene(evidence, 'grass-touch')

    def test_wrong_native_tip_region_is_rejected(self):
        evidence = copy.deepcopy(self.evidence())
        evidence['frames'][32]['tip_center_hit'] = False
        with self.assertRaisesRegex(AssertionError, 'tip target'):
            check_grass_scene(evidence, 'grass-touch')

    def test_hover_moving_the_hand_is_rejected(self):
        evidence = self.evidence()
        evidence['frames'][40]['parameters']['ParamArmRA'] = 55
        with self.assertRaisesRegex(AssertionError, 'Hover moved'):
            check_grass_scene(evidence, 'grass-touch')


if __name__ == '__main__':
    unittest.main()
