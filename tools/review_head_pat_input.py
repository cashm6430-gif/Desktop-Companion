"""Replay the approved head pat through the real Qt window and Native renderer.

    python tools/review_head_pat_input.py .local/authoring/head-pat-input-new-run
    python tools/review_head_pat_input.py <existing-run> --verify-only

Input is synthetic Qt input in an offscreen review process. No hooks, desktop
file operations, physical mouse input or installed settings are changed.
"""
import argparse
import json
from pathlib import Path

from review_motion import ROOT, EXE, run_exe
from review_session import deployment_snapshot, digest

SCENARIOS = tuple('head-pat-input-' + name for name in (
    'left', 'right', 'busy-left', 'busy-right', 'ungrab', 'deactivate',
    'hide', 'delete', 'grass', 'wave', 'drag',
))
SCOPE = 'window_player_and_synthetic_qt_pointer_with_native_model'


def check_scene(path):
    data = json.loads(path.read_text(encoding='utf8'))
    scenario = data['scenario']
    assert scenario in SCENARIOS and data['scope'] == SCOPE
    assert data['passed'] and not data['failure'], (scenario, data['failure'])
    assert data['capture']['render_backend'] == 'cubism_native'
    assert not data['capture']['render_error']
    frames = data['frames']
    assert frames and len(frames) <= 120
    assert all(not f['pointer']['preview_enabled'] for f in frames)
    assert all(f['pointer']['head_pat_approved'] for f in frames)
    assert all(value is True for key, value in data['checks'].items()
               if key != 'press_head_hit'), data['checks']
    body_cancel = not scenario.endswith(('-left', '-right', '-wave', '-drag'))
    assert data['checks']['press_head_hit'] is (not body_cancel)
    pointers = [f['pointer'] for f in frames]
    if scenario.endswith(('-left', '-right')):
        active = [f for f in frames if f['pointer']['reaction'] == 'head-pat']
        assert active and active[0]['pointer']['gesture'] == 'pat'
        assert all(not f['pointer']['wave_active'] for f in frames)
        direction = -1 if scenario.endswith('-left') else 1
        assert max(direction * f['parameters']['ParamAngleZ'] for f in active) > 4.5
        assert not data['final_pointer']['reaction']
        if '-busy-' in scenario:
            assert data['checks']['busy_automatic_short_release']
            for f in frames:
                assert f['state'] == 1 and f['active_turns'] == 1
                if f['time'] < 2.2:
                    continue
                for axis in ('ParamSitPose', 'ParamDeskVisible', 'ParamLaptopVisible'):
                    assert f['parameters'][axis] > .98, (scenario, axis)
    elif scenario.endswith('-wave'):
        assert any(p['wave_active'] for p in pointers)
        assert not any(p['reaction'] == 'head-pat' for p in pointers)
    elif scenario.endswith('-drag'):
        assert any(p['dragging'] and p['drag_motion_active'] for p in pointers)
        assert not any(p['wave_active'] or p['reaction'] for p in pointers)
    else:
        cancelled = next(i for i, f in enumerate(frames)
                         if f['event'] == 'body_pending_cancel_then_return_idle')
        after = frames[cancelled:]
        assert after[-1]['wall_ms'] - after[0]['wall_ms'] >= 600
        assert all(f['pointer']['gesture'] == 'none'
                   and not f['pointer']['long_press_pending']
                   and not f['pointer']['wave_active'] for f in after)
    return {'scenario': scenario, 'frames': len(frames), 'passed': True,
            'checks': data['checks']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--scenario', choices=SCENARIOS, action='append',
                        help='Capture or verify selected scenarios; default: all eleven.')
    args = parser.parse_args()
    output = args.output.resolve()
    if not args.verify_only:
        if output.exists():
            parser.error('Use a new output directory to preserve earlier evidence.')
        inputs = deployment_snapshot()
        renderer = digest(EXE)
        core = digest(ROOT / 'build/Live2DCubismCore.dll')
        review_tool = digest(Path(__file__))
        output.mkdir(parents=True)
        (output / 'capture-start.json').write_text(json.dumps({
            'inputs': inputs, 'renderer_sha256': renderer, 'cubism_core_sha256': core,
            'review_tool_sha256': review_tool}, indent=2) + '\n', encoding='utf8')
    results = []
    for scenario in args.scenario or SCENARIOS:
        folder = output / scenario
        if not args.verify_only:
            folder.mkdir()
            run_exe(['--render-interaction', folder, scenario], watch=(folder, 120, 'frames (upper bound)'))
        results.append(check_scene(folder / 'scene.json'))
    report = {'scope': SCOPE, 'passed': True, 'scenarios': results,
              'physical_windows_pointer_routing': 'not_exercised'}
    if not args.verify_only:
        assert deployment_snapshot() == inputs, 'Assets changed during input capture; run is stale.'
        assert digest(EXE) == renderer and digest(ROOT / 'build/Live2DCubismCore.dll') == core, \
            'Renderer/Core changed during input capture; run is stale.'
        assert digest(Path(__file__)) == review_tool, 'Review tool changed during capture; run is stale.'
        report['executable_sha256'] = renderer
        report['provenance_checks'] = 'source/deployment, renderer, Core and tool unchanged'
        (output / 'input-check.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
