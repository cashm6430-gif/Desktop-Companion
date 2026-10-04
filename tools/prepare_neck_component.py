"""Prepare the auxiliary neck's missing lower skin coverage in a fresh source pack.

This is a deterministic texture-authoring step, using only the existing skin
palette. The main character atlas, jaw, bindings and motion files are inputs.
"""
from pathlib import Path
import argparse
import hashlib
import json

import numpy as np
from PIL import Image, ImageDraw
from psd_tools import PSDImage

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'art/live2d/neck-component-r1'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prepare(output, source=SOURCE):
    source, output = Path(source).resolve(), Path(output).resolve()
    if output.exists() or output == source or source.is_relative_to(output):
        raise ValueError('Output must be a new, separate source directory.')
    if not output.is_relative_to(ROOT / 'art/live2d') and not output.is_relative_to(ROOT / '.local/authoring'):
        raise ValueError('Output must stay in this project authoring area.')
    inputs = [source / name for name in ('neck-authoring.png', 'neck-design.json', 'neck-source.json')]
    pinned = {str(path): digest(path) for path in inputs}
    original = np.array(Image.open(inputs[0]).convert('RGBA'))
    if original.shape != (1254, 1254, 4):
        raise ValueError('Unexpected original source canvas.')
    author = original.copy()
    # Retain the complete jaw and its original upper shadow. The old lower
    # V-shaped dark outline must also go: leaving it inside new skin would
    # create an internal seam even when the alpha hole is filled.
    start, end = 574, 598
    if original[start:end, :555, 3].any() or original[start:end, 682:, 3].any():
        raise ValueError('Replacement rows contain artwork outside the registered neck.')
    author[start:end, :, :] = 0
    polygon = [(610, 574), (613, 581), (616, 589), (616, 598),
               (644, 598), (645, 589), (650, 581), (654, 574)]
    factor = 8
    coverage = Image.new('L', (128 * factor, (end - start) * factor))
    ImageDraw.Draw(coverage).polygon([((x - 555) * factor, (y - start) * factor) for x, y in polygon], fill=255)
    coverage = np.array(coverage.resize((128, end - start), Image.Resampling.BOX))
    # Source y574 is an uninterrupted, warm neck-interior row. Resampling its
    # horizontal shading preserves the palette and avoids copying black V
    # contour pixels into the exposed seated neck. Lower colour comes from
    # the original interior, with no invented face detail or chest repaint.
    profile = original[574, 612:644, :3].astype(float)
    lower = original[578:582, 625:633, :3].astype(float).mean(axis=(0, 1))
    if not ((profile[:, 0] > 230).all() and (profile[:, 0] > profile[:, 2] + 30).all()):
        raise ValueError('Source profile contains a contour or non-skin pixel.')
    for row, y in enumerate(range(start, end)):
        xs = np.flatnonzero(coverage[row] > 0)
        if not len(xs):
            raise ValueError('Missing lower-neck source row.')
        t = np.linspace(0, len(profile) - 1, len(xs))
        colours = np.stack([np.interp(t, np.arange(len(profile)), profile[:, channel]) for channel in range(3)], axis=1)
        blend = min(1, (y - start) / 15) * .35
        colours = colours * (1 - blend) + lower * blend
        author[y, xs + 555, :3] = np.rint(colours).astype(np.uint8)
        author[y, xs + 555, 3] = coverage[row, xs]
    author[author[:, :, 3] == 0, :3] = 0
    if not np.array_equal(author[:start], original[:start]):
        raise ValueError('Jaw or upper shadow changed.')
    if not np.array_equal(author[end:], original[end:]):
        raise ValueError('Outside-neck source changed.')
    # Explicit coverage gate measures a geometric interior, not a difference
    # bounding box. Both lower corners remain opaque, even as the collar
    # descends. Antialiasing is allowed only outside this interior.
    required = {'left': [618, 586], 'right': [641, 586],
                'left_lower': [619, 592], 'right_lower': [640, 592]}
    if any(author[y, x, 3] != 255 for x, y in required.values()):
        raise ValueError('A required seated lower-neck corner is transparent.')
    for y in range(581, end):
        if not (author[y, 618:642, 3] == 255).all():
            raise ValueError(f'Transparent lower-neck interior at source y{y}.')
    output.mkdir(parents=True, exist_ok=False)
    image = Image.fromarray(author)
    image.save(output / 'neck-authoring.png')
    bounds = image.getbbox()
    psd = PSDImage.new(mode='RGBA', size=image.size, color=(0, 0, 0, 0), depth=8)
    psd.create_pixel_layer(image.crop(bounds), name='neck', top=bounds[1], left=bounds[0])
    psd.save(output / 'neck-authoring.psd')
    # Read actual layer channels. PSD composite() may substitute white RGB
    # under alpha zero; these invisible pixels are not the stored texture.
    reopened = PSDImage.open(output / 'neck-authoring.psd')
    if len(reopened) != 1 or reopened[0].name != 'neck':
        raise ValueError('PSD lost its single neck layer.')
    layer = reopened[0]
    decoded = Image.new('RGBA', reopened.size)
    decoded.paste(layer.topil().convert('RGBA'), (layer.left, layer.top))
    if not np.array_equal(np.array(decoded), author):
        raise ValueError('PSD full RGBA differs from authoring PNG.')
    (output / 'neck-design.json').write_bytes(inputs[1].read_bytes())
    report = {'version': 1, 'kind': 'auxiliary_neck_lower_skin_coverage',
              'source_inputs': pinned, 'source_canvas': [1254, 1254],
              'source_bbox': list(bounds), 'retained_upper_rows': [0, start],
              'replacement_rows': [start, end], 'skin_polygon_source': polygon,
              'colour_source_profile': [612, 574, 644, 575],
              'colour_source_lower': [625, 578, 633, 582],
              'required_opaque_corner_samples': required,
              'lower_interior_required_opaque': [618, 581, 642, end],
              'old_V_contour_replaced': True, 'upper_jaw_rgba_identical': True,
              'outside_replacement_rows_rgba_identical': True,
              'replacement_rows_original_art_limited_to_neck': [555, start, 682, end],
              'mesh_design_byte_identical': True,
              'decoded_PSD_RGBA_equals_PNG': True, 'visual_review': 'pending',
              'standing_visible_protection': 'Existing foreground standing collar occludes added skin; Native comparison required.',
              'outputs': {name: digest(output / name) for name in ('neck-authoring.png', 'neck-authoring.psd', 'neck-design.json')}}
    for path, sha in pinned.items():
        if digest(path) != sha:
            raise ValueError('Source pack changed during preparation.')
    (output / 'neck-source.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps({'output': str(output), 'bbox': bounds, 'coverage_corners_opaque': True,
                      'upper_jaw_identical': True, 'PSD_decoding_exact': True}, ensure_ascii=False))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=SOURCE)
    args = parser.parse_args()
    prepare(args.output, args.source)
