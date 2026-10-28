"""Validate and compose the isolated live-Cubism Qt edge review captures.

Input directories must be produced by `--render-interaction ... edge-hide-*`.
This does not run or validate the ordinary desktop entry or real hooks.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

from PIL import Image, ImageDraw, ImageFont


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--run', action='store_true', help='run four isolated Native scenarios first')
    parser.add_argument('--executable', type=Path, default=Path('build/DesktopCompanion.exe'))
    args = parser.parse_args()
    # Never overwrite earlier approval evidence. A fresh capture and output
    # directory are both required for a new run.
    args.output.mkdir(parents=True, exist_ok=False)
    if args.run:
        args.capture_root.mkdir(parents=True, exist_ok=False)
        exe = args.executable.resolve(strict=True)
        for side in ('left', 'right', 'top', 'bottom'):
            print(f'Capturing {side}...', flush=True)
            result = subprocess.run([str(exe), '--render-interaction',
                                     str((args.capture_root / side).resolve()), 'edge-hide-' + side],
                                    capture_output=True, text=True, timeout=110)
            if result.returncode:
                raise RuntimeError(f'{side} failed ({result.returncode}): {result.stderr[-2000:]}')
    sides = [('left', '左'), ('right', '右'), ('top', '上'), ('bottom', '下')]
    required = {'native_and_isolated', 'normal_head_press_accepted', 'edge_drop_enters',
                'preview_gate_entered', 'cursor_event_transforms_to_model',
                'busy_and_permission_bookkeeping', 'no_automatic_cue_or_delete',
                'memo_content_retained', 'empty_area_not_grabbable',
                'gaze_reaches_native_model', 'short_pull_returns_to_hidden', 'lost_capture_restores_hidden', 'head_press_accepted',
                'drag_keeps_grabbed_head_under_pointer', 'drag_restored',
                'preview_gate_left', 'no_old_event_replayed', 'inverse_gaze_transform', 'native_frame'}
    scenes = {}
    for side, _ in sides:
        folder = args.capture_root / side
        data = json.loads((folder / 'scene.json').read_text(encoding='utf-8'))
        if (not data.get('passed') or data.get('scenario') != 'edge-hide-' + side
                or data.get('scope') != 'isolated_qt_virtual_screen_live_cubism_synthetic_pointer'
                or data.get('render_backend') != 'cubism_native' or data.get('frame_count') != 75
                or not all(data.get('checks', {}).get(k) is True for k in required)):
            raise ValueError(f'Failed or incomplete capture: {side}')
        trace = data.get('frames', [])
        if len(trace) != 75 or any(row.get('index') != i or row.get('file') != f'frame-{i:03}.png'
                                   for i, row in enumerate(trace)):
            raise ValueError(f'Incomplete or reordered frame trace: {side}')
        if not all((folder / f'frame-{i:03}.png').is_file() for i in range(75)):
            raise ValueError(f'Missing captured frames: {side}')
        scenes[side] = data
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 17)
    frames = []
    for i in range(75):
        sheet = Image.new('RGB', (560, 620), '#f7f9fb')
        draw = ImageDraw.Draw(sheet)
        draw.text((10, 4), '实时模型 + Qt 虚拟屏幕合成输入；未接正常入口', font=font, fill='#253543')
        for j, (side, name) in enumerate(sides):
            x, y = (j % 2) * 280, 32 + (j // 2) * 292
            frame = Image.open(args.capture_root / side / f'frame-{i:03}.png').convert('RGB')
            frame = frame.resize((270, 270), Image.Resampling.LANCZOS)
            sheet.paste(frame, (x + 5, y + 20))
            draw.text((x + 8, y), name, font=font, fill='#253543')
        frames.append(sheet)
    preview = args.output / 'edge-hide-qt-preview.gif'
    frames[0].save(preview, save_all=True, append_images=frames[1:], duration=50, loop=0, optimize=True)
    # Native-size close-ups of the hidden head (one fixed time, not a claim
    # that every frame has been visually reviewed).
    detail = Image.new('RGB', (840, 900), '#f7f9fb')
    draw = ImageDraw.Draw(detail)
    draw.text((15, 8), '半隐藏头部细节：真实模型第 40 帧', font=font, fill='#253543')
    crops = [(0, 180, 200, 380), (360, 180, 560, 380),
             (180, 0, 380, 200), (180, 360, 380, 560)]
    for i, (side, name) in enumerate(sides):
        x, y = 10 + (i % 2) * 420, 35 + (i // 2) * 430
        frame = Image.open(args.capture_root / side / 'frame-040.png').convert('RGB')
        detail.paste(frame.crop(crops[i]).resize((400, 400), Image.Resampling.LANCZOS), (x, y + 22))
        draw.text((x, y), name, font=font, fill='#253543')
    detail_path = args.output / 'edge-hide-head-detail.png'
    detail.save(detail_path)
    with ZipFile(args.output / 'scene-traces.zip', 'w', ZIP_DEFLATED) as archive:
        for side, _ in sides:
            archive.write(args.capture_root / side / 'scene.json', side + '/scene.json')
    check = {
        'scope': 'isolated Qt virtual screen, real live Cubism, synthetic mouse and fixture events',
        'approval': 'pending',
        'scenarios': {s: {'checks': len(scenes[s]['checks']), 'frames': scenes[s]['frame_count']} for s, _ in sides},
        'preview_sha256': digest(preview),
        'head_detail_sha256': digest(detail_path),
        'scene_traces_sha256': digest(args.output / 'scene-traces.zip'),
        'limitations': ['ordinary entry disabled', 'no physical mouse or real multi-monitor window',
                        'deletion gate is a fixture, not a production source',
                        'fixtures use direct controller events, not a real agent or hook',
                        'Cubism physics is evaluated before frame rotation; rotated gravity is unverified'],
    }
    if args.run:
        exe = args.executable.resolve(strict=True)
        model = exe.parent / 'assets/live2d/whale-girl'
        source = Path(__file__).resolve().parents[1] / 'assets/live2d/whale-girl'
        names = ['whale-girl-layered-draft.model3.json', 'whale-girl-layered-draft.moc3',
                 'whale-girl-layered-draft.physics3.json']
        names += [f'whale-girl-layered-draft.4096/texture_{i:02}.png' for i in range(5)]
        hashes = {}
        for name in names:
            actual, expected = digest(model / name), digest(source / name)
            if actual != expected:
                raise ValueError(f'Deployed model differs from source: {name}')
            hashes[name] = actual
        check['inputs'] = {'executable_sha256': digest(exe), 'model_and_texture_sha256': hashes}
    (args.output / 'verification.json').write_text(json.dumps(check, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(check, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
