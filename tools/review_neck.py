"""Capture isolated Native neck evidence; never modify or adopt model/motion assets.

    python tools/review_neck.py --model <candidate.model3.json>
    python tools/review_neck.py --raw-export --scenes none --no-motion-keys

Static poses freeze physics. Runtime scenes sample the existing player: exit is
75 frames; the seated cycle is 195 frames including entry, work and release.
Raw export is static only and deliberately bypasses runtime corrections.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
from uuid import uuid4

from PIL import Image, ImageDraw
from probe_cubism_core import model_family
from review_interactions import check_scene
from review_motion import (EXE, run_exe, compose_sequence, compose_desktop_scale,
                           font, border_max, floor_row)
from review_report import write_review_report
from review_session import ROOT, BUILD, deployment_snapshot, digest

FIXTURE = ROOT / 'art/live2d/workflow/neck-review-fixture.json'
DEFAULT_MODEL = BUILD / 'assets/live2d/whale-girl/whale-girl-layered-draft.model3.json'
MAX_POSES = 128
TOOLS = ('review_neck.py', 'review_motion.py', 'review_session.py',
         'review_report.py', 'review_interactions.py', 'probe_cubism_core.py')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf8'))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def finite_parameters(values):
    return isinstance(values, dict) and all(
        isinstance(key, str) and key and isinstance(value, (int, float))
        and not isinstance(value, bool) and math.isfinite(value) for key, value in values.items())


def pose_manifest(fixture, motion_directory=None):
    """Retain exact authored parameters and source identity; do not resample curves."""
    fixture = Path(fixture).resolve()
    document = read_json(fixture)
    crop = document.get('neck_crop_normalized')
    if (not isinstance(crop, list) or len(crop) != 4 or
            any(not isinstance(value, (int, float)) or not math.isfinite(value) for value in crop)
            or min(crop) < 0 or crop[2] <= 0 or crop[3] <= 0
            or crop[0] + crop[2] > 1 or crop[1] + crop[3] > 1):
        raise ValueError('Fixture needs a positive normalized crop within the frame.')
    frames = []
    sources = [fixture]
    for source in [fixture] + (sorted(Path(motion_directory).glob('*.motion.json'))
                              if motion_directory is not None else []):
        motion = document if source == fixture else read_json(source)
        if source != fixture:
            sources.append(source.resolve())
        keys = motion.get('keyframes', [])
        if not isinstance(keys, list):
            raise ValueError('keyframes must be an array: ' + str(source))
        for index, frame in enumerate(keys):
            if not isinstance(frame, dict) or not finite_parameters(frame.get('parameters')):
                raise ValueError('Invalid keyframe parameters: ' + str(source))
            row = dict(frame)
            row['name'] = (frame.get('name') or frame.get('label') or f'key-{index:02}')
            if source != fixture:
                row['name'] = source.name.removesuffix('.motion.json') + '/' + row['name']
                row['phase'] = 'authored-motion-keyframe'
            row['source'] = {'path': str(source.resolve()), 'sha256': digest(source), 'keyframe': index}
            frames.append(row)
    if not frames or len(frames) > MAX_POSES:
        raise ValueError(f'Pose count must be 1..{MAX_POSES}; got {len(frames)}.')
    return {'kind': 'isolated_neck_review_fixture', 'revision': document.get('revision', 1),
            'approval': 'pending', 'adoption': 'not_requested',
            'neck_crop_normalized': crop, 'keyframes': frames}, sources


def model_inputs(model, raw_export=False):
    """Pin export files and the exact runtime masks/component descriptor references."""
    model = Path(model).resolve()
    if not model.name.lower().endswith('.model3.json'):
        raise ValueError('Review model must be an existing .model3.json file.')
    _, family = model_family(model)
    paths = {Path(item['path']) for item in family}
    metadata = model.with_name(model.name.removesuffix('.model3.json') + '.psd2live.json')
    if metadata.is_file() and not raw_export:
        document = read_json(metadata)
        separation = document.get('runtimeMaterialSeparation', {})
        for field in ('underpaint', 'neckMask', 'neckEraseMask', 'bodyMask', 'duplicateMask', 'ownership'):
            reference = separation.get(field)
            if reference:
                paths.add((model.parent / reference).resolve())
        component = document.get('runtimeNeckConnection', {}).get('independentSurface')
        if component:
            component_model = (model.parent / component['model']).resolve()
            _, component_family = model_family(component_model)
            paths.update(Path(item['path']) for item in component_family)
            family += component_family
            paths.add((model.parent / component['rig']).resolve())
    verify_snapshot(family)
    return file_snapshot(paths)


def file_snapshot(paths):
    records = []
    for path in sorted({Path(path).resolve() for path in paths}, key=str):
        if not path.is_file():
            raise ValueError('Missing captured input: ' + str(path))
        records.append({'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest(path)})
    return records


def verify_snapshot(records):
    for item in records:
        path = Path(item['path'])
        if not path.is_file() or digest(path) != item['sha256']:
            raise ValueError('Capture input changed; run is stale: ' + str(path))


def verify_capture(document, model, raw_export, override, *, static=False):
    if Path(document.get('model_manifest_path', '')).resolve() != Path(model).resolve():
        raise ValueError('Native capture used a different model manifest.')
    if (document.get('raw_export_only') is not raw_export
            or document.get('runtime_patches_applied') is not (not raw_export)
            or document.get('isolated_model_override') is not override
            or document.get('render_backend') != 'cubism_native'
            or document.get('adoption') != 'not_requested'):
        raise ValueError('Native capture scope/backend does not match the requested review.')
    if static and (document.get('scope') != 'static_requested_pose_native_capture'
                   or document.get('physics_evaluated') is not False):
        raise ValueError('Static capture did not attest frozen physics.')


def validate_static(folder, manifest, model, raw_export, override):
    evidence = read_json(folder / 'neck.capture.json')
    verify_capture(evidence, model, raw_export, override, static=True)
    frames = manifest['keyframes']
    records = evidence.get('captures', [])
    expected = [f'neck-{index:02}.png' for index in range(len(frames))]
    if len(records) != len(frames) or sorted(path.name for path in folder.glob('neck-*.png')) != sorted(expected):
        raise ValueError('Static capture frame count differs from the fixture.')
    border_failures = 0
    for index, (row, frame) in enumerate(zip(records, frames)):
        if (row.get('file') != expected[index] or row.get('index') != index
                or row.get('frame_saved') is not True or row.get('source_keyframe') != frame):
            raise ValueError('Static capture source keyframe is missing or changed.')
        with Image.open(folder / expected[index]) as picture:
            alpha = picture.convert('RGBA').getchannel('A')
            if alpha.getbbox() is None:
                raise ValueError('Native pose has no visible pixel: ' + expected[index])
            border_failures += int(border_max(alpha) != 0)
    return {'native_poses': len(frames), 'alpha_border_failures': border_failures,
            'scope': 'requested_parameters; Native clamps are not sampled by this image report',
            'physics_evaluated': False}


def crop_neck(picture, crop):
    x, y, width, height = crop
    return picture.crop((round(x * picture.width), round(y * picture.height),
                         round((x + width) * picture.width), round((y + height) * picture.height)))


def compose_static(folder, manifest, output):
    frames = manifest['keyframes']
    sheet = Image.new('RGB', (1080, 76 + math.ceil(len(frames) / 3) * 346), '#e9edf5')
    draw = ImageDraw.Draw(sheet)
    draw.text((18, 14), 'Neck · Native static poses · review pending', font=font(22), fill='#243654')
    draw.text((18, 44), 'Frozen physics; crop is a capture excerpt, not contact geometry measurement.', font=font(15), fill='#52617b')
    for index, frame in enumerate(frames):
        x, y = index % 3 * 360, 76 + index // 3 * 346
        with Image.open(folder / f'neck-{index:02}.png') as source:
            picture = source.convert('RGBA')
        small = picture.resize((180, 180), Image.Resampling.LANCZOS)
        sheet.paste(small, (x + 90, y), small)
        detail = crop_neck(picture, manifest['neck_crop_normalized'])
        detail.thumbnail((342, 122), Image.Resampling.LANCZOS)
        sheet.paste(detail, (x + (360 - detail.width) // 2, y + 180), detail)
        name = str(frame['name'])
        draw.text((x + 10, y + 307), f'{index:02}. {name[:38]}', font=font(15), fill='#243654')
        draw.text((x + 10, y + 327), frame.get('phase', 'static'), font=font(12), fill='#52617b')
    sheet.save(output)


def validate_sequence(folder, count):
    frames = sorted(folder.glob('frame-*.png'))
    if [path.name for path in frames] != [f'frame-{index:03}.png' for index in range(count)]:
        raise ValueError('Native sequence frame count differs from the contract.')
    borders, floors = 0, []
    for path in frames:
        with Image.open(path) as source:
            alpha = source.convert('RGBA').getchannel('A')
        borders += int(border_max(alpha) != 0)
        floors.append(floor_row(alpha))
    return {'sequence_frames': count, 'alpha_border_failures': borders,
            'floor_y_range': [min(floors), max(floors)],
            'floor_drift_pixels': max(floors) - min(floors)}


def compose_neck_sequence(folder, crop, output):
    frames = sorted(folder.glob('frame-*.png'))
    picks = [round(index * (len(frames) - 1) / 11) for index in range(12)]
    sheet = Image.new('RGB', (1260, 76 + 4 * 280), '#e9edf5')
    draw = ImageDraw.Draw(sheet)
    draw.text((18, 14), folder.name + ' · Native neck excerpts · review pending', font=font(22), fill='#243654')
    draw.text((18, 44), 'Every frame is also preserved at original resolution in the scene folder.', font=font(15), fill='#52617b')
    animation = []
    for index, path in enumerate(frames):
        with Image.open(path) as source:
            detail = crop_neck(source.convert('RGBA'), crop).resize((420, 238), Image.Resampling.LANCZOS)
        matte = Image.new('RGBA', detail.size, '#f8faff')
        matte.alpha_composite(detail)
        animation.append(matte.convert('RGB'))
        if index in picks:
            cell = picks.index(index)
            x, y = cell % 3 * 420, 76 + cell // 3 * 280
            sheet.paste(animation[-1], (x, y))
            draw.text((x + 10, y + 246), f'{path.name} · {index / 15:.3f}s', font=font(15), fill='#243654')
    sheet.save(output / (folder.name + '-neck-frames.png'))
    animation[0].save(output / (folder.name + '-neck-motion.gif'), save_all=True,
                      append_images=animation[1:], duration=1000 / 15, loop=0, optimize=False)


def review(model=None, fixture=FIXTURE, raw_export=False, scenes='all', motion_keys=True):
    if raw_export and scenes != 'none':
        raise ValueError('Raw export reviews require --scenes none; runtime transitions are not raw poses.')
    model = Path(model).resolve() if model is not None else DEFAULT_MODEL.resolve()
    override = model != DEFAULT_MODEL.resolve()
    manifest, pose_sources = pose_manifest(fixture, ROOT / 'assets/motions' if motion_keys else None)
    deployment = deployment_snapshot()
    inputs = model_inputs(model, raw_export)
    pinned = file_snapshot([EXE, BUILD / 'Live2DCubismCore.dll',
                            *(ROOT / 'tools' / name for name in TOOLS), *pose_sources])
    run = BUILD / 'review-runs' / ('neck-v1-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
                                 + '-' + uuid4().hex[:8])
    run.mkdir(parents=True, exist_ok=False)
    poses, output = run / 'poses', run / 'artifacts'
    output.mkdir()
    manifest_path = run / 'poses.json'
    write_json(manifest_path, manifest)
    pinned += file_snapshot([manifest_path])
    options = ['--review-model', model] if override else []
    if raw_export:
        options.append('--review-raw-export')
    session = {'schema_version': 1, 'clip': 'neck', 'scenario': 'neck-regression',
               'motion_revision': manifest['revision'], 'review_approval': 'pending',
               'authored_approval': 'not_a_motion', 'adoption': 'not_requested',
               'capture_status': 'started', 'capture_scope': 'raw_export_static_requested_poses' if raw_export
               else 'static_requested_poses_and_runtime_scenes' if scenes != 'none'
               else 'static_requested_poses_with_runtime_patches',
               'model_manifest_path': str(model), 'raw_export_only': raw_export,
               'created_utc': datetime.now(timezone.utc).isoformat(), 'inputs': inputs,
               'source_deployment_inputs': deployment, 'pinned_files': pinned,
               'renderer_sha256': digest(EXE), 'cubism_core_sha256': digest(BUILD / 'Live2DCubismCore.dll'),
               'review_tools': {name: digest(ROOT / 'tools' / name) for name in TOOLS},
               'not_captured': ['Native clamped parameter values', 'neck contact distance/triangle geometry',
                                'physical desktop pointer routing', 'new model/motion adoption']}

    def persist():
        session['artifacts'] = {path.relative_to(run).as_posix(): digest(path)
                                for path in sorted(run.rglob('*')) if path.is_file()
                                and path.name not in ('session.json', 'review-report.json',
                                                     'review-summary.txt', 'visual-review.template.json')}
        write_json(run / 'session.json', session)
        write_review_report(run, session)

    persist()
    print('Isolated neck review: ' + str(run), flush=True)
    try:
        run_exe(['--review-motion', poses, manifest_path, 'neck', *options],
                watch=(poses, len(manifest['keyframes']), 'static poses'))
        static = validate_static(poses, manifest, model, raw_export, override)
        compose_static(poses, manifest, output / 'neck-static.png')
        scene_results = {}
        for name, count, command in [('turn-ended-laptop', 75, '--render-interaction'),
                                     ('busy-laptop', 195, '--render-motion')]:
            if scenes == 'none' or (scenes == 'exit' and name == 'busy-laptop'):
                continue
            sequence = run / name
            run_exe([command, sequence, name, *options], watch=(sequence, count, 'scene frames'))
            evidence = read_json(sequence / ('scene.json' if command == '--render-interaction' else 'capture.json'))
            verify_capture(evidence['capture'] if command == '--render-interaction' else evidence,
                           model, raw_export, override)
            scene_results[name] = validate_sequence(sequence, count)
            if command == '--render-interaction':
                scene_results[name]['player_contracts'] = check_scene(sequence / 'scene.json', name)
            else:
                trace = evidence.get('frames', [])
                if len(trace) != count or evidence.get('physics_evaluated') is not True:
                    raise ValueError('Seated cycle did not capture the full sampled player trace.')
                if trace[-1]['requested_parameters'].get('ParamBusyLaptop', 1) >= 0.01:
                    raise ValueError('Seated cycle did not release to standing.')
            profile = {'name': name, 'motion': 'turn-ended' if command == '--render-interaction' else 'busy-laptop',
                       'revision': 1, 'sequence': sequence, 'frames': count, 'output': output, 'draft': True,
                       'capture_caption': 'Native neck regression; this capture awaits visual review.'}
            compose_sequence(profile)
            compose_desktop_scale(profile)
            compose_neck_sequence(sequence, manifest['neck_crop_normalized'], output)
        verify_snapshot(inputs + pinned)
        if deployment_snapshot() != deployment:
            raise ValueError('Formal/deployed inputs changed during capture; run is stale.')
        validation = {'sequence_frames': sum(value['sequence_frames'] for value in scene_results.values()),
                      'native_poses': static['native_poses'],
                      'alpha_border_failures': static['alpha_border_failures'] + sum(
                          value['alpha_border_failures'] for value in scene_results.values()),
                      'floor_drift_pixels': max((value['floor_drift_pixels'] for value in scene_results.values()), default=0),
                      'static_poses': static, 'scene_contracts': scene_results,
                      'floor_scope': 'each continuous scene only; static fixture floor changes are not measured',
                      'continuous_player_exercised': bool(scene_results),
                      'visual_neck_continuity': 'pending'}
        if validation['alpha_border_failures'] or validation['floor_drift_pixels'] > 4:
            raise ValueError('Native frames clipped or continuous scene floor drift exceeded 4px.')
        write_json(output / 'neck-check.json', validation)
        session.update(validation=validation, capture_status='complete',
                       provenance_checks=dict.fromkeys(('source_deployment_inputs', 'renderer', 'cubism_core', 'review_tools'), 'passed'))
        persist()
    except BaseException as error:
        session.update(capture_status='failed', error=str(error))
        persist()
        raise
    return run


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, help='Explicit isolated model3.json; default is the deployed formal model.')
    parser.add_argument('--fixture', type=Path, default=FIXTURE)
    parser.add_argument('--raw-export', action='store_true', help='Frozen raw export; requires --scenes none.')
    parser.add_argument('--scenes', choices=('none', 'exit', 'all'), default='all')
    parser.add_argument('--no-motion-keys', action='store_true', help='Only diagnostic fixture; omit authored keyframe regression.')
    args = parser.parse_args(argv)
    try:
        review(args.model, args.fixture, args.raw_export, args.scenes, not args.no_motion_keys)
    except (ValueError, OSError) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    main()
