"""Render the pending desk peek on R19 in isolated stand/laptop scenes.

Usage: python tools/review_desk_peek.py <fresh-build-directory>
The preview never opens hooks or adds an automatic motion to the live pet.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import subprocess

from PIL import Image, ImageDraw, ImageFont

from review_session import BUILD, ROOT, deployment_snapshot, digest

CLIP = ROOT / 'art/live2d/workflow/desk-peek-20261109/desk-peek.motion.json'
EXE = BUILD / 'DesktopCompanion.exe'
SCENES = ('standing-right', 'standing-left', 'laptop-right', 'laptop-left',
          'standing-drag', 'laptop-delete')
LEAN = {'ParamBodyAngleX', 'ParamBodyAngleZ'}
FACE = {'ParamAngleX', 'ParamAngleY', 'ParamAngleZ', 'ParamEyeLOpen', 'ParamEyeROpen',
        'ParamEyeBallX', 'ParamEyeBallY', 'ParamEyeSmile', 'ParamMouthForm', 'ParamCheek'}


def require(condition, error):
    if not condition:
        raise ValueError(error)


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def source_hashes():
    return {path.relative_to(ROOT).as_posix(): digest(path) for path in
            (CLIP, EXE, BUILD / 'Live2DCubismCore.dll', ROOT / 'src/DeskPeekReview.cpp',
             ROOT / 'src/main.cpp', Path(__file__).resolve())}


def capture(output, scene):
    run = subprocess.run([str(EXE), '--render-interaction', str(output / scene),
                          'desk-peek-' + scene, str(CLIP)], cwd=ROOT, capture_output=True, timeout=60)
    (output / f'{scene}.log').write_bytes(run.stdout + run.stderr)
    require(run.returncode == 0, f'{scene}: Native capture exited {run.returncode}')


def verify(folder, scene, clip):
    path = folder / 'scene.json'
    data = read_json(path)
    require(data['scenario'] == 'desk-peek-' + scene and data['isolated_settings'], 'Unexpected capture')
    require(data['scope'] == 'candidate_layer_on_real_motion_player_native_render', 'Unexpected scope')
    require(data['approval'] == clip['approval'] == 'pending' and data['revision'] == clip['revision'],
            'Preview approval metadata mismatch')
    require(data['render_backend'] == 'cubism_native' and not data['render_error'], 'Native model unavailable')
    frames, start, step = data['frames'], data['start_frame'], data['step']
    expected_frames = start + math.ceil((clip['duration'] + .65) / step)
    require(step == 1 / 15 and (len(frames) == expected_frames if scene != 'laptop-delete'
                               else len(frames) > expected_frames), 'Unexpected review clock or frame count')
    laptop = scene.startswith('laptop')
    can_write = FACE if laptop else FACE | LEAN
    marks = {event['name']: start + round(event['time'] / step) for event in clip['events']}
    cuts = [(i, f['event']) for i, f in enumerate(frames) if f['event'] in ('drag_begin', 'delete_begin')]
    expected = {'standing-drag': 'drag_begin', 'laptop-delete': 'delete_begin'}.get(scene)
    require([name for _, name in cuts] == ([expected] if expected else []), 'Missing or unexpected interruption')
    bounds = []
    for i, frame in enumerate(frames):
        applied, base = frame['applied'], frame['base']
        require(abs(frame['time'] - i * step) < 1e-8 and frame['file'] == f'frame-{i:03}.png',
                'Missing or misordered frame')
        require(applied.keys() == base.keys() and all(map(math.isfinite, (*applied.values(), *base.values()))),
                'Missing or non-finite parameter')
        changed = {axis for axis in base if base[axis] != applied[axis]}
        require(changed <= can_write, f'{scene} frame {i}: changed protected axes {changed - can_write}')
        require(not changed or start <= i and frame['weight'] > 0, 'Preview moved outside its window')
        with Image.open(folder / frame['file']) as image:
            alpha = image.getchannel('A')
            box = alpha.point(lambda a: 255 if a >= 128 else 0).getbbox()
            require(box and box[2] - box[0] > image.width * .5 and box[3] - box[1] > image.height * .55,
                    f'{scene} frame {i}: missing character silhouette')
            bounds.append(box)
            if not cuts or i < cuts[0][0]:
                require(0 < box[0] and box[2] < image.width and 0 < box[1] and box[3] < image.height,
                        f'{scene} frame {i}: character clipped at canvas border')
        if laptop and (not cuts or i < cuts[0][0]):
            require(frame['desk_phase'] == 'work' and applied['ParamSitPose'] > .99
                    and applied['ParamLaptopVisible'] > .99 and applied['ParamDeskVisible'] > .99,
                    f'{scene} frame {i}: stable work pose lost')
    if cuts:
        cut = cuts[0][0]
        require(frames[cut - 1]['weight'] == 1 and 0 < frames[cut]['weight'] < 1
                and all(f['weight'] == 0 for f in frames[cut + 1:]), 'Peek did not yield or replayed')
        require(any(f['event'] == ('resume_busy' if laptop else 'drag_end') for f in frames[cut + 1:]),
                f'{scene}: did not reach foreground recovery')
    else:
        sign = -1 if scene.endswith('left') else 1

        def delta(mark, axis):
            pose = frames[marks[mark]]
            return pose['applied'][axis] - pose['base'][axis]

        require(sign * delta('eyes_lead', 'ParamEyeBallX') > .55 and delta('eyes_lead', 'ParamAngleX') == 0,
                'Eyes did not lead the head')
        require(sign * delta('lean_peak', 'ParamAngleX') > 5 and sign * delta('lean_peak', 'ParamAngleZ') > 3
                and (laptop or sign * delta('lean_peak', 'ParamBodyAngleZ') > 2), 'Head/shoulder beat missing')
        require(all(frames[marks['blink']]['applied'][axis] < .08 for axis in ('ParamEyeLOpen', 'ParamEyeROpen')),
                'Authored blink did not close')
        require(abs(delta('eyes_home', 'ParamEyeBallX')) < .02 and abs(delta('eyes_home', 'ParamAngleX')) > 2,
                'Eyes and head did not settle separately')
        require(all(f['applied'] == f['base'] for f in frames[marks['settled'] + 1:]), 'Candidate did not return to base')
    return {'scene': scene, 'frames': len(frames), 'key_frames': marks, 'scene_sha256': digest(path),
            'interrupt_frame': cuts[0][0] if cuts else None,
            'opaque_bounds': [min(b[0] for b in bounds), min(b[1] for b in bounds),
                              max(b[2] for b in bounds), max(b[3] for b in bounds)]}


def paste_frame(canvas, output, scene, index, x, y, size):
    with Image.open(output / scene / f'frame-{index:03}.png') as source:
        frame = source.convert('RGBA').resize((size, size), Image.Resampling.LANCZOS)
    canvas.paste(frame, (x, y), frame)


def compose(output, results):
    font_path = 'C:/Windows/Fonts/msyh.ttc'
    title = ImageFont.truetype(font_path, 18)
    tag = ImageFont.truetype(font_path, 14)
    reports = {row['scene']: row for row in results}
    marks = reports['standing-right']['key_frames']
    scenes = ('standing-right', 'laptop-right')
    previews = []
    for i in range(reports['standing-right']['frames']):
        slide = Image.new('RGB', (600, 338), '#eaf2fa')
        draw = ImageDraw.Draw(slide)
        for col, (scene, caption, bg, fg) in enumerate(zip(scenes, ('站姿', '抱电脑'),
                                                         ('#eaf2fa', '#233249'), ('#243b55', '#e9f2fb'))):
            x = col * 300
            draw.rectangle((x, 0, x + 299, 337), fill=bg)
            paste_frame(slide, output, scene, i, x + 10, 40, 280)
            draw.text((x + 14, 10), caption, font=title, fill=fg)
            draw.text((x + 224, 13), '看向右侧', font=tag, fill=fg)
        previews.append(slide)
    durations = [70 if i % 3 != 2 else 60 for i in range(len(previews))]
    previews[0].save(output / 'desk-peek.gif', save_all=True, append_images=previews[1:],
                     duration=durations, loop=0, disposal=2)

    captions = ('原姿态', '眼先看', '探头', '慢眨', '收回')
    indices = (1, marks['eyes_lead'], marks['lean_peak'], marks['blink'], marks['settled'] + 2)
    board = Image.new('RGB', (1280, 572), '#eaf2fa')
    pen = ImageDraw.Draw(board)
    for row, (scene, label, bg, fg) in enumerate(zip(('standing-right', 'laptop-left'), ('站姿向右', '抱电脑向左'),
                                                     ('#eaf2fa', '#233249'), ('#243b55', '#e9f2fb'))):
        y = row * 286
        pen.rectangle((0, y, 1279, y + 285), fill=bg)
        for col, (caption, index) in enumerate(zip(captions, indices)):
            x = col * 256
            paste_frame(board, output, scene, index, x + 8, y + 39, 240)
            pen.text((x + 12, y + 11), f'{label} · {caption}' if col == 0 else caption, font=title, fill=fg)
    board.save(output / 'key-poses.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    clip = read_json(CLIP)
    require(clip['tracks'].keys() == FACE | LEAN, 'Unexpected authored channels')
    inputs, assets = source_hashes(), deployment_snapshot()
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(lambda scene: capture(output, scene), SCENES))
    require(source_hashes() == inputs and deployment_snapshot() == assets, 'Review inputs changed during capture')
    results = [verify(output / scene, scene, clip) for scene in SCENES]
    compose(output, results)
    report = {'passed': True, 'approval': 'pending', 'revision': clip['revision'], 'runtime_entry': False,
              'scope': 'R19 candidate parameters over the real background player; simulated drag/delete; no hooks',
              'frames': sum(row['frames'] for row in results), 'scenes': results, 'source_sha256': inputs,
              'deployed_assets': assets, 'artifacts': {name: digest(output / name)
                                                       for name in ('desk-peek.gif', 'key-poses.png')}}
    (output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{len(results)} scenes, {report['frames']} Native frames passed: {output}")


if __name__ == '__main__':
    main()
