"""Isolated Native review runs, deployment checks and reproducible evidence."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
MODEL = ROOT / 'assets/live2d/whale-girl'


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def input_files():
    # Metadata and generated motions matter as well as the MOC3/atlas: the
    # renderer resolves feet and mouth drawables through these JSON files.
    return sorted(path for folder in (MODEL, ROOT / 'assets/motions')
                  for path in folder.rglob('*') if path.is_file())


def deployment_snapshot():
    source_names = {path.name for path in (ROOT / 'assets/motions').glob('*.motion.json')}
    deployed_names = {path.name for path in (BUILD / 'assets/motions').glob('*.motion.json')}
    extra = sorted(deployed_names - source_names)
    if extra:
        raise SystemExit('Stale deployed motions are loaded by MotionLibrary; remove/redeploy first:\n  '
                         + '\n  '.join(extra))
    records = []
    for source in input_files():
        relative = source.relative_to(ROOT)
        deployed = BUILD / relative
        records.append({'path': relative.as_posix(), 'source_sha256': digest(source),
                        'deployed_sha256': digest(deployed) if deployed.is_file() else None})
    if not records:
        raise SystemExit('No review assets found.')
    mismatches = [item['path'] for item in records
                  if item['source_sha256'] != item['deployed_sha256']]
    if mismatches:
        raise SystemExit('Review assets differ from build/assets; build/deploy first:\n  '
                         + '\n  '.join(mismatches))
    return records


def new_draft_profile(profile):
    result = dict(profile)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run = BUILD / 'review-runs' / f"{profile['name']}-v{profile['revision']}-{stamp}-{uuid4().hex[:8]}"
    # Fresh directories make rerenders append-only. No earlier PNG, approved
    # sheet, GIF, or the user's editor archive is deleted or replaced.
    run.mkdir(parents=True, exist_ok=False)
    result.update(draft=True, run=run, output=run / 'artifacts',
                  sequence=run / 'sequence', review=run / 'poses',
                  sweeps={kind: run / f'{kind}-sweep' for kind in profile['sweeps']})
    return result


def record_session(profile, records):
    motion = json.loads((ROOT / f"assets/motions/{profile['motion']}.motion.json").read_text(encoding='utf8'))
    try:
        commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True,
                                text=True, check=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        commit = None
    evidence = {
        'schema_version': 1, 'clip': profile['name'], 'motion_file': profile['motion'],
        'review_revision': profile['revision'], 'motion_revision': motion.get('revision', 1),
        'authored_approval': motion.get('approval', 'pending'), 'review_approval': 'pending',
        'capture_status': 'started', 'git_commit': commit,
        'created_utc': datetime.now(timezone.utc).isoformat(),
        'renderer_sha256': digest(BUILD / 'DesktopCompanion.exe'),
        'cubism_core_sha256': digest(BUILD / 'Live2DCubismCore.dll'),
        'review_tools': {name: digest(ROOT / 'tools' / name)
                         for name in ('review_motion.py', 'review_session.py')},
        'fixed_step_seconds': 1 / 15, 'frames_expected': profile['frames'],
        'capture_scope': 'native_model_and_thought_bubble',
        'not_captured': ['desktop icon flight overlay', 'fed-file wrap prop',
                         'input interaction', 'left/right event scene'],
        'inputs': records,
        'human_checks': {'intent_readable': 'pending', 'contact_and_occlusion': 'pending',
                         'expression': 'pending', 'interrupt_and_response': 'pending',
                         'comfortable_on_desktop': 'pending'},
        'metric_policy': 'Motion energy is diagnostic; a quiet hold is valid.'}
    write_session(profile, evidence)
    return evidence


def write_session(profile, evidence):
    (profile['run'] / 'session.json').write_text(
        json.dumps(evidence, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def finish_session(profile, evidence):
    # Editing/deploying while capturing produces a mixed run. Preserve its
    # files as evidence, but do not present it as a reproducible candidate.
    if deployment_snapshot() != evidence['inputs']:
        raise SystemExit('Review inputs changed during capture; run is stale.')
    if digest(BUILD / 'DesktopCompanion.exe') != evidence['renderer_sha256']:
        raise SystemExit('Renderer changed during capture; run is stale.')
    if digest(BUILD / 'Live2DCubismCore.dll') != evidence['cubism_core_sha256']:
        raise SystemExit('Cubism Core changed during capture; run is stale.')
    evidence['artifacts'] = {path.relative_to(profile['run']).as_posix(): digest(path)
                             for path in sorted(profile['output'].glob('*')) if path.is_file()}
    evidence['capture_status'] = 'complete'
    write_session(profile, evidence)
