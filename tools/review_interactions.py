"""Review real short-reaction players, including their background and interrupts.

    python tools/review_interactions.py turn-ended-laptop
    python tools/review_interactions.py all

Every run is an isolated pending draft. These scenes exercise motion events and
Native geometry; physical Windows pointer routing and file props are separate.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from review_motion import (
    ROOT, EXE, run_exe, compose_sequence, compose_desktop_scale, validate,
)
from review_session import (
    deployment_snapshot, new_draft_profile, record_session, finish_session,
    write_session, digest,
)

SCENARIOS = ('turn-ended-standing', 'turn-ended-laptop', 'turn-ended-interrupt',
             'head-pat', 'head-pat-busy', 'head-pat-interrupt')
SCOPE = 'real_interaction_player_and_native_model'


def check_scene(path, scenario):
    evidence = json.loads(path.read_text(encoding='utf8'))
    assert evidence['scenario'] == scenario
    frames = evidence['frames']
    assert len(frames) == 75
    assert all(frame['foot_not_head'] for frame in frames), 'Feet entered the head region'
    assert sum(frame['head_center_hit'] for frame in frames) >= 60, 'Head region is misaligned'
    glance = scenario.startswith('turn-ended')
    interaction = 'turn-ended' if glance else 'head-pat'
    active = [frame for frame in frames if frame['interaction'] == interaction]
    assert active, 'The real player never started the reaction'
    if scenario == 'turn-ended-laptop':
        assert all(frame['parameters']['ParamBusyLaptop'] >= 0.99
                   and frame['parameters']['ParamLaptopVisible'] >= 0.99 for frame in active), \
            'Seated glance lost its computer before responding'
        assert frames[-1]['parameters']['ParamBusyLaptop'] < 0.01
    if scenario == 'head-pat-busy':
        assert all(frame['parameters']['ParamBusyLaptop'] >= 0.99 for frame in frames), \
            'Head pat changed the working body'
    if scenario.endswith('interrupt'):
        assert not any(frame['interaction'] for frame in frames if frame['time'] >= 1.2), \
            'A higher-priority event did not cancel the reaction'
    else:
        assert not frames[-1]['interaction'], 'Reaction did not release back to its background'
        assert frames[-1]['parameters']['ParamEyeLOpen'] > 0.85
        # Exponential blending converges to zero; it does not produce exact
        # floating-point zero after a working expression was interrupted.
        assert abs(frames[-1]['parameters']['ParamMouthOpenY']) < 1e-4
    return {'frames': len(frames), 'player_started': True, 'head_region_aligned': True,
            'priority_and_release': 'passed', 'scope': SCOPE}


def review(scenario):
    records = deployment_snapshot()
    motion = 'turn-ended' if scenario.startswith('turn-ended') else 'head-pat'
    authored = json.loads((ROOT / f'assets/motions/{motion}.motion.json').read_text(encoding='utf8'))
    profile = new_draft_profile({'name': scenario, 'motion': motion,
                                'revision': authored.get('revision', 1),
                                'frames': 75, 'prefix': scenario, 'sweeps': {},
                                'manifest': None, 'style': 'sequence'})
    session = record_session(profile, records)
    session.update(capture_scope=SCOPE, scenario=scenario,
                   not_captured=['physical Windows pointer routing', 'desktop icon flight overlay',
                                 'fed-file wrap prop'])
    session['review_tools']['review_interactions.py'] = digest(Path(__file__))
    write_session(profile, session)
    print(f'{scenario}: isolated draft {profile["run"]}', flush=True)
    try:
        run_exe(['--render-interaction', profile['sequence'], scenario],
                watch=(profile['sequence'], 75, 'scene frames'))
        contracts = check_scene(profile['sequence'] / 'scene.json', scenario)
        compose_sequence(profile)
        compose_desktop_scale(profile)
        report = validate(profile)
        report['capture_scope'] = SCOPE
        report['scene_contracts'] = contracts
        profile['output'].mkdir(parents=True, exist_ok=True)
        (profile['output'] / 'scene-check.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
        shutil.copyfile(profile['sequence'] / 'scene.json', profile['output'] / 'scene.json')
        session.update(validation=report)
        finish_session(profile, session)
        print('  Scene checks passed; human approval remains pending.', flush=True)
    except BaseException as error:
        session.update(capture_status='failed', error=str(error))
        write_session(profile, session)
        raise
    return profile['run']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', choices=(*SCENARIOS, 'all'))
    args = parser.parse_args()
    if not EXE.is_file():
        parser.error('Build DesktopCompanion first.')
    deployment_snapshot()
    subprocess.run([sys.executable, str(ROOT / 'tools/validate_live2d_assets.py')], check=True)
    for scenario in SCENARIOS if args.scenario == 'all' else (args.scenario,):
        review(scenario)


if __name__ == '__main__':
    main()
