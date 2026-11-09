"""Run the PetWindow edge preview with synthetic input and an isolated profile."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

from PIL import Image, ImageDraw, ImageFont


SIDES = (('left', '左'), ('right', '右'), ('top', '上'), ('bottom', '下'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--executable', type=Path, default=Path('build/DesktopCompanion.exe'))
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    args.capture_root.mkdir(parents=True, exist_ok=False)
    args.output.mkdir(parents=True, exist_ok=False)
    scenes = {}
    for side, _ in SIDES:
        print(f'Capturing {side}...', flush=True)
        capture = (args.capture_root / side).resolve()
        scenario = f'edge-dock-window-{side}'
        result = subprocess.run([str(executable), '--render-interaction', str(capture), scenario],
                                capture_output=True, timeout=65)
        receipt = capture / 'scene.json'
        scene = json.loads(receipt.read_text(encoding='utf-8')) if receipt.exists() else {}
        if (result.returncode or not scene.get('passed') or scene.get('scenario') != scenario
                or scene.get('render_backend') != 'cubism_native'
                or len(scene.get('frames', [])) != 35 or not all(scene.get('checks', {}).values())):
            raise RuntimeError(f'{side}: exit {result.returncode}, {scene.get("failure")}; '
                               f'{result.stderr[-1000:].decode(errors="replace")}')
        scenes[side] = scene

    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 18)
    preview_frames = []
    for index in range(35):
        sheet = Image.new('RGB', (916, 776), '#f3f6fb')
        draw = ImageDraw.Draw(sheet)
        draw.text((9, 5), '桌宠窗口 · 四边半隐藏（虚拟屏幕、合成鼠标）', font=font, fill='#263443')
        for column, (side, label) in enumerate(SIDES):
            scene = scenes[side]
            row = scene['frames'][index]
            sx, sy, sw, sh = scene['screen']
            screen = Image.new('RGBA', (sw, sh), '#dce7f2')
            with Image.open(args.capture_root / side / row['image']) as native:
                screen.alpha_composite(native.convert('RGBA'),
                                       (row['window_x'] - sx, row['window_y'] - sy))
            x, y = 5 + (column % 2) * 456, 29 + (column // 2) * 373
            draw.text((x + 3, y), label, font=font, fill='#263443')
            sheet.paste(screen.resize((450, 350), Image.Resampling.LANCZOS).convert('RGB'), (x, y + 23))
        preview_frames.append(sheet)
    preview = args.output / 'edge-dock-window.gif'
    durations = [50] * len(preview_frames)
    durations[0], durations[22], durations[-1] = 300, 350, 450
    preview_frames[0].save(preview, save_all=True, append_images=preview_frames[1:],
                           duration=durations, loop=0, optimize=True)
    detail = args.output / 'edge-dock-heads.png'
    preview_frames[22].save(detail)

    traces = args.output / 'scene-traces.zip'
    with ZipFile(traces, 'w', ZIP_DEFLATED) as archive:
        for side, _ in SIDES:
            archive.write(args.capture_root / side / 'scene.json', f'{side}/scene.json')
    model = Path('assets/live2d/whale-girl')
    source = Path(__file__).resolve().parents[1] / model
    deployed = executable.parent / model
    names = [p.relative_to(source) for p in source.rglob('*') if p.is_file() and
             (p.name.endswith(('.moc3', '.model3.json', '.physics3.json')) or p.name.startswith('texture_'))]
    model_hashes = {path.as_posix(): digest(deployed / path) for path in sorted(names)}
    if not model_hashes or any(digest(source / path) != sha for path, sha in model_hashes.items()):
        raise ValueError('Deployed model differs from repository assets')
    report = {
        'scope': 'real PetWindow and Cubism; virtual monitor and synthetic events',
        'entry_motion': 'previous continuous four-edge candidate (on hold)',
        'default_enabled': False,
        'normal_entry_available': False,
        'scenarios': {side: {'checks': len(scene['checks']), 'frames': len(scene['frames'])}
                      for side, scene in scenes.items()},
        'limitations': ['no physical mouse or mixed-DPI display', 'events injected directly into the controller'],
        'inputs': {'executable_sha256': digest(executable), 'model_and_texture_sha256': model_hashes},
        'outputs': {p.name: digest(p) for p in (preview, detail, traces)},
    }
    receipt = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    (args.output / 'verification.json').write_text(receipt, encoding='utf-8')
    print(receipt)


if __name__ == '__main__':
    main()
