"""Compose a four-edge *static concept* from a Native capture and its report.

This intentionally rotates the complete captured character, then clips at a
virtual screen boundary. It is not a runtime render or a motion preview.
"""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('capture', type=Path)
    parser.add_argument('report', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--sequence', type=Path, help='Native idle capture directory (optional)')
    args = parser.parse_args()
    report = json.loads(args.report.read_text(encoding='utf-8'))
    if report.get('render_backend') != 'cubism_native' or not report.get('frame_saved'):
        raise ValueError('A successful Native render capture is required')
    source = Image.open(args.capture).convert('RGBA')
    if source.size != (840, 840):
        raise ValueError(f'Expected the 840 x 840 Native capture, got {source.size}')
    args.output.mkdir(parents=True, exist_ok=True)
    size, edge, scale = 420, 330, 0.5
    body = source.resize((420, 420), Image.Resampling.LANCZOS)
    # The line is just under the chin in this pose; long hair and body continue
    # behind it. This is a visual exploration, not a measured head boundary.
    chin_line = 390 * scale
    base = Image.new('RGBA', (size, size), '#eaf0f5')
    layer = Image.new('RGBA', (size, size))
    layer.alpha_composite(body, (0, round(edge - chin_line)))
    layer.paste((0, 0, 0, 0), (0, edge, size, size))
    base.alpha_composite(layer)
    draw = ImageDraw.Draw(base)
    draw.rectangle((0, edge, size, size), fill='#3b4858')
    draw.line((0, edge, size, edge), fill='#aebccd', width=3)
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 21)
    small = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 16)
    title = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 26)
    sheet = Image.new('RGB', (900, 1010), '#f7f9fb')
    d = ImageDraw.Draw(sheet)
    d.text((30, 14), '屏幕边缘露头：静态构图试验', font=title, fill='#243343')
    d.text((30, 53), '整帧旋转 + 边缘裁切；仅供观察，不是运行效果或动作审批', font=small, fill='#536476')
    cases = [('左边 · 侧身', 270), ('右边 · 侧身', 90), ('上边 · 倒挂', 180), ('下边 · 直立', 0)]
    for i, (label, angle) in enumerate(cases):
        x, y = 20 + (i % 2) * 450, 90 + (i // 2) * 450
        panel = base.rotate(angle, resample=Image.Resampling.BICUBIC)
        sheet.paste(panel.convert('RGB'), (x, y + 25))
        d.text((x + 3, y), label, font=font, fill='#243343')
    result = args.output / 'four-edge-static-concept.png'
    sheet.save(result)
    manifest = {
        'scope': 'static_composite_of_single_native_capture_not_runtime_or_motion',
        'capture_sha256': sha(args.capture),
        'capture_report_sha256': sha(args.report),
        'capture_report': report,
        'image_sha256': sha(result),
        'screen_boundary_source_y_px': 390,
        'assumptions': ['entire rendered frame rotated for side and top concepts',
                        'no hit testing, animation, cursor or multi-monitor behavior tested'],
    }
    (args.output / 'concept.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.sequence:
        sequence_report = json.loads((args.sequence / 'capture.json').read_text(encoding='utf-8'))
        if (sequence_report.get('render_backend') != 'cubism_native'
                or sequence_report.get('scope') != 'sampled_motion_player_native_capture'
                or sequence_report.get('clip') != 'idle'
                or not sequence_report.get('physics_evaluated')):
            raise ValueError('A Native idle sequence with physics is required')
        count = min(75, len(sequence_report.get('frames', [])))
        if count < 75:
            raise ValueError('Expected at least 75 idle frames')
        images, hashes = [], []
        size, edge, scale = 300, 235, 300 / 840
        smallfont = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 16)
        for index in range(count):
            path = args.sequence / f'frame-{index:03}.png'
            frame = Image.open(path).convert('RGBA')
            if frame.size != (840, 840):
                raise ValueError(f'Unexpected frame dimensions: {path}')
            hashes.append(sha(path))
            body = frame.resize((size, size), Image.Resampling.LANCZOS)
            base = Image.new('RGBA', (size, size), '#eaf0f5')
            layer = Image.new('RGBA', (size, size))
            layer.alpha_composite(body, (0, round(edge - 390 * scale)))
            layer.paste((0, 0, 0, 0), (0, edge, size, size))
            base.alpha_composite(layer)
            draw = ImageDraw.Draw(base)
            draw.rectangle((0, edge, size, size), fill='#3b4858')
            draw.line((0, edge, size, edge), fill='#aebccd', width=2)
            sheet = Image.new('RGB', (640, 700), '#f7f9fb')
            draw = ImageDraw.Draw(sheet)
            draw.text((20, 9), '真实模型待机帧 + 固定边缘裁切（不是进出动作）', font=smallfont, fill='#243343')
            for i, (label, angle) in enumerate(cases):
                x, y = 10 + (i % 2) * 320, 40 + (i // 2) * 325
                sheet.paste(base.rotate(angle, resample=Image.Resampling.BICUBIC).convert('RGB'), (x, y + 22))
                draw.text((x, y), label, font=smallfont, fill='#243343')
            images.append(sheet)
        animated = args.output / 'four-edge-idle-concept.gif'
        images[0].save(animated, save_all=True, append_images=images[1:], duration=67, loop=0, optimize=True)
        # The side/top character first slips behind the boundary, turns while
        # fully hidden, and peeks back in. This is still a composite experiment,
        # not an actual window gesture or authored per-part Cubism motion.
        transitions = []
        tile, edge, sprite = 300, 235, 156
        boundary = tile - edge
        centre = (tile - sprite) / 2
        target_bottom = edge - sprite * 390 / 840
        target_outer = tile - target_bottom - sprite
        positions = [((boundary, centre), (target_outer, centre)),
                     ((edge - sprite, centre), (target_bottom, centre)),
                     ((centre, boundary), (centre, target_outer)),
                     ((centre, edge - sprite), (centre, target_bottom))]
        hidden = [(boundary - sprite, centre), (edge, centre),
                  (centre, boundary - sprite), (centre, target_bottom)]
        angles = [-90, 90, 180, 0]
        def eased(t):
            t = max(0.0, min(1.0, t))
            return t * t * (3 - 2 * t)
        def lerp(a, b, t):
            return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
        for index in range(count):
            path = args.sequence / f'frame-{index:03}.png'
            body = Image.open(path).convert('RGBA').resize((sprite, sprite), Image.Resampling.LANCZOS)
            sheet = Image.new('RGB', (640, 700), '#f7f9fb')
            draw = ImageDraw.Draw(sheet)
            draw.text((20, 9), '真实待机帧 + 整体旋转位移试验（非实际交互）', font=smallfont, fill='#243343')
            for i, (label, _) in enumerate(cases):
                x, y = 10 + (i % 2) * 320, 40 + (i // 2) * 325
                panel = Image.new('RGBA', (tile, tile), '#eaf0f5')
                layer = Image.new('RGBA', (tile, tile))
                start, end = positions[i]
                if i == 3:
                    phase = eased(index / 26) if index < 27 else (1.0 if index < 48 else eased((74 - index) / 26))
                    position, angle = lerp(start, end, phase), 0
                elif index <= 11:
                    position, angle = lerp(start, hidden[i], eased(index / 11)), 0
                elif index <= 14:
                    position, angle = hidden[i], angles[i] * eased((index - 11) / 3)
                elif index <= 26:
                    position, angle = lerp(hidden[i], end, eased((index - 14) / 12)), angles[i]
                elif index < 48:
                    position, angle = end, angles[i]
                elif index <= 59:
                    position, angle = lerp(end, hidden[i], eased((index - 47) / 12)), angles[i]
                elif index <= 62:
                    position, angle = hidden[i], angles[i] * (1 - eased((index - 59) / 3))
                else:
                    position, angle = lerp(hidden[i], start, eased((index - 62) / 12)), 0
                px, py = map(round, position)
                layer.alpha_composite(body.rotate(angle, resample=Image.Resampling.BICUBIC), (px, py))
                if i == 0: layer.paste((0, 0, 0, 0), (0, 0, boundary, tile))
                if i == 1: layer.paste((0, 0, 0, 0), (edge, 0, tile, tile))
                if i == 2: layer.paste((0, 0, 0, 0), (0, 0, tile, boundary))
                if i == 3: layer.paste((0, 0, 0, 0), (0, edge, tile, tile))
                panel.alpha_composite(layer)
                pd = ImageDraw.Draw(panel)
                if i == 0: pd.rectangle((0, 0, boundary, tile), fill='#3b4858')
                if i == 1: pd.rectangle((edge, 0, tile, tile), fill='#3b4858')
                if i == 2: pd.rectangle((0, 0, tile, boundary), fill='#3b4858')
                if i == 3: pd.rectangle((0, edge, tile, tile), fill='#3b4858')
                sheet.paste(panel.convert('RGB'), (x, y + 22))
                draw.text((x, y), label, font=smallfont, fill='#243343')
            transitions.append(sheet)
        transition = args.output / 'four-edge-transition-experiment.gif'
        transitions[0].save(transition, save_all=True, append_images=transitions[1:], duration=67, loop=0, optimize=True)
        manifest['idle_sequence'] = {
            'scope': 'native_idle_frames_with_simulated_fixed_rotation_and_clipping',
            'frame_count': count, 'fixed_step_seconds': sequence_report['fixed_step_seconds'],
            'capture_report_sha256': sha(args.sequence / 'capture.json'),
            'native_frame_sha256': hashes, 'gif_sha256': sha(animated),
            'transition_gif_sha256': sha(transition),
            'limitations': ['not a transition, runtime window, input or gaze test',
                            'only selected five seconds of an eight-second idle sequence',
                            'transition is a software composite, not a real window or authored motion'],
        }
        (args.output / 'concept.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(result)


if __name__ == '__main__':
    main()
