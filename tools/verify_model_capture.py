"""Read-only verification of a saved whole-model capture receipt and PNGs.

This checks source hashes, the complete capture plan and exact run-local
artifact/hash binding. It does not run Native, compare models, review images
for aesthetics, adopt a model, or change a motion approval.

    python tools/verify_model_capture.py <capture-directory>

The JSON on stdout pins the receipt version/hash and this verifier's hash.
Save it with the comparison bundle if a durable verification record is needed.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import stat
import sys

from PIL import Image

from review_model_equivalence import capture_plan
from review_neck import verify_snapshot


class CaptureVerificationError(ValueError):
    pass


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def reject_reparse_path(path):
    """Refuse any symlink/junction/reparse ancestor before resolving it."""
    path = Path(path).absolute()
    for component in reversed((path, *path.parents)):
        try:
            attributes = component.lstat()
        except OSError as error:
            raise CaptureVerificationError('Missing or unreadable path: ' + str(component)) from error
        if (stat.S_ISLNK(attributes.st_mode)
                or getattr(attributes, 'st_file_attributes', 0)
                & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)):
            raise CaptureVerificationError('Symlink, junction or reparse path is forbidden: ' + str(component))


def absolute_record_path(value, label):
    if not isinstance(value, str) or not value or not Path(value).is_absolute():
        raise CaptureVerificationError(label + ' requires an absolute path.')
    reject_reparse_path(value)
    return Path(value).resolve(strict=True)


def validate_records(records, label):
    if not isinstance(records, list) or not records:
        raise CaptureVerificationError(label + ' must contain nonempty hash records.')
    paths = set()
    for record in records:
        if not isinstance(record, dict):
            raise CaptureVerificationError(label + ' contains an invalid hash record.')
        path = absolute_record_path(record.get('path'), label)
        if path in paths:
            raise CaptureVerificationError(label + ' contains a duplicate resolved path: ' + str(path))
        paths.add(path)
        expected = record.get('sha256')
        if (not isinstance(expected, str) or len(expected) != 64
                or any(c not in '0123456789abcdef' for c in expected)):
            raise CaptureVerificationError(label + ' requires a lowercase SHA256: ' + str(path))
        size = record.get('bytes')
        if isinstance(size, bool) or not isinstance(size, int) or size < 0 or path.stat().st_size != size:
            raise CaptureVerificationError(label + ' byte count differs: ' + str(path))
    verify_snapshot(records)
    return paths


def verify_capture_binding(directory):
    tool_hash = digest(Path(__file__))
    original = Path(directory).absolute()
    reject_reparse_path(original)
    run = original.resolve(strict=True)
    if not run.is_dir():
        raise CaptureVerificationError('Capture path is not a directory.')
    receipt_path = run / 'capture-receipt.json'
    reject_reparse_path(receipt_path)
    raw_receipt = receipt_path.read_bytes()
    receipt_hash = hashlib.sha256(raw_receipt).hexdigest()
    receipt = json.loads(raw_receipt.decode('utf-8-sig'))
    if (not isinstance(receipt, dict) or isinstance(receipt.get('version'), bool)
            or receipt.get('version') != 1):
        raise CaptureVerificationError('Unsupported or missing capture receipt version.')
    if receipt.get('status') != 'complete':
        raise CaptureVerificationError('Only a complete saved capture can be verified.')
    scope, raw = receipt.get('scope'), receipt.get('raw_export_only')
    if scope not in ('static', 'full') or not isinstance(raw, bool):
        raise CaptureVerificationError('Unknown capture scope or raw_export_only flag.')
    if raw and scope != 'static':
        raise CaptureVerificationError('A raw export capture must be static.')

    model_paths = validate_records(receipt.get('model_inputs'), 'model_inputs')
    shared_paths = validate_records(receipt.get('shared_inputs'), 'shared_inputs')
    model = absolute_record_path(receipt.get('model'), 'model')
    fixture = absolute_record_path(receipt.get('fixture'), 'fixture')
    if model not in model_paths or fixture not in shared_paths:
        raise CaptureVerificationError('The declared model/fixture is not pinned in its input records.')
    if not {'DesktopCompanion.exe', 'Live2DCubismCore.dll', 'review_model_equivalence.py'} <= {
            path.name for path in shared_paths}:
        raise CaptureVerificationError('Missing pinned renderer, Cubism Core or capture tool input.')

    expected = capture_plan(fixture, scope == 'static', raw)
    sequences = receipt.get('sequences')
    if not isinstance(sequences, list) or not sequences:
        raise CaptureVerificationError('Capture sequences are empty or missing.')
    try:
        inventory = [{key: sequence[key] for key in ('kind', 'name', 'folder', 'frames')}
                     for sequence in sequences]
    except (KeyError, TypeError) as error:
        raise CaptureVerificationError('Capture sequence inventory is malformed.') from error
    if receipt.get('plan') != expected or inventory != expected:
        raise CaptureVerificationError('Capture sequence inventory does not match its complete recomputed plan.')
    expected_total = sum(len(sequence['frames']) for sequence in expected)
    if (isinstance(receipt.get('total_frames'), bool)
            or receipt.get('total_frames') != expected_total or expected_total <= 0):
        raise CaptureVerificationError('Capture total_frames differs from its recomputed plan.')

    all_frames, summaries = set(), []
    for sequence in sequences:
        relative = Path(sequence['folder'])
        if relative.is_absolute() or '..' in relative.parts:
            raise CaptureVerificationError('Sequence folder must stay within this run.')
        folder = run / relative
        reject_reparse_path(folder)
        folder = folder.resolve(strict=True)
        if not folder.is_relative_to(run):
            raise CaptureVerificationError('Sequence folder escaped this run.')
        frames = sequence['frames']
        trace = sequence.get('trace')
        if not isinstance(trace, list) or len(trace) != len(frames):
            raise CaptureVerificationError('Trace count differs from the planned frame count: ' + sequence['name'])
        expected_paths = set()
        for filename in frames:
            if not isinstance(filename, str) or Path(filename).name != filename or not filename.endswith('.png'):
                raise CaptureVerificationError('Frame inventory must contain simple PNG filenames.')
            path = folder / filename
            reject_reparse_path(path)
            path = path.resolve(strict=True)
            if not path.is_relative_to(run) or path in expected_paths or path in all_frames:
                raise CaptureVerificationError('Foreign or duplicate frame in capture inventory: ' + str(path))
            expected_paths.add(path)
        records = sequence.get('captured_files')
        recorded_paths = validate_records(records, 'captured_files/' + sequence['name'])
        if recorded_paths != expected_paths:
            raise CaptureVerificationError('Captured PNG hashes are not bound one-to-one to folder/frames: ' + sequence['name'])
        # Interaction folders may also contain the separately named diagnostic
        # PNG. Only the planned frame/pose inventory is subject to exact count.
        pattern = 'pose-*.png' if sequence['kind'] == 'static' else 'frame-*.png'
        if {path.resolve(strict=True) for path in folder.glob(pattern)} != expected_paths:
            raise CaptureVerificationError('Missing or extra planned capture PNGs: ' + sequence['name'])
        for path in sorted(expected_paths):
            with Image.open(path) as image:
                if image.format != 'PNG' or image.width <= 0 or image.height <= 0:
                    raise CaptureVerificationError('Capture frame is not a readable positive-size PNG: ' + str(path))
                image.load()
                if image.convert('RGBA').getchannel('A').getbbox() is None:
                    raise CaptureVerificationError('Capture PNG is visually empty: ' + str(path))
        all_frames.update(expected_paths)
        summaries.append({'kind': sequence['kind'], 'name': sequence['name'],
                          'folder': sequence['folder'], 'frames': len(frames),
                          'trace_rows': len(trace), 'png_hashes': len(records)})

    # Pin the saved version for this exact verification, and reject inputs or
    # output PNGs being changed while the read-only audit is running.
    verify_snapshot(receipt['model_inputs'] + receipt['shared_inputs'])
    for sequence in sequences:
        verify_snapshot(sequence['captured_files'])
    if digest(receipt_path) != receipt_hash:
        raise CaptureVerificationError('Saved receipt changed during verification.')
    if digest(Path(__file__)) != tool_hash:
        raise CaptureVerificationError('Verifier tool changed during verification.')
    return {
        'schema_version': 1, 'kind': 'strict_saved_capture_binding_verification',
        'status': 'passed', 'created_utc': datetime.now(timezone.utc).isoformat(),
        'scope': 'saved receipt, pinned sources and planned decoded PNGs only',
        'capture_scope': scope, 'raw_export_only': raw,
        'run': str(run), 'receipt_version': receipt['version'], 'receipt_sha256': receipt_hash,
        'verifier_tool_sha256': tool_hash,
        'planned_motion_count': sum(item['kind'] == 'motion' for item in expected),
        'planned_interaction_count': sum(item['kind'] == 'interaction' for item in expected),
        'sequence_count': len(sequences), 'total_frames': len(all_frames), 'sequences': summaries,
        'native_rendering_rerun': False, 'model_equivalence_tested': False,
        'visual_approval': 'pending', 'adoption': 'not_requested',
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args(argv)
    try:
        report = verify_capture_binding(args.capture)
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(2, str(error) + '\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
