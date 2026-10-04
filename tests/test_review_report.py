"""Evidence and capability boundaries for the cross-model review report."""
from contextlib import redirect_stderr
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import review_report


class ReviewReportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='pet-review-report-')
        self.addCleanup(temporary.cleanup)
        self.run = Path(temporary.name)
        self.artifact = self.run / 'artifacts/native-sheet.png'
        self.artifact.parent.mkdir()
        Image.new('RGBA', (32, 32), (40, 80, 120, 255)).save(self.artifact)
        self.session = {
            'clip': 'head-pat', 'motion_revision': 1, 'authored_approval': 'approved',
            'capture_status': 'complete', 'frames_expected': 33,
            'capture_scope': 'native_model_and_thought_bubble',
            'provenance_checks': dict.fromkeys(
                ('source_deployment_inputs', 'renderer', 'cubism_core', 'review_tools'), 'passed'),
            'validation': {'sequence_frames': 33, 'alpha_border_failures': 0,
                           'floor_drift_pixels': 0, 'legibility': {'energy': 0.01}},
            'artifacts': {'artifacts/native-sheet.png': review_report.digest(self.artifact)},
            'not_captured': ['physical Windows pointer routing'], 'inputs': [],
        }
        self.save_session()

    def save_session(self):
        review_report.write_json(self.run / 'session.json', self.session)

    def visual_review(self):
        review = {
            'source_session_sha256': review_report.digest(self.run / 'session.json'),
            'reviewer': {'name': 'visual reviewer fixture', 'role': 'visual', 'can_view_images': True},
            'checks': dict.fromkeys(review_report.VISUAL_CHECKS, 'passed'),
            'reviewed_artifacts': [{'path': 'artifacts/native-sheet.png',
                                    'sha256': review_report.digest(self.artifact),
                                    'coverage': 'All labelled poses; no continuous frames in this fixture.'}],
            'issues': [],
        }
        review['checks']['interrupt_and_response'] = 'not_applicable'
        return review

    def test_visual_review_cannot_claim_an_uncaptured_interruption(self):
        review = self.visual_review()
        review['checks']['interrupt_and_response'] = 'passed'
        with self.assertRaisesRegex(ValueError, 'does not exercise input or interruption'):
            review_report.validate_visual_review(self.run, self.session, review)

    def test_unknown_scope_or_non_interrupt_scene_cannot_claim_interruption(self):
        for scope, scenario in (('unknown', 'head-pat-interrupt'),
                                ('real_interaction_player_and_native_model', 'head-pat')):
            with self.subTest(scope=scope, scenario=scenario):
                self.session.update(capture_scope=scope, scenario=scenario)
                self.save_session()
                review = self.visual_review()
                review['checks']['interrupt_and_response'] = 'passed'
                with self.assertRaisesRegex(ValueError, 'supported interruption scene'):
                    review_report.validate_visual_review(self.run, self.session, review)

    def test_json_metrics_alone_are_not_visual_evidence(self):
        metric = self.run / 'artifacts/scene-check.json'
        metric.write_text('{}', encoding='utf8')
        self.session['artifacts'][metric.relative_to(self.run).as_posix()] = review_report.digest(metric)
        self.save_session()
        review = self.visual_review()
        review['reviewed_artifacts'] = [{'path': 'artifacts/scene-check.json',
                                        'sha256': review_report.digest(metric), 'coverage': 'All metrics'}]
        with self.assertRaisesRegex(ValueError, 'inspected image/GIF'):
            review_report.validate_visual_review(self.run, self.session, review)

    def test_automatic_checks_never_inherit_visual_or_user_approval(self):
        report = review_report.write_review_report(self.run)
        self.assertEqual(report['engineering']['status'], 'passed')
        self.assertEqual(report['visual']['status'], 'pending')
        self.assertEqual(report['adoption']['status'], 'pending')
        self.assertEqual(report['adoption']['authored_motion_approval'], 'approved')
        self.assertEqual(json.loads((self.run / 'session.json').read_text()), self.session)
        self.assertIn('physical Windows pointer routing', report['engineering']['not_verified'])

    def test_text_executor_cannot_submit_a_passing_visual_review(self):
        for role, capability in (('text', False), ('text', True), ('visual', False)):
            with self.subTest(role=role, capability=capability):
                review = self.visual_review()
                review['reviewer'].update(role=role, can_view_images=capability)
                with self.assertRaisesRegex(ValueError, 'image-capable'):
                    review_report.validate_visual_review(self.run, self.session, review)

    def test_visual_review_is_bound_to_session_and_inspected_artifacts(self):
        review = self.visual_review()
        self.session['motion_revision'] = 2
        self.save_session()
        with self.assertRaisesRegex(ValueError, 'changed session'):
            review_report.validate_visual_review(self.run, self.session, review)
        review = self.visual_review()
        self.artifact.write_bytes(b'Replaced preview')
        with self.assertRaisesRegex(ValueError, 'Visual evidence'):
            review_report.validate_visual_review(self.run, self.session, review)

    def test_changed_or_missing_artifacts_fail_engineering_checks(self):
        for missing in (False, True):
            with self.subTest(missing=missing):
                self.artifact.write_bytes(b'Changed capture')
                if missing:
                    self.artifact.unlink()
                report = review_report.build_report(self.run, self.session)
                self.assertEqual(report['engineering']['status'], 'failed')
                self.assertEqual(report['adoption']['status'], 'pending')

    def test_visual_review_never_adopts_and_stales_after_input_changes(self):
        review_report.write_json(self.run / 'visual-review.json', self.visual_review())
        report = review_report.write_review_report(self.run)
        self.assertEqual(report['visual']['status'], 'reviewed')
        self.assertEqual(report['adoption']['status'], 'pending')
        self.session['inputs'].append({'path': 'model.moc3', 'source_sha256': 'different'})
        self.save_session()
        self.assertEqual(review_report.write_review_report(self.run)['visual']['status'], 'invalid')

    def test_missing_provenance_or_metrics_cannot_claim_engineering_pass(self):
        for mutation in ('provenance', 'partial_provenance', 'metrics', 'started'):
            with self.subTest(mutation=mutation):
                session = copy.deepcopy(self.session)
                if mutation == 'provenance':
                    session.pop('provenance_checks')
                elif mutation == 'partial_provenance':
                    session['provenance_checks'] = {'renderer': 'passed'}
                elif mutation == 'metrics':
                    session['validation'] = {'legibility': {'energy': 100}}
                else:
                    session['capture_status'] = 'started'
                self.assertEqual(review_report.build_report(self.run, session)['engineering']['status'], 'pending')

    def test_failed_visual_checks_need_an_actionable_handoff_issue(self):
        review = self.visual_review()
        review['checks']['contact_and_occlusion'] = 'failed'
        with self.assertRaisesRegex(ValueError, 'actionable issue'):
            review_report.validate_visual_review(self.run, self.session, review)
        review['issues'] = [{
            'id': 'contact-1', 'scene': 'sit-entry', 'location': 'frame 8 / material switch',
            'parts': ['neck', 'collar'], 'observed': 'Example gap', 'expected': 'Continuous skin contact',
            'allowed_changes': ['neck binding'], 'preserve': ['approved facial silhouette'],
            'recheck': ['sit-entry', 'reverse transition'],
        }]
        review_report.write_json(self.run / 'visual-review.json', review)
        report = review_report.write_review_report(self.run)
        self.assertEqual(report['visual']['status'], 'changes_requested')
        self.assertIn('contact-1', (self.run / 'review-summary.txt').read_text(encoding='utf8'))

    def test_reviewer_cannot_reference_evidence_outside_the_run(self):
        review = self.visual_review()
        review['reviewed_artifacts'][0]['path'] = '../other-run.png'
        with self.assertRaisesRegex(ValueError, 'within this capture run'):
            review_report.validate_visual_review(self.run, self.session, review)

    def test_failed_import_preserves_existing_review(self):
        existing = self.visual_review()
        review_report.write_json(self.run / 'visual-review.json', existing)
        supplied = self.visual_review()
        supplied['reviewer']['can_view_images'] = False
        review_report.write_json(self.run / 'incoming.json', supplied)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            review_report.main([str(self.run), '--visual-review', str(self.run / 'incoming.json')])
        self.assertEqual(review_report.read_json(self.run / 'visual-review.json'), existing)

    def test_malformed_external_visual_record_cannot_break_capture_reporting(self):
        for malformed in ([], {'reviewer': []}, {'reviewer': {'name': 'x'}}):
            with self.subTest(malformed=malformed):
                if isinstance(malformed, dict):
                    malformed['source_session_sha256'] = review_report.digest(self.run / 'session.json')
                review_report.write_json(self.run / 'visual-review.json', malformed)
                report = review_report.write_review_report(self.run)
                self.assertEqual(report['visual']['status'], 'invalid')
                self.assertEqual(report['adoption']['status'], 'pending')


if __name__ == '__main__':
    unittest.main()
