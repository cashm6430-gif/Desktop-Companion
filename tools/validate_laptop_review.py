"""Check native review captures for clipping and a moving desktop floor."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

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
        alpha = np.asarray(Image.open(path).convert('RGBA'))[:, :, 3]
        assert not max(alpha[0].max(), alpha[-1].max(), alpha[:, 0].max(), alpha[:, -1].max()), path
        if path in frames:
            floors.append(int(np.nonzero(alpha > 128)[0].max()))
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
