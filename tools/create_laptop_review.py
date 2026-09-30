"""Build approval artifacts exclusively from native model captures.

Thin wrapper over tools/review_motion.py; prefer

    python tools/review_motion.py busy-laptop

Kept because the seated-laptop commands are documented. The sheet layout and
the artifact names are unchanged, so the approved v1..v3 sheets rebuild byte
for byte.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import review_motion


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--revision', type=int, default=1)
    parser.add_argument('--manifest', action='store_true')
    args = parser.parse_args()
    profile = review_motion.profile('busy-laptop', args.revision)
    if args.manifest:
        print(review_motion.ensure_manifest(profile))
        return 0
    if not review_motion.pose_paths(profile):
        review_motion.capture(profile)
    review_motion.compose(profile)
    return 0


if __name__ == '__main__':
    sys.exit(main())
