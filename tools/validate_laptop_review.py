"""Check native review captures for clipping and a moving desktop floor.

Thin wrapper over tools/review_motion.py; prefer

    python tools/review_motion.py <clip> --validate

for any motion. Kept because the seated-laptop command is documented and the
report path build/laptop-v{revision}-review-check.json is unchanged.
Uses Pillow only, so the review loop runs without the heavier model-build
dependencies.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import review_motion


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--revision', type=int, default=2)
    args = parser.parse_args()
    profile = review_motion.profile('busy-laptop', args.revision)
    review_motion.validate(profile, expected_poses=6)
    return 0


if __name__ == '__main__':
    sys.exit(main())
