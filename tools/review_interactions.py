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
    ROOT, EXE, run_exe, compose_sequence, compose_desktop_scale, compose_keyframes, validate,
)
from review_session import (
    deployment_snapshot, new_draft_profile, record_session, finish_session,
    write_session, digest,
)

SCENARIOS = ('turn-ended-standing', 'turn-ended-laptop', 'turn-ended-interrupt',
             'head-pat', 'head-pat-busy', 'head-pat-interrupt',
             'grass-touch', 'grass-timeout', 'grass-delete-busy')
SCOPE = 'real_interaction_player_and_native_model'
GRASS_SCOPE = 'window_player_and_synthetic_qt_pointer_with_native_model'


def check_grass_scene(evidence, scenario):
    frames = evidence['frames']
    assert len(frames) == 180
    phases = [frame['grass_phase'] for frame in frames]
    assert 'enter' in phases and 'hold' in phases
    hold = [frame for frame in frames if frame['grass_phase'] == 'hold']
    assert all(frame['tip_center_hit'] for frame in hold), 'Grass tip target missed its rendered center'
    assert all(frame['foot_not_tip'] for frame in frames), 'Feet entered the tip target'
    assert all(frame['parameters']['ParamHandRGrip'] >= 0.99 for frame in hold), 'Grass root lost the palm'
    for axis in ('ParamArmRA', 'ParamElbowRA', 'ParamWristRA', 'ParamGrassReach'):
        # Leave five 120ms blend constants for the arriving wrist pose to settle.
        # Its remaining approach to the fixed target is not hover-driven motion.
        settled = [frame for frame in hold if frame['time'] >= hold[0]['time'] + 0.6]
        assert max(frame['parameters'][axis] for frame in settled) - min(
            frame['parameters'][axis] for frame in settled) < 0.05, f'Hover moved {axis}'
    assert any(frame['event'] == 'body_click_and_double_click_miss' and frame['grass_phase'] == 'hold'
               for frame in frames), 'Body input restarted or accepted the grass interaction'
    assert max(frame['active_turns'] for frame in frames) == 1
    if scenario == 'grass-touch':
        assert 'respond' in phases and 'timeout' not in phases and 'release' in phases
        assert any(frame['event'] == 'tip_click_twice' and frame['grass_phase'] == 'respond' for frame in frames)
        assert sum(before != 'respond' and after == 'respond' for before, after in zip(phases, phases[1:])) == 1
    elif scenario == 'grass-timeout':
        assert 'timeout' in phases and 'respond' not in phases and 'release' in phases
        assert len(hold) / 15 <= 3.1, 'Grass hold exceeded its bounded timeout'
    else:
        interruption = next(frame for frame in frames if frame['event'] == 'delete_interrupt')
        assert interruption['state'] == 2 and not interruption['grass_phase']
        assert all(not frame['grass_phase'] for frame in frames if frame['time'] >= interruption['time'])
        assert frames[-1]['state'] == 1 and frames[-1]['active_turns'] == 1
    if scenario != 'grass-delete-busy':
        assert frames[-1]['state'] == 0 and frames[-1]['active_turns'] == 0 and not phases[-1]
        assert frames[-1]['parameters']['ParamGrassVisible'] < 0.01
    return {'frames': len(frames), 'branch': scenario, 'tip_and_palm': 'passed',
            'qt_pointer_arbitration': 'passed', 'background_and_interrupt': 'passed', 'scope': GRASS_SCOPE}


def check_scene(path, scenario):
    evidence = json.loads(path.read_text(encoding='utf8'))
    assert evidence['scenario'] == scenario
    frames = evidence['frames']
    if scenario.startswith('grass-'):
        return check_grass_scene(evidence, scenario)
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
    grass = scenario.startswith('grass-')
    motion = 'grass-touch' if grass else 'turn-ended' if scenario.startswith('turn-ended') else 'head-pat'
    scope = GRASS_SCOPE if grass else SCOPE
    count = 180 if grass else 75
    authored = json.loads((ROOT / f'assets/motions/{motion}.motion.json').read_text(encoding='utf8'))
    profile = new_draft_profile({'name': scenario, 'motion': motion,
                                'revision': authored.get('revision', 1),
                                'frames': count, 'prefix': scenario, 'sweeps': {},
                                'manifest': None, 'style': 'sequence',
                                'capture_caption': ('模型与Qt输入分支；Windows真实鼠标路由仍需桌面复核。'
                                                    if grass else None)})
    session = record_session(profile, records)
    session.update(capture_scope=scope, scenario=scenario,
                   not_captured=['physical Windows pointer routing', 'desktop icon flight overlay',
                                 'fed-file wrap prop'])
    session['review_tools']['review_interactions.py'] = digest(Path(__file__))
    write_session(profile, session)
    print(f'{scenario}: isolated draft {profile["run"]}', flush=True)
    try:
        if scenario == 'grass-touch':
            # Key poses are reviewed separately; the sequence below follows
            # only the selected branch through the actual window player.
            profile['manifest'] = ROOT / f'assets/motions/{motion}.motion.json'
            key_count = len(authored['keyframes'])
            run_exe(['--review-motion', profile['review'], profile['manifest'], profile['prefix']],
                    watch=(profile['review'], key_count, 'key poses'))
        run_exe(['--render-interaction', profile['sequence'], scenario],
                watch=(profile['sequence'], count, 'scene frames'))
        contracts = check_scene(profile['sequence'] / 'scene.json', scenario)
        compose_sequence(profile)
        if profile['manifest']:
            compose_keyframes(profile)
        compose_desktop_scale(profile)
        report = validate(profile)
        report['capture_scope'] = scope
        report['scene_contracts'] = contracts
        profile['output'].mkdir(parents=True, exist_ok=True)
        (profile['output'] / 'scene-check.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
        shutil.copyfile(profile['sequence'] / 'scene.json', profile['output'] / 'scene.json')
        if grass:
            shutil.copyfile(profile['sequence'] / 'tip-region-diagnostic.png',
                            profile['output'] / 'tip-region-diagnostic.png')
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
