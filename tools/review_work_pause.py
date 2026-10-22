"""Capture the real standing/seated work-pause player and interruption branches.

    python tools/review_work_pause.py .local/authoring/work-pause-new
    python tools/review_work_pause.py <existing-run> --verify-only

Offscreen Native rendering and direct player events; no physical input, hooks,
desktop deletion, installation or user settings are exercised. New standing
choreography remains pending. Reuse a fresh directory for every capture.
"""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from review_motion import EXE, ROOT, run_exe
from review_session import deployment_snapshot, digest

SCENARIOS = tuple('stretch-' + name for name in (
    'standing', 'laptop', 'stop', 'delete', 'drag', 'pat', 'preview-off'))
SCOPE = 'real_interaction_player_and_native_model'
POSTURE = ('ParamBusyLaptop', 'ParamSitPose', 'ParamDeskVisible', 'ParamLaptopVisible')


def read(path):
    return json.loads(path.read_text(encoding='utf8'))


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def check_scene(path, approvals):
    data = read(path)
    name = data['scenario']
    assert name in SCENARIOS and data['scope'] == SCOPE
    assert data['capture']['render_backend'] == 'cubism_native'
    assert not data['capture']['render_error']
    frames = data['frames']
    assert len(frames) == 75
    seated = name == 'stretch-laptop'
    clip = 'busy-stretch' if seated else 'busy-rest'
    approved = approvals[clip] == 'approved'
    active = [f for f in frames if f['interaction'] == clip]
    assert active, f'{name}: pause never started'
    assert all(f['default_pause_accepted'] is approved for f in frames if f['time'] >= .4)
    assert max(f['busy_phase'] for f in active) - min(f['busy_phase'] for f in active) < 1e-8
    assert len({f['busy_choice_at'] for f in active}) == 1
    assert max(f['continuous_busy'] for f in active) - min(f['continuous_busy'] for f in active) < 1e-8
    for f in frames:
        assert all(abs(f['parameters'][k] - int(seated)) < .02 for k in POSTURE), (name, f['time'])
    if not seated:
        assert all(abs(f['parameters'][k]) < .01 for f in active
                   for k in ('ParamBusyTypingL', 'ParamBusyTypingR'))
        # Standing work already carries a mirrored +/-7 arm accent. Rest must
        # preserve that background, not snap the arms to a new zero pose.
        assert all(abs(f['parameters']['ParamArmLA'] + f['parameters']['ParamArmRA']) < .01
                   and abs(f['parameters']['ParamArmLA']) <= 7.01 for f in active)
        assert all(f['thought_bubble'] < .02 for f in active if f['interaction_time'] > .35)
    expected_interrupt = name not in ('stretch-standing', 'stretch-laptop')
    if name == 'stretch-preview-off' and approved:
        expected_interrupt = False
        assert any(f['event'] == 'preview_disabled_approved_retained' and f['interaction'] == clip
                   for f in frames), 'Disabling drafts cancelled an approved pause'
    if expected_interrupt:
        event = next(f for f in frames if f['time'] >= 1.2 and f['event'])
        assert all(f['interaction'] != clip for f in frames if f['time'] >= event['time']), name
        assert frames[-1]['state'] == (0 if name == 'stretch-stop' else 1)
    else:
        assert not frames[-1]['interaction']
        assert frames[-1]['busy_phase'] > active[-1]['busy_phase'] + .25
        assert frames[-1]['state'] == 1
    return {'scenario': name, 'frames': len(frames), 'clip': clip, 'passed': True,
            'default_entry': 'approved_accepted' if approved else 'pending_refused',
            'pose_and_props_preserved': True, 'busy_choice_clock_frozen': True,
            'interruption_or_release': True, 'physical_mouse': 'not_exercised',
            'visual_approval': approvals[clip]}


def compose(output):
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 16)
    clip = read(ROOT/'assets/motions/busy-rest.motion.json')
    label = '已批准' if clip['approval'] == 'approved' else '待审批'
    folder = output / 'stretch-standing'
    backgrounds = ('#e9e9ef', '#252b36')
    gif = []
    for i in range(75):
        frame = Image.open(folder / f'frame-{i:03d}.png').convert('RGBA')
        frame.thumbnail((280, 280), Image.Resampling.LANCZOS)
        card = Image.new('RGB', (560, 312))
        for col, background in enumerate(backgrounds):
            tile = Image.new('RGB', (280, 312), background)
            tile.paste(frame, ((280-frame.width)//2, 30), frame)
            ImageDraw.Draw(tile).text((8, 5), f'站姿歇一下 · {label}  {i/15:.2f}s',
                                     fill='#7999ba', font=font)
            card.paste(tile, (col*280, 0))
        gif.append(card)
    gif[0].save(output/'standing-rest-preview.gif', save_all=True, append_images=gif[1:],
                duration=67, loop=0, disposal=2)
    sheet = Image.new('RGB', (len(clip['keyframes'])*280, 2*320))
    for row, background in enumerate(backgrounds):
        for i, key in enumerate(clip['keyframes']):
            frame = Image.open(output/'poses'/f'rest-{i:02d}.png').convert('RGBA')
            frame.thumbnail((280, 280), Image.Resampling.LANCZOS)
            tile = Image.new('RGB', (280, 320), background)
            tile.paste(frame, ((280-frame.width)//2, 38), frame)
            ImageDraw.Draw(tile).text((5, 6), f"{key['time']:.2f}s {key['label']}",
                                     fill='#7999ba', font=font)
            sheet.paste(tile, (i*280, row*320))
    sheet.save(output/'standing-rest-keyposes.png')
    picks = (0, 11, 18, 22, 29, 35, 44, 60, 74)
    sheet = Image.new('RGB', (9*280, len(SCENARIOS)*320), '#e9e9ef')
    for row, name in enumerate(SCENARIOS):
        data = read(output/name/'scene.json')
        for col, i in enumerate(picks):
            frame = Image.open(output/name/f'frame-{i:03d}.png').convert('RGBA')
            frame.thumbnail((280, 280), Image.Resampling.LANCZOS)
            x, y = col*280, row*320
            sheet.paste(frame, (x+(280-frame.width)//2, y+38), frame)
            ImageDraw.Draw(sheet).text((x+5,y+5),f'{name} {data["frames"][i]["time"]:.2f}s',
                                       fill='#1c344e',font=font)
    sheet.save(output/'interruption-samples.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    if not args.verify_only:
        if output.exists(): parser.error('Use a fresh output directory.')
        snapshot = deployment_snapshot()
        approvals = {name:read(ROOT/f'assets/motions/{name}.motion.json')['approval']
                     for name in ('busy-rest','busy-stretch')}
        output.mkdir(parents=True)
        receipt = {'inputs':snapshot,'renderer_sha256':digest(EXE),
                   'core_sha256':digest(ROOT/'build/Live2DCubismCore.dll'),
                   'review_tool_sha256':digest(Path(__file__)), 'scope':SCOPE,
                   'fixed_step_seconds':1/15, 'visual_approval':approvals['busy-rest'],
                   'source_hashes':{name:digest(ROOT/name) for name in (
                       'src/main.cpp','src/ParameterMotion.h','src/ParameterMotion.cpp',
                       'src/PetWindow.cpp')},
                   'authored_approvals':approvals}
        write(output/'capture-start.json', receipt)
        run_exe(['--review-motion',output/'poses',ROOT/'assets/motions/busy-rest.motion.json','rest'],
                watch=(output/'poses',len(read(ROOT/'assets/motions/busy-rest.motion.json')['keyframes']),
                       'standing key poses'))
        for name in SCENARIOS:
            run_exe(['--render-interaction',output/name,name],watch=(output/name,75,name))
            print(check_scene(output/name/'scene.json',receipt['authored_approvals']),flush=True)
        assert snapshot == deployment_snapshot(), 'Assets changed during capture'
        assert receipt['renderer_sha256'] == digest(EXE), 'Renderer changed during capture'
        assert receipt['core_sha256'] == digest(ROOT/'build/Live2DCubismCore.dll')
        assert receipt['review_tool_sha256'] == digest(Path(__file__))
        assert all(value == digest(ROOT/name) for name,value in receipt['source_hashes'].items())
        compose(output)
    approvals = read(output/'capture-start.json')['authored_approvals']
    checks = [check_scene(output/name/'scene.json',approvals) for name in SCENARIOS]
    write(output/'scene-check.json', {'passed':True,'scenarios':checks,
                                     'live_provenance_checked':not args.verify_only,
                                     'visual_approval':approvals['busy-rest']})
    print(f'{len(checks)}/{len(SCENARIOS)} player scenarios passed; standing approval: {approvals["busy-rest"]}',flush=True)


if __name__ == '__main__':
    main()
