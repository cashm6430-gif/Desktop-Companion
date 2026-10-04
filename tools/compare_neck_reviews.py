"""Compare two immutable Native neck reviews and paginate every captured frame.

    python tools/compare_neck_reviews.py --before <baseline-run> --after <candidate-run> \
        --output art/live2d/review/neck-refinement-20261004 --regions <independent-roi.json>

Regions must come from independent geometry, never raster differences. Without
regions, full RGBA metrics and review pages are produced, but outside-neck
verification remains pending. Image metrics do not approve neck appearance.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from probe_cubism_core import model_family
from review_neck import (ROOT, digest, file_snapshot, verify_snapshot, read_json,
                         verify_capture, validate_static, validate_sequence)
from review_session import deployment_snapshot

SCENES = (('turn-ended-laptop', 75), ('busy-laptop', 195))
FONT = 'C:/Windows/Fonts/msyh.ttc'


def write_json(path, data):
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def font(size):
    return ImageFont.truetype(FONT, size)


def load_review(run):
    run = Path(run).resolve()
    session = read_json(run / 'session.json')
    if session.get('capture_status') != 'complete' or session.get('raw_export_only') is not False:
        raise ValueError('Need a completed runtime review: ' + str(run))
    if not all(session.get('provenance_checks', {}).get(key) == 'passed' for key in
               ('source_deployment_inputs', 'renderer', 'cubism_core', 'review_tools')):
        raise ValueError('Capture provenance did not pass: ' + str(run))
    verify_snapshot(session['inputs'] + session['pinned_files'])
    report = read_json(run / 'review-report.json')
    if report.get('session_sha256') != digest(run / 'session.json'):
        raise ValueError('Review report belongs to a different session: ' + str(run))
    artifacts = session.get('artifacts', {})
    if not artifacts:
        raise ValueError('No hashed capture artifacts: ' + str(run))
    for name, expected in artifacts.items():
        path = (run / name).resolve()
        if (Path(name).is_absolute() or not path.is_relative_to(run)
                or not path.is_file() or digest(path) != expected):
            raise ValueError('Capture artifact is missing, changed or outside run: ' + name)
    if deployment_snapshot() != session['source_deployment_inputs']:
        raise ValueError('Formal assets/deployment changed since review: ' + str(run))
    manifest = read_json(run / 'poses.json')
    frames = manifest.get('keyframes', [])
    if (len(frames) != 83 or sum(row.get('phase') == 'authored-motion-keyframe' for row in frames) != 67
            or sum(row.get('phase') != 'authored-motion-keyframe' for row in frames) != 16):
        raise ValueError('Expected 83 poses = 16 diagnostic + 67 authored keys.')
    model = Path(session['model_manifest_path']).resolve()
    default = (ROOT / 'build/assets/live2d/whale-girl/whale-girl-layered-draft.model3.json').resolve()
    override = model != default
    validate_static(run / 'poses', manifest, model, False, override)
    rows = [{'key': 'poses/' + f'neck-{index:02}.png', 'group': 'poses', 'index': index,
             'label': str(frame['name']), 'parameters': frame['parameters']}
            for index, frame in enumerate(frames)]
    for scene, count in SCENES:
        metrics = validate_sequence(run / scene, count)
        if metrics['alpha_border_failures'] or metrics['floor_drift_pixels'] > 4:
            raise ValueError('Native scene clipped or floor drifted: ' + scene)
        evidence = read_json(run / scene / ('scene.json' if scene == 'turn-ended-laptop' else 'capture.json'))
        verify_capture(evidence['capture'] if scene == 'turn-ended-laptop' else evidence,
                       model, False, override)
        trace = evidence.get('frames', [])
        if len(trace) != count:
            raise ValueError('Scene trace is incomplete: ' + scene)
        for index, frame in enumerate(trace):
            if frame.get('time') != index / 15:
                # C++ multiplication by step differs from division by one ulp.
                if abs(frame.get('time', float('inf')) - index / 15) > 1e-12:
                    raise ValueError('Scene trace time differs from fixed step: ' + scene)
            rows.append({'key': scene + '/' + f'frame-{index:03}.png', 'group': scene,
                         'index': index, 'label': f'{scene} {index:03} / {index / 15:.3f}s',
                         'parameters': frame.get('parameters') if scene == 'turn-ended-laptop'
                         else frame.get('requested_parameters')})
    if session.get('validation', {}).get('sequence_frames') != 270:
        raise ValueError('Expected 270 continuous frames.')
    return {'run': run, 'session': session, 'manifest': manifest, 'model': model, 'rows': rows}


def frozen_model_files(review):
    _, family = model_family(review['model'])
    # Metadata has one intentional descriptor change. CMO3 is authoring-only.
    ignored = {'export_metadata', 'editable_export'}
    result = {tuple(item['roles']): item['sha256'] for item in family
              if not set(item['roles']) & ignored}
    metadata_path = review['model'].with_name(review['model'].name.removesuffix('.model3.json') + '.psd2live.json')
    metadata = read_json(metadata_path)
    neck = dict(metadata.get('runtimeNeckConnection', {}))
    component = neck.pop('independentSurface', None)
    metadata = dict(metadata, runtimeNeckConnection=neck)
    masks = metadata.get('runtimeMaterialSeparation', {})
    for field in ('underpaint', 'neckMask', 'neckEraseMask', 'bodyMask', 'duplicateMask', 'ownership'):
        if field in masks:
            result[('runtimeMaterialSeparation.' + field,)] = digest(review['model'].parent / masks[field])
    return result, metadata, component


def verify_pair(before, after):
    for field in ('renderer_sha256', 'cubism_core_sha256', 'review_tools', 'source_deployment_inputs'):
        if before['session'].get(field) != after['session'].get(field):
            raise ValueError('Reviews are not comparable; different ' + field)
    if before['manifest'] != after['manifest'] or before['rows'] != after['rows']:
        raise ValueError('Reviews have different source poses or per-frame requested parameters.')
    a_files, a_metadata, a_component = frozen_model_files(before)
    b_files, b_metadata, b_component = frozen_model_files(after)
    if a_files != b_files:
        raise ValueError('Main Native MOC/atlas/physics/export motions/masks changed.')
    if a_metadata != b_metadata:
        raise ValueError('Main metadata changed beyond the independent neck descriptor.')
    if a_component is not None or not isinstance(b_component, dict):
        raise ValueError('Expected baseline legacy neck and candidate independent neck descriptor.')
    component_model = (after['model'].parent / b_component['model']).resolve()
    refs = read_json(component_model)['FileReferences']
    members = {'mocSha256': component_model.parent / refs['Moc'],
               'rigSha256': after['model'].parent / b_component['rig']}
    textures = refs.get('Textures', [])
    if len(textures) != 1:
        raise ValueError('Independent neck must have one declared atlas for this descriptor.')
    members['atlasSha256'] = component_model.parent / textures[0]
    pinned = {str(Path(item['path']).resolve()): item['sha256'] for item in after['session']['inputs']}
    for key, path in members.items():
        actual = digest(path)
        if b_component.get(key) != actual or pinned.get(str(path.resolve())) != actual:
            raise ValueError('Independent neck descriptor/member SHA is not pinned: ' + key)
    if pinned.get(str(component_model)) != digest(component_model):
        raise ValueError('Independent neck model3 manifest is not pinned.')
    return {'main_runtime_files': len(a_files), 'main_files_and_metadata_frozen': True,
            'independent_surface_descriptor': b_component,
            'independent_surface_sha256_checked': True}


def load_regions(path, before, after):
    if path is None:
        return None
    document = read_json(path)
    if (document.get('derivation') != 'native_geometry_before_raster_comparison'
            or document.get('baseline_session_sha256') != digest(before['run'] / 'session.json')
            or document.get('candidate_session_sha256') != digest(after['run'] / 'session.json')):
        raise ValueError('Regions must bind both sessions and declare independent geometry derivation.')
    records = document.get('inputs', [])
    if not records:
        raise ValueError('Independent region geometry must include hashed model/tool inputs.')
    verify_snapshot(records)
    regions = document.get('regions', {})
    if set(regions) != {row['key'] for row in before['rows']}:
        raise ValueError('Independent neck regions must cover exactly all 353 captured images.')
    for key, rectangles in regions.items():
        if (not isinstance(rectangles, list) or not rectangles
                or any(not isinstance(rectangle, list) or len(rectangle) != 4
                       or any(not isinstance(value, int) or isinstance(value, bool) for value in rectangle)
                       or rectangle[0] < 0 or rectangle[1] < 0
                       or rectangle[2] <= rectangle[0] or rectangle[3] <= rectangle[1]
                       for rectangle in rectangles)):
            raise ValueError('Expected integer pixel rectangles [left,top,right,bottom]: ' + key)
    return document


def bbox(mask):
    yy, xx = np.nonzero(mask)
    return None if not len(xx) else [int(xx.min()), int(yy.min()), int(xx.max() + 1), int(yy.max() + 1)]


def rgba_metrics(a, b, rectangles=None):
    if a.shape != b.shape or a.ndim != 3 or a.shape[2] != 4:
        raise ValueError('RGBA image dimensions differ.')
    difference = np.abs(a.astype(np.int16) - b.astype(np.int16))
    changed = np.any(difference != 0, axis=2)
    visible = changed & ((a[:, :, 3] != 0) | (b[:, :, 3] != 0))
    result = {'size': [a.shape[1], a.shape[0]], 'changed_rgba_pixels': int(changed.sum()),
              'changed_visible_pixels': int(visible.sum()), 'changed_bbox': bbox(changed),
              'visible_changed_bbox': bbox(visible), 'maximum_rgba_channel_error': int(difference.max()),
              'mean_absolute_rgba_channel_error': float(difference.mean())}
    if rectangles is not None:
        permitted = np.zeros(changed.shape, dtype=bool)
        for left, top, right, bottom in rectangles:
            if right > a.shape[1] or bottom > a.shape[0]:
                raise ValueError('Neck region exceeds Native capture dimensions.')
            permitted[top:bottom, left:right] = True
        outside = changed & ~permitted
        result.update(independent_neck_regions=rectangles,
                      changed_pixels_outside_neck=int(outside.sum()),
                      changed_visible_pixels_outside_neck=int((visible & ~permitted).sum()),
                      outside_neck_changed_bbox=bbox(outside))
    else:
        result.update(changed_pixels_outside_neck=None,
                      region_verification='pending_independent_geometry_regions')
    return result


def preview_box(size, regions, crop):
    if regions:
        left = min(rect[0] for rect in regions)
        top = min(rect[1] for rect in regions)
        right = max(rect[2] for rect in regions)
        bottom = max(rect[3] for rect in regions)
        # Context for visual inspection. This padded crop never defines what
        # pixels are permitted to change; metrics use the independent regions.
        pad = 25
        return [max(0, left - pad), max(0, top - pad), min(size[0], right + pad), min(size[1], bottom + pad)]
    x, y, width, height = crop
    return [round(x * size[0]), round(y * size[1]), round((x + width) * size[0]), round((y + height) * size[1])]


def matte(picture, size, background='#f8faff'):
    result = Image.new('RGBA', size, background)
    picture = picture.copy()
    picture.thumbnail(size, Image.Resampling.LANCZOS)
    result.alpha_composite(picture, ((size[0] - picture.width) // 2, (size[1] - picture.height) // 2))
    return result.convert('RGB')


def page_pairs(before, after, rows, regions, output, crop):
    pages = []
    for group in ('poses', *(name for name, _ in SCENES)):
        selected = [row for row in rows if row['group'] == group]
        for page, offset in enumerate(range(0, len(selected), 25)):
            batch = selected[offset:offset + 25]
            sheet = Image.new('RGB', (1600, 90 + math.ceil(len(batch) / 5) * 220), '#e9edf5')
            draw = ImageDraw.Draw(sheet)
            draw.text((20, 13), f'工程精修 / Native · {group} · {offset}–{offset + len(batch) - 1}',
                      font=font(22), fill='#243654')
            draw.text((20, 48), '每格左：基准；右：新颈部。全部连续帧分页；接缝、遮挡和形状需逐格复核。',
                      font=font(17), fill='#52617b')
            for cell, row in enumerate(batch):
                x, y = cell % 5 * 320, 90 + cell // 5 * 220
                with Image.open(before['run'] / row['key']) as src:
                    a = src.convert('RGBA')
                with Image.open(after['run'] / row['key']) as src:
                    b = src.convert('RGBA')
                rectangles = regions['regions'][row['key']] if regions else None
                box = preview_box(a.size, rectangles, crop)
                sheet.paste(matte(a.crop(box), (152, 168)), (x + 4, y + 4))
                sheet.paste(matte(b.crop(box), (152, 168)), (x + 164, y + 4))
                draw.text((x + 8, y + 176), f"{row['index']:03} · {row['label'][:27]}", font=font(12), fill='#243654')
                draw.text((x + 8, y + 196), '基准                         新颈部', font=font(12), fill='#52617b')
            name = f'{group}-pairs-page-{page + 1:02}.png'
            sheet.save(output / name)
            pages.append({'file': name, 'group': group, 'first': batch[0]['key'],
                          'last': batch[-1]['key'], 'count': len(batch)})
    return pages


def compose_key_comparisons(before, after, regions, output, crop):
    keys = ['poses/neck-00.png', 'poses/neck-05.png', 'poses/neck-06.png',
            'poses/neck-12.png', 'turn-ended-laptop/frame-030.png', 'busy-laptop/frame-190.png']
    rows = {row['key']: row for row in before['rows']}
    desktop = Image.new('RGB', (1800, 690), '#e9edf5')
    detail = Image.new('RGB', (1800, 490), '#e9edf5')
    for sheet in (desktop, detail):
        draw = ImageDraw.Draw(sheet)
        draw.text((20, 15), '工程精修 / Native · 基准与新颈部', font=font(25), fill='#243654')
    for cell, key in enumerate(keys):
        x, y = cell % 3 * 600, 60 + cell // 3 * 312
        with Image.open(before['run'] / key) as source:
            a = source.convert('RGBA')
        with Image.open(after['run'] / key) as source:
            b = source.convert('RGBA')
        for column, picture in enumerate((a, b)):
            desktop.paste(matte(picture, (280, 280)), (x + 10 + column * 300, y))
        ImageDraw.Draw(desktop).text((x + 10, y + 283), key, font=font(14), fill='#243654')
        box = preview_box(a.size, regions['regions'][key] if regions else None, crop)
        dy = 60 + cell // 3 * 208
        for column, picture in enumerate((a, b)):
            detail.paste(matte(picture.crop(box), (280, 176)), (x + 10 + column * 300, dy))
        ImageDraw.Draw(detail).text((x + 10, dy + 180), rows[key]['label'][:48], font=font(14), fill='#243654')
    desktop.save(output / 'neck-before-after-280px.png')
    detail.save(output / 'neck-before-after-key-crops.png')


def compare(before_path, after_path, output, regions_path=None):
    before, after = load_review(before_path), load_review(after_path)
    frozen = verify_pair(before, after)
    regions = load_regions(regions_path, before, after)
    output = Path(output).resolve()
    roots = (ROOT / 'build/review-comparisons', ROOT / 'art/live2d/review',
             ROOT / '.local/authoring/neck-refinement/comparisons')
    if (not any(output.is_relative_to(root.resolve()) for root in roots)
            or output.is_relative_to(before['run']) or output.is_relative_to(after['run'])):
        raise ValueError('Comparison output must be a fresh review/comparison directory.')
    output.mkdir(parents=True, exist_ok=False)
    pinned = file_snapshot([Path(__file__), before['run'] / 'session.json', after['run'] / 'session.json',
                            *([regions_path] if regions_path else [])])
    metrics = []
    print('Comparing 83 static poses and 270 continuous frames.', flush=True)
    for row in before['rows']:
        with Image.open(before['run'] / row['key']) as src:
            a = np.array(src.convert('RGBA'))
        with Image.open(after['run'] / row['key']) as src:
            b = np.array(src.convert('RGBA'))
        item = {'file': row['key'], 'group': row['group'], 'index': row['index'], 'label': row['label']}
        item.update(rgba_metrics(a, b, regions['regions'][row['key']] if regions else None))
        metrics.append(item)
    crop = before['manifest']['neck_crop_normalized']
    pages = page_pairs(before, after, before['rows'], regions, output, crop)
    compose_key_comparisons(before, after, regions, output, crop)
    # Recheck source, renderer, module, poses and all immutable capture frames
    # after composing. A comparison cannot silently span a deployment/adoption.
    load_review(before['run'])
    load_review(after['run'])
    verify_snapshot(pinned)
    if regions:
        verify_snapshot(regions['inputs'])
    outside = sum(item['changed_pixels_outside_neck'] for item in metrics) if regions else None
    result = {'schema_version': 1, 'kind': 'same_renderer_native_neck_comparison',
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'baseline_run': str(before['run']), 'candidate_run': str(after['run']),
              'baseline_session_sha256': digest(before['run'] / 'session.json'),
              'candidate_session_sha256': digest(after['run'] / 'session.json'),
              'renderer_sha256': before['session']['renderer_sha256'],
              'cubism_core_sha256': before['session']['cubism_core_sha256'],
              'source_file_checks': frozen, 'inputs': pinned,
              'region_evidence': {'path': str(Path(regions_path).resolve()), 'sha256': digest(regions_path)} if regions else None,
              'summary': {'static_poses': 83, 'continuous_frames': 270, 'compared_images': len(metrics),
                          'changed_images': sum(item['changed_rgba_pixels'] != 0 for item in metrics),
                          'changed_rgba_pixels_total': sum(item['changed_rgba_pixels'] for item in metrics),
                          'changed_pixels_outside_neck_total': outside,
                          'neck_exterior_check': 'pending_independent_regions' if outside is None else
                          'passed' if outside == 0 else 'failed',
                          'visual_contact_and_shape': 'pending', 'adoption': 'not_requested'},
              'frames': metrics, 'review_pages': pages,
              'artifacts': {path.name: digest(path) for path in sorted(output.glob('*.png'))},
              'limitations': ['RGBA metrics do not measure anatomical contact or approve appearance.',
                              'Padded visual crops never define the allowed change region.',
                              'This comparison does not approve or replace any authored motion.']}
    write_json(output / 'comparison.json', result)
    print(json.dumps(result['summary'], ensure_ascii=False), flush=True)
    print('Comparison: ' + str(output), flush=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--regions', type=Path, help='Independent geometry-derived per-image ROI file.')
    args = parser.parse_args(argv)
    try:
        result = compare(args.before, args.after, args.output, args.regions)
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(2, str(error) + '\n')
    return 1 if result['summary']['neck_exterior_check'] == 'failed' else 0


if __name__ == '__main__':
    raise SystemExit(main())
