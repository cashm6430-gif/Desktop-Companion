"""Use an explicitly selected, isolated renderer for whole-model capture.

    python tools/review_isolated_model.py --renderer <DesktopCompanion.exe> \
        capture --model <model3.json> --fixture <poses.json> --output <new-dir>

The runtime directory must contain its own DLLs and motion assets. Both this
entry point and the renderer are pinned in every capture receipt. The usual
build deployment is not changed.
"""
from pathlib import Path
import sys

import review_model_equivalence as gate
import review_motion


def main():
    arguments = sys.argv[1:]
    if arguments.count('--renderer') != 1:
        raise SystemExit('Provide exactly one --renderer path.')
    index = arguments.index('--renderer')
    if index + 1 >= len(arguments):
        raise SystemExit('--renderer needs an executable path.')
    executable = Path(arguments[index + 1]).resolve(strict=True)
    if executable.name != 'DesktopCompanion.exe':
        raise SystemExit('The isolated renderer must be named DesktopCompanion.exe.')
    del arguments[index:index + 2]
    review_motion.EXE = gate.EXE = executable
    review_motion.BUILD = gate.BUILD = executable.parent
    original = gate.shared_inputs

    def shared_inputs(fixture, previous_tool=None):
        return original(fixture, previous_tool) + gate.file_snapshot([Path(__file__)])

    gate.shared_inputs = shared_inputs
    sys.argv = [sys.argv[0]] + arguments
    return gate.main()


if __name__ == '__main__':
    sys.exit(main())
