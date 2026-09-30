"""Create a review sheet from actual Cubism-rendered poses, never generated art.

Thin wrapper over tools/review_motion.py; prefer

    python tools/review_motion.py grass

Kept because the grass commands are documented. The sheet, the expression
detail, the wrist / arm / eye panels and the loop GIF keep their previous
layouts and file names, so the approved v7 sheet rebuilds byte for byte.

The --eye-manifest / --arm-manifest / --wrist-manifest flags still write one
sweep manifest and print its path, for rendering with

    DesktopCompanion.exe --review-motion <dir> <manifest> grass
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import review_motion

SWEEP_FLAGS = {'--eye-manifest': 'eye', '--arm-manifest': 'arm', '--wrist-manifest': 'wrist'}


def main():
    profile = review_motion.profile('grass')
    for flag, kind in SWEEP_FLAGS.items():
        if flag in sys.argv:
            print(review_motion.write_grass_sweep_manifest(kind, profile))
            return 0
    if not review_motion.pose_paths(profile):
        review_motion.capture(profile)
    review_motion.compose(profile)
    return 0


if __name__ == "__main__":
    sys.exit(main())
