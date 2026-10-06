"""Compose transparent Native evidence with one crop, scale and floor line.

Source PNGs and material assets are read-only. The output is a review matte;
it is not a Native capture, a model adoption or a per-pose fitted preview.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import re
import math
from PIL import Image, ImageDraw, ImageFont

GREY = (65, 69, 78, 255)
EXPECTED_SITS = [0., .35, .65, 1.]


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def natural_key(path):
    token = path.stem.removeprefix('sit-')
    if re.fullmatch(r'\d+(?:\.\d+)?', token):
        return (0, float(token), path.name)
    return (1, path.name)


def font(size):
    for path in ['C:/Windows/Fonts/segoeui.ttf', 'C:/Windows/Fonts/arial.ttf']:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default()


def sit_label(value):
    if value is None:
        return 'Sit ?'
    number = f'{value:.3f}'.rstrip('0').rstrip('.')
    if number == '0':
        number = '.0'
    elif number.startswith('0.'):
        number = number[1:]
    return 'Sit ' + number


def metadata_labels(folder, files, supplied):
    if supplied is not None:
        values = [float(v) for v in supplied.split(',')]
        if len(values) != len(files) or not all(math.isfinite(v) and 0 <= v <= 1 for v in values):
            raise ValueError('--sit-values must give one finite 0..1 value per sorted PNG')
        return values, 'explicit --sit-values', {}, False
    found, sources = {}, {}
    for name in ['pose.capture.json', 'sit.capture.json', 'captures.json', 'capture.json']:
        path = folder / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        doc = json.loads(data)
        sources[str(path.resolve())] = sha_bytes(data)
        if not isinstance(doc, dict):
            continue
        for row in doc.get('captures', []):
            keyframe = row.get('source_keyframe', row)
            value = keyframe.get('parameters', {}).get('ParamSitPose')
            filename = row.get('file')
            if filename and value is not None:
                value = float(value)
                if not math.isfinite(value) or not 0 <= value <= 1:
                    raise ValueError(f'Invalid captured Sit value in {path}')
                basename = Path(filename).name
                if basename in found and found[basename] != value:
                    raise ValueError('Conflicting capture parameter receipts')
                found[basename] = value
    if all(p.name in found for p in files):
        return [found[p.name] for p in files], 'capture source_keyframe parameters', sources, True
    # Fractional filenames are self-descriptive; integer/zero-padded filenames
    # are often ordinals and must not silently be interpreted as parameter values.
    values, has_fraction = [], False
    for p in files:
        token = p.stem.removeprefix('sit-')
        if not re.fullmatch(r'(?:0(?:\.\d+)?|1(?:\.0+)?)', token):
            values = []
            break
        values.append(float(token))
        has_fraction = has_fraction or '.' in token
    if values and has_fraction:
        return values, 'literal fractional filename; parameter receipt absent', sources, False
    if len(files) == 4:
        return EXPECTED_SITS.copy(), 'requested four-pose order .0/.35/.65/1; parameter receipt absent', sources, False
    raise ValueError('No parameter labels for this capture; provide --sit-values or capture metadata')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--height', type=int, default=360, help='Common cropped Native image height, 300..400px')
    parser.add_argument('--padding-source', type=int, default=12)
    parser.add_argument('--floor-y', type=float, help='One physical floor Y in the source PNG coordinate system')
    parser.add_argument('--sit-values', help='Explicit comma-separated labels for naturally sorted files')
    parser.add_argument('--require-parameter-receipt', action='store_true')
    args = parser.parse_args()
    if not args.capture.is_dir():
        raise ValueError('--capture must be an existing directory')
    if not 300 <= args.height <= 400 or not 0 <= args.padding_source <= 128:
        raise ValueError('Height must be 300..400px; source padding must be 0..128px')
    if args.output.suffix.lower() != '.png':
        raise ValueError('--output must be a fresh PNG filename')
    receipt_path = args.output.with_suffix('.json')
    if args.output.exists() or receipt_path.exists():
        raise FileExistsError('Output PNG and its JSON receipt must both be fresh')
    files = sorted(args.capture.glob('sit-*.png'), key=natural_key)
    if not files:
        raise ValueError('No sit-*.png captures found')
    values, label_source, metadata_pins, labels_verified = metadata_labels(args.capture, files, args.sit_values)
    if args.require_parameter_receipt and not labels_verified:
        raise ValueError('Captured parameter receipt required; supplied/default labels are not verification')
    images, inputs = [], []
    canvas = None
    for path in files:
        data = path.read_bytes()
        original = Image.open(io.BytesIO(data))
        if original.format != 'PNG':
            raise ValueError(f'Not a PNG capture: {path}')
        image = original.convert('RGBA')
        alpha = image.getchannel('A')
        if alpha.getextrema()[0] != 0:
            raise ValueError(f'Transparent capture background required: {path}')
        bounds = alpha.getbbox()
        if bounds is None:
            raise ValueError(f'Empty transparent capture: {path}')
        if canvas is None:
            canvas = image.size
        elif image.size != canvas:
            raise ValueError('Source Native canvases must have the same dimensions; do not fit each pose independently')
        images.append(image)
        inputs.append({'path': str(path.resolve()), 'sha256': sha_bytes(data),
                       'native_canvas_size': list(image.size), 'alpha_bbox': list(bounds)})
    union = [min(r['alpha_bbox'][0] for r in inputs), min(r['alpha_bbox'][1] for r in inputs),
             max(r['alpha_bbox'][2] for r in inputs), max(r['alpha_bbox'][3] for r in inputs)]
    floor = args.floor_y if args.floor_y is not None else float(union[3] - 1)
    if not math.isfinite(floor) or not 0 <= floor < canvas[1]:
        raise ValueError('--floor-y must be inside the common source canvas')
    pad = args.padding_source
    crop = [max(0, union[0]-pad), max(0, min(union[1], int(math.floor(floor)))-pad),
            min(canvas[0], union[2]+pad), min(canvas[1], max(union[3], int(math.ceil(floor))+1)+pad)]
    scale = args.height / (crop[3]-crop[1])
    scaled_size = (max(1, round((crop[2]-crop[0])*scale)), args.height)
    header, margin, footer = 58, 12, 42
    column_width = scaled_size[0] + margin*2
    sheet = Image.new('RGBA', (column_width*len(images), header+args.height+footer), GREY)
    draw = ImageDraw.Draw(sheet)
    title_font, filename_font, footer_font = font(20), font(14), font(12)
    floor_in_sheet = header + (floor-crop[1])*scale
    for i, (image, record, value) in enumerate(zip(images, inputs, values)):
        x = i*column_width + margin
        # Every source uses exactly the same crop/resize/paste origin. No pose
        # receives a separate bounding box, centering offset or bottom alignment.
        framed = image.crop(tuple(crop)).resize(scaled_size, Image.Resampling.LANCZOS)
        sheet.alpha_composite(framed, (x, header))
        draw.text((x, 5), sit_label(value), fill=(245,245,245), font=title_font)
        draw.text((x, 32), Path(record['path']).name, fill=(225,225,225), font=filename_font)
        draw.line((x, round(floor_in_sheet), x+scaled_size[0]-1, round(floor_in_sheet)), fill=(140,145,154), width=1)
        record['sit_label'] = value
        record['same_sheet_origin'] = [x, header]
    line1 = f'Union {tuple(union)}; common scale {scale:.5f}; source floor Y={floor:g}'
    line2 = 'Parameter receipt verified' if labels_verified else 'Labels supplied/filename-based; captured parameter receipt absent'
    if args.floor_y is None:
        line2 += '; line is common alpha-bottom reference'
    draw.text((margin, header+args.height+4), line1, fill=(220,220,220), font=footer_font)
    draw.text((margin, header+args.height+21), line2, fill=(220,220,220), font=footer_font)
    for record in inputs:
        if sha_bytes(Path(record['path']).read_bytes()) != record['sha256']:
            raise ValueError('Capture changed during composition')
    for path, pin in metadata_pins.items():
        if sha_bytes(Path(path).read_bytes()) != pin:
            raise ValueError('Capture parameter receipt changed during composition')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as f:
        sheet.convert('RGB').save(f, format='PNG')
    report = {'schema_version': 1, 'kind': 'common_scale_Native_capture_review_matte',
        'created_utc': datetime.now(timezone.utc).isoformat(),
        'source_capture_directory': str(args.capture.resolve()), 'inputs': inputs,
        'capture_parameter_receipts': metadata_pins, 'sit_label_source': label_source,
        'sit_values_verified_against_capture_receipt': labels_verified,
        'common_native_canvas': list(canvas), 'union_alpha_bbox': union, 'common_crop': crop,
        'common_scale': scale, 'common_resized_dimensions': list(scaled_size),
        'source_floor_y': floor, 'floor_reference': 'explicit source floor' if args.floor_y is not None else 'union alpha bottom; physical floor unverified',
        'floor_y_in_sheet': floor_in_sheet, 'per_pose_fit_or_vertical_shift': False,
        'source_images_unchanged': True, 'source_transparent_background_required': True,
        'output': {'path': str(args.output.resolve()), 'sha256': sha_bytes(args.output.read_bytes()), 'size': list(sheet.size)},
        'tool': {'path': str(Path(__file__).resolve()), 'sha256': sha_bytes(Path(__file__).read_bytes())},
        'visual_review': 'pending', 'adoption': 'pending',
        'limits': ['This is a matte derived from supplied PNG bytes, not a new Native render.',
                   'Common source dimensions are checked; source renderer scale/model identity still belongs to the original capture receipt.',
                   'Physical foot/palm contact is not proved by an alpha-bottom reference line.']}
    with receipt_path.open('x', encoding='utf8') as f:
        f.write(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(json.dumps({'output': str(args.output.resolve()), 'receipt': str(receipt_path.resolve()),
        'common_crop': crop, 'common_scale': scale, 'frames': len(inputs),
        'parameter_labels_verified': labels_verified}, indent=2))


if __name__ == '__main__':
    main()
