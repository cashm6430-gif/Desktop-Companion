"""Report capture facts separately from visual review and user adoption.

Every draft writes review-report.json, review-summary.txt and a visual review
template. Executors declare their role and whether they can inspect images;
this records accountability, not automatic identification of model capability.
Text-only executors must leave visual checks pending. The template binds
observations to session and artifact hashes; it never changes motion approval.

    python tools/review_report.py build/review-runs/<run>
    python tools/review_report.py build/review-runs/<run> --visual-review <review.json>
"""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


VISUAL_CHECKS = ('intent_readable', 'contact_and_occlusion', 'expression',
                 'interrupt_and_response', 'comfortable_on_desktop')


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf8'))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def artifact_path(run, name):
    path = (run / name).resolve()
    if Path(name).is_absolute() or not path.is_relative_to(run.resolve()):
        raise ValueError('Evidence artifact must stay within this capture run: ' + name)
    return path


def validate_visual_review(run, session, review):
    """Validate attribution, scope and evidence, not the reviewer's aesthetics."""
    if not isinstance(review, dict):
        raise ValueError('Visual review must be a JSON object.')
    if review.get('source_session_sha256') != digest(run / 'session.json'):
        raise ValueError('Visual review belongs to a different or changed session.')
    actor = review.get('reviewer', {})
    if (not isinstance(actor, dict) or actor.get('can_view_images') is not True
            or actor.get('role') not in ('visual', 'user')
            or not str(actor.get('name', '')).strip()):
        raise ValueError('Visual review requires a named image-capable visual reviewer or user.')
    checks = review.get('checks', {})
    if not isinstance(checks, dict) or set(checks) != set(VISUAL_CHECKS) or any(
            value not in ('passed', 'failed', 'pending', 'not_applicable') for value in checks.values()):
        raise ValueError('Each visual check must be passed, failed, pending or not_applicable.')
    if checks['interrupt_and_response'] == 'passed':
        scopes = ('real_interaction_player_and_native_model',
                  'window_player_and_synthetic_qt_pointer_with_native_model')
        scenarios = ('turn-ended-interrupt', 'head-pat-interrupt', 'grass-delete-busy')
        if session.get('capture_scope') not in scopes or session.get('scenario') not in scenarios:
            raise ValueError('This capture does not exercise input or interruption in a supported '
                             'interruption scene; mark the check pending/not_applicable.')
    artifacts = session.get('artifacts', {})
    inspected = review.get('reviewed_artifacts', [])
    if not isinstance(inspected, list) or not inspected or not all(isinstance(item, dict) for item in inspected):
        raise ValueError('List the artifacts actually inspected, with their captured hashes.')
    visual_artifact_seen = False
    for item in inspected:
        name = item.get('path', '')
        expected = artifacts.get(name)
        path = artifact_path(run, name)
        if not expected or item.get('sha256') != expected or not path.is_file() or digest(path) != expected:
            raise ValueError('Visual evidence is missing, changed or outside the session: ' + name)
        if not str(item.get('coverage', '')).strip():
            raise ValueError('Describe inspected frames, time ranges or poses: ' + name)
        if path.suffix.lower() in ('.png', '.gif', '.jpg', '.jpeg', '.webp'):
            try:
                with Image.open(path) as picture:
                    picture.verify()
                visual_artifact_seen = True
            except (OSError, ValueError) as error:
                raise ValueError('Visual evidence must be a readable image: ' + name) from error
    if not visual_artifact_seen:
        raise ValueError('Visual review needs an inspected image/GIF; JSON metrics are supporting facts only.')
    for issue in review.get('issues', []):
        required = ('id', 'scene', 'location', 'parts', 'observed', 'expected',
                    'allowed_changes', 'preserve', 'recheck')
        if not isinstance(issue, dict) or any(not issue.get(key) for key in required):
            raise ValueError('Visual issues need scene/location/parts/expected/change boundaries/recheck.')
    if all(value == 'not_applicable' for value in checks.values()):
        raise ValueError('A visual review cannot mark every check not_applicable.')
    if 'failed' in checks.values() and not review.get('issues'):
        raise ValueError('A failed visual check needs an actionable issue for the next executor.')


def build_report(run, session):
    run = Path(run)
    state = session.get('capture_status', 'unknown')
    validation = session.get('validation') or {}
    failures = []
    if state == 'failed':
        failures.append(session.get('error') or 'Capture failed; inspect the capture log.')
    artifacts = session.get('artifacts', {})
    for name, expected in artifacts.items():
        try:
            path = artifact_path(run, name)
            if not path.is_file() or digest(path) != expected:
                failures.append('Artifact missing or changed: ' + name)
        except ValueError as error:
            failures.append(str(error))
    if state == 'complete' and not artifacts:
        failures.append('Completed session has no hashed review artifacts.')
    required_metrics = ('sequence_frames', 'alpha_border_failures', 'floor_drift_pixels')
    provenance = session.get('provenance_checks', {})
    provenance_complete = all(provenance.get(key) == 'passed' for key in
                              ('source_deployment_inputs', 'renderer', 'cubism_core', 'review_tools'))
    complete = (state == 'complete' and all(key in validation for key in required_metrics)
                and bool(artifacts) and provenance_complete)
    if 'failed' in provenance.values():
        failures.append('Capture provenance check failed.')
    if validation.get('alpha_border_failures', 0) != 0:
        failures.append('Native captures touch the frame border.')
    expected = session.get('frames_expected')
    if expected is not None and validation.get('sequence_frames', expected) != expected:
        failures.append('Validated frame count differs from the capture contract.')
    engineering = 'failed' if failures else 'passed' if complete else 'pending'
    facts = {
        'capture_status': state,
        'capture_scope': session.get('capture_scope', 'unknown'),
        'sequence_frames': validation.get('sequence_frames'),
        'native_poses': validation.get('native_poses'),
        'alpha_border_failures': validation.get('alpha_border_failures'),
        'floor_drift_pixels': validation.get('floor_drift_pixels'),
        'scene_contracts': validation.get('scene_contracts', {}),
        'provenance_checks': session.get('provenance_checks', {}),
    }
    visual = {'status': 'pending', 'reviewer': None, 'checks': dict.fromkeys(VISUAL_CHECKS, 'pending'),
              'reviewed_artifacts': [], 'issues': []}
    review_path = run / 'visual-review.json'
    if review_path.is_file():
        try:
            review = read_json(review_path)
            validate_visual_review(run, session, review)
            statuses = review['checks'].values()
            status = ('changes_requested' if 'failed' in statuses or review.get('issues') else
                      'pending' if 'pending' in statuses else 'reviewed')
            visual.update(status=status, reviewer=review['reviewer'], checks=review['checks'],
                          reviewed_artifacts=review['reviewed_artifacts'], issues=review.get('issues', []),
                          review_sha256=digest(review_path))
        except (ValueError, OSError, TypeError, KeyError) as error:
            visual.update(status='invalid', error=str(error))
    return {
        'schema_version': 1,
        'session_sha256': digest(run / 'session.json'),
        'report_generator_sha256': digest(Path(__file__)),
        'review_tools_at_capture': session.get('review_tools', {}),
        'clip': session.get('clip'), 'scenario': session.get('scenario'),
        'motion_revision': session.get('motion_revision'),
        'engineering': {'status': engineering, 'scope': 'capture and recorded automatic checks only',
                        'facts': facts, 'failures': failures,
                        'not_verified': session.get('not_captured', []) + [
                            'Full project test suite is not attested by this capture.',
                            'Palm-to-grass and neck contact geometry are not measured by these image checks.']},
        'visual': visual,
        'adoption': {'status': 'pending',
                     'authored_motion_approval': session.get('authored_approval', 'unknown'),
                     'note': 'Existing motion approval does not approve this capture or a changed model. '
                             'User adoption must be recorded against this evidence; this tool never edits approval.'},
        'diagnostics': {'legibility': validation.get('legibility'),
                        'policy': 'Motion energy is diagnostic; quiet holds are valid.'},
        'inputs': session.get('inputs', []),
        'renderer_sha256': session.get('renderer_sha256'),
        'cubism_core_sha256': session.get('cubism_core_sha256'),
        'artifact_sha256': artifacts,
    }


def summary_text(report):
    engineering = report['engineering']
    visual = report['visual']
    facts = engineering['facts']
    lines = [f"动作/场景：{report.get('scenario') or report.get('clip')} · revision {report['motion_revision']}",
             f"工程检查：{engineering['status']}（仅本次捕获及所列自动检查）",
             f"视觉复核：{visual['status']}；本次采用：{report['adoption']['status']}",
             f"源动作审批：{report['adoption']['authored_motion_approval']}，不继承为本次批准。",
             f"捕获范围：{facts['capture_scope']}",
             f"帧数：{facts['sequence_frames']}；边界失败：{facts['alpha_border_failures']}；"
             f"脚底漂移：{facts['floor_drift_pixels']} px",
             f"证据 session SHA256：{report['session_sha256']}"]
    if engineering['failures']:
        lines += ['失败：'] + ['- ' + item for item in engineering['failures']]
    if facts['scene_contracts']:
        lines += ['场景事实：', json.dumps(facts['scene_contracts'], ensure_ascii=False)]
    if report['engineering']['status'] == 'pending':
        lines += ['本次工程依据不完整：需要完整捕获、边界/脚底/帧数检查与部署来源验证。']
    lines += ['尚未验证：'] + ['- ' + item for item in engineering['not_verified']]
    lines += ['视觉检查：'] + [f'- {key}: {value}' for key, value in visual['checks'].items()]
    for issue in visual['issues']:
        lines += [f"问题 {issue['id']}：{issue['scene']} / {issue['location']}",
                  f"- 部件：{issue['parts']}；观察：{issue['observed']}",
                  f"- 预期：{issue['expected']}；允许修改：{issue['allowed_changes']}",
                  f"- 保留：{issue['preserve']}；复核：{issue['recheck']}"]
    if visual.get('error'):
        lines += ['视觉记录无效：' + visual['error']]
    lines += ['交接：纯文本执行者可修改代码、时序与约束，并重跑自动检查。',
              '能看图的执行者填写 visual-review.template.json 后以 --visual-review 导入；',
              '逐项说明实际检查范围。测试与数值不能代替表情、接触与动作自然度的复核。']
    return '\n'.join(lines) + '\n'


def write_review_report(run, session=None):
    run = Path(run)
    session = session if session is not None else read_json(run / 'session.json')
    report = build_report(run, session)
    write_json(run / 'review-report.json', report)
    (run / 'review-summary.txt').write_text(summary_text(report), encoding='utf8')
    template = {
        'schema_version': 1, 'source_session_sha256': report['session_sha256'],
        'reviewer': {'name': '', 'role': 'text', 'can_view_images': False},
        'checks': dict.fromkeys(VISUAL_CHECKS, 'pending'),
        'available_artifacts': report['artifact_sha256'],
        'reviewed_artifacts': [], 'issues': [],
        'issue_fields': ['id', 'scene', 'location (frame/time/phase)', 'parts', 'observed',
                         'expected', 'allowed_changes', 'preserve', 'recheck'],
        'note': 'Copy this template; specify actually inspected artifact path/sha256/coverage. '
                'Do not declare visual capability or user approval on behalf of another executor.'}
    write_json(run / 'visual-review.template.json', template)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path, help='capture directory containing session.json')
    parser.add_argument('--visual-review', type=Path, help='attributed review of this exact evidence')
    args = parser.parse_args(argv)
    try:
        session = read_json(args.run / 'session.json')
        if args.visual_review:
            review = read_json(args.visual_review)
            validate_visual_review(args.run, session, review)
            if session.get('capture_status') != 'complete':
                raise ValueError('Review a complete capture, not an unfinished or failed run.')
            write_json(args.run / 'visual-review.json', review)
        report = write_review_report(args.run, session)
    except (ValueError, OSError, TypeError, KeyError) as error:
        parser.error(str(error))
    print(summary_text(report), end='')
    return 1 if report['engineering']['status'] == 'failed' or report['visual']['status'] == 'invalid' else 0


if __name__ == '__main__':
    raise SystemExit(main())
