"""Check native review captures for clipping and a moving desktop floor.

Uses Pillow only, so the review loop runs without the heavier model-build
dependencies. The alpha border and floor checks are unchanged.
"""
import argparse
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ALPHA_THRESHOLD = 128


def alpha_channel(path):
    return Image.open(path).convert("RGBA").getchannel("A")


def border_max(alpha):
    width, height = alpha.size
    return max(
        alpha.crop((0, 0, width, 1)).getextrema()[1],
        alpha.crop((0, height - 1, width, height)).getextrema()[1],
        alpha.crop((0, 0, 1, height)).getextrema()[1],
        alpha.crop((width - 1, 0, width, height)).getextrema()[1],
    )


def floor_row(alpha):
    solid = alpha.point(lambda value: 255 if value > ALPHA_THRESHOLD else 0)
    box = solid.getbbox()
    assert box is not None, "Frame has no opaque pixel"
    return box[3] - 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--revision', type=int, default=2)
    args = parser.parse_args()
    version = f'v{args.revision}'
    frames = sorted((ROOT / f'build/laptop-{version}-sequence').glob('frame-*.png'))
    poses = sorted((ROOT / f'build/laptop-{version}-review').glob('grass-*.png'))
    assert len(frames) == 195 and len(poses) == 6, 'Incomplete native capture'
    floors = []
    for path in frames + poses:
        alpha = alpha_channel(path)
        assert not border_max(alpha), path
        if path in frames:
            floors.append(floor_row(alpha))
    # These poses place the shoes below hair, skirt and tail. Alpha contours
    # independently check the floor, rather than testing the renderer's offset.
    assert max(floors) - min(floors) <= 4, f'Floor drift: {min(floors)}..{max(floors)}'
    report = {'sequence_frames': len(frames), 'native_poses': len(poses),
              'alpha_border_failures': 0, 'floor_y_range': [min(floors), max(floors)],
              'floor_drift_pixels': max(floors) - min(floors)}
    (ROOT / f'build/laptop-{version}-review-check.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
