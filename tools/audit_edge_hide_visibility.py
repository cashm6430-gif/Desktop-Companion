"""Measure visible non-background pixels in an isolated edge review capture.

This is a coarse diagnostic for a specific virtual-screen fixture, not a
semantic measure of head visibility or a substitute for visual review.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image


def longest_zero_run(counts):
    longest = current = 0
    for count in counts:
        current = current + 1 if count == 0 else 0
        longest = max(longest, current)
    return longest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--continuous', action='store_true', help='audit the continuous-entry candidate')
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(f'Refusing to overwrite {args.output}')
    result = {'scope': 'approximate visible pixels in isolated Qt edge review',
              'frame_range': [12, 32], 'step_seconds': 0.05,
              'background_rgb': [234, 240, 245], 'channel_threshold': 18,
              'crop': [22, 22, 538, 538], 'sides': {},
              'limitations': ['pixel difference is not semantic head detection',
                              'only the scripted virtual-screen entry is measured',
                              'human review remains necessary']}
    for side in ('left', 'right', 'top', 'bottom'):
        folder = args.capture_root / side
        scene = json.loads((folder / 'scene.json').read_text(encoding='utf8'))
        prefix = 'edge-hide-continuous-' if args.continuous else 'edge-hide-'
        if (not scene.get('passed') or scene.get('scenario') != prefix + side
                or scene.get('scope') != 'isolated_qt_virtual_screen_live_cubism_synthetic_pointer'
                or scene.get('frame_count') != 75):
            raise ValueError(f'Invalid scene: {side}')
        counts = []
        hashes = []
        background = np.array(result['background_rgb'], dtype=np.int16)
        for index in range(12, 33):
            path = folder / f'frame-{index:03}.png'
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest())
            image = Image.open(path).convert('RGB')
            if image.size != (560, 560):
                raise ValueError(f'Unexpected frame size: {path}')
            pixels = np.asarray(image, dtype=np.int16)[22:538, 22:538]
            difference = np.max(np.abs(pixels - background), axis=2)
            counts.append(int(np.count_nonzero(difference > result['channel_threshold'])))
        run = longest_zero_run(counts)
        result['sides'][side] = {'pixels_by_frame': counts, 'longest_zero_frames': run,
                                 'longest_zero_seconds': round(run * result['step_seconds'], 3),
                                 'scene_sha256': hashlib.sha256((folder / 'scene.json').read_bytes()).hexdigest(),
                                 'frame_sha256': hashes}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps({k: v['longest_zero_seconds'] for k, v in result['sides'].items()}, ensure_ascii=False))


if __name__ == '__main__':
    main()
