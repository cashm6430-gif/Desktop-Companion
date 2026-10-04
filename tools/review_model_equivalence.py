"""Capture and compare whole-model Native frames in isolated directories.

The comparison requires identical renderer, motion inputs, parameter traces,
and decoded RGBA pixels. It never adopts a model or grants visual approval.
Raw export captures are static; full captures also exercise every motion and
the real interaction player. Use the same fixture for both model captures.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image

from review_interactions import SCENARIOS, check_scene
from review_motion import BUILD, EXE, available_clips, profile, run_exe
from review_neck import (file_snapshot, finite_parameters, model_inputs,
                         verify_capture, verify_snapshot)
from review_session import digest


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def shared_inputs(fixture, previous_tool=None):
    paths = [EXE, fixture]
    paths += list(BUILD.glob('*.dll')) + list((BUILD / 'platforms').glob('*.dll'))
    paths += list((BUILD / 'assets/motions').glob('*.motion.json'))
    paths += list((Path(__file__).resolve().parents[1] / 'assets/motions').glob('*.motion.json'))
    paths += [Path(__file__), Path(__file__).with_name('review_motion.py'),
              Path(__file__).with_name('review_neck.py'),
              Path(__file__).with_name('review_interactions.py'),
              Path(__file__).with_name('probe_cubism_core.py')]
    if previous_tool:
        paths.append(previous_tool)
    if not (BUILD / 'Live2DCubismCore.dll').is_file():
        raise ValueError('Missing pinned Cubism Core.')
    return file_snapshot(paths)


def capture_plan(fixture, static_only=False, raw=False):
    if raw and not static_only:
        raise ValueError('Raw capture requires --static-only.')
    count = len(read(fixture)['keyframes'])
    plan = [dict(kind='static', name='fixture', folder='static',
                 frames=[f'pose-{index:02}.png' for index in range(count)])]
    if not static_only:
        for clip in available_clips():
            spec = profile(clip)
            plan.append(dict(kind='motion', name=spec['render'], folder='motions/' + clip,
                             frames=[f'frame-{index:03}.png' for index in range(spec['frames'])]))
        for scenario in SCENARIOS:
            count = 180 if scenario.startswith('grass-') else 75
            plan.append(dict(kind='interaction', name=scenario, folder='interactions/' + scenario,
                             frames=[f'frame-{index:03}.png' for index in range(count)]))
    return plan


def capture(model, fixture, output, static_only=False, raw=False, resume=False, previous_tool=None):
    model, fixture, output = model.resolve(), fixture.resolve(), output.resolve()
    if raw and not static_only:
        raise ValueError('Raw capture requires --static-only.')
    keys = read(fixture).get('keyframes')
    if not isinstance(keys, list) or not keys or any(
            not isinstance(row, dict) or not finite_parameters(row.get('parameters')) for row in keys):
        raise ValueError('Fixture must contain finite keyframe parameters.')
    if output.exists() and not resume:
        raise ValueError('Use a new output directory; existing evidence is never overwritten.')
    if resume and not output.is_dir():
        raise ValueError('--resume requires a failed capture directory.')
    previous_tool = previous_tool.resolve() if previous_tool else None
    inputs, shared = model_inputs(model, raw), shared_inputs(fixture, previous_tool)
    resume_history = []
    receipt_path = output / 'capture-receipt.json'
    if resume:
        old = read(receipt_path)
        if (old.get('status') != 'failed' or old.get('model') != str(model)
                or old.get('fixture') != str(fixture) or old.get('raw_export_only') != raw
                or old.get('scope') != ('static' if static_only else 'full')
                or old.get('model_inputs') != inputs):
            raise ValueError('Failed capture scope/model/fixture changed; cannot resume.')
        # Only an explicitly archived capture-tool version may change. Every
        # renderer, dependency, fixture and motion input must still be pinned.
        old_tool = [row for row in old['shared_inputs'] if Path(row['path']).resolve() == Path(__file__).resolve()]
        if len(old_tool) != 1:
            raise ValueError('Previous capture lacks a unique tool provenance.')
        verify_snapshot([row for row in old['shared_inputs'] if row not in old_tool])
        if digest(Path(__file__)) != old_tool[0]['sha256']:
            if previous_tool is None or digest(previous_tool) != old_tool[0]['sha256']:
                raise ValueError('Changed capture tool requires its exact --previous-tool archive.')
        for sequence in old.get('sequences', []):
            verify_snapshot(sequence['captured_files'])
        archive = output / ('previous-receipt-' + digest(receipt_path)[:16] + '.json')
        if archive.exists():
            raise ValueError('Previous receipt archive already exists.')
        shutil.copyfile(receipt_path, archive)
        resume_history = old.get('resume_history', []) + [dict(
            receipt=file_snapshot([archive])[0], previous_tool_sha256=old_tool[0]['sha256'],
            current_tool_sha256=digest(Path(__file__)),
            reason='revalidate complete captures and continue after capture validation failure')]
    else:
        output.mkdir(parents=True)
    receipt = dict(version=1, created_utc=datetime.now(timezone.utc).isoformat(),
                   model=str(model), fixture=str(fixture), raw_export_only=raw,
                   scope='static' if static_only else 'full', model_inputs=inputs,
                   shared_inputs=shared, status='running', approval='pending',
                   adoption='not_requested', sequences=[], resume_history=resume_history,
                   plan=capture_plan(fixture, static_only, raw))
    write(receipt_path, receipt)

    def record(kind, name, folder, count, evidence):
        verify_capture(evidence['capture'] if kind == 'interaction' else evidence,
                       model, raw, True, static=kind == 'static')
        pattern = 'pose-*.png' if kind == 'static' else 'frame-*.png'
        names = ([f'pose-{index:02}.png' for index in range(count)] if kind == 'static'
                 else [f'frame-{index:03}.png' for index in range(count)])
        if sorted(path.name for path in folder.glob(pattern)) != sorted(names):
            raise ValueError('Missing or extra frames: ' + name)
        trace = evidence.get('captures' if kind == 'static' else 'frames', [])
        if len(trace) != count:
            raise ValueError('Incomplete Native trace: ' + name)
        if kind == 'static':
            for index, row in enumerate(trace):
                if (row.get('source_keyframe') != keys[index] or row.get('index') != index
                        or row.get('frame_saved') is not True or row.get('file') != names[index]):
                    raise ValueError('Static source mismatch: ' + name)
        elif kind == 'motion':
            if (evidence.get('scope') != 'sampled_motion_player_native_capture'
                    or evidence.get('clip') != name or evidence.get('physics_evaluated') is not True):
                raise ValueError('Motion trace mismatch: ' + name)
        rows = []
        for filename in names:
            path = folder / filename
            with Image.open(path) as picture:
                rgba = picture.convert('RGBA')
                if rgba.getchannel('A').getbbox() is None:
                    raise ValueError('Empty Native frame: ' + str(path))
            rows.append(filename)
        receipt['sequences'].append(dict(kind=kind, name=name,
                                        folder=folder.relative_to(output).as_posix(),
                                        frames=rows, trace=trace,
                                        captured_files=file_snapshot([folder / filename for filename in names])))
        verify_snapshot(inputs + shared)
        write(receipt_path, receipt)

    try:
        folder = output / 'static'
        args = ['--review-motion', folder, fixture, 'pose', '--review-model', model]
        if raw:
            args.append('--review-raw-export')
        if not folder.exists():
            run_exe(args, watch=(folder, len(keys), 'static frames'))
        elif not resume:
            raise ValueError('Existing static folder is not an authorized resume.')
        record('static', 'fixture', folder, len(keys), read(folder / 'pose.capture.json'))
        if not static_only:
            for clip in available_clips():
                spec = profile(clip)
                folder = output / 'motions' / clip
                if not folder.exists():
                    run_exe(['--render-motion', folder, spec['render'], '--review-model', model],
                            watch=(folder, spec['frames'], 'motion frames'))
                elif not resume:
                    raise ValueError('Existing motion folder is not an authorized resume.')
                record('motion', spec['render'], folder, spec['frames'], read(folder / 'capture.json'))
            for scenario in SCENARIOS:
                folder = output / 'interactions' / scenario
                count = 180 if scenario.startswith('grass-') else 75
                if not folder.exists():
                    run_exe(['--render-interaction', folder, scenario, '--review-model', model],
                            watch=(folder, count, 'interaction frames'))
                elif not resume:
                    raise ValueError('Existing interaction folder is not an authorized resume.')
                check_scene(folder / 'scene.json', scenario)
                record('interaction', scenario, folder, count, read(folder / 'scene.json'))
        receipt.update(status='complete', total_frames=sum(len(s['frames']) for s in receipt['sequences']))
        write(receipt_path, receipt)
        print(f"Complete: {receipt['total_frames']} Native frames; {receipt_path}", flush=True)
    except BaseException as error:
        receipt.update(status='failed', error=str(error))
        write(receipt_path, receipt)
        raise


def load_capture(folder):
    folder = folder.resolve()
    receipt = read(folder / 'capture-receipt.json')
    if receipt.get('status') != 'complete':
        raise ValueError('Capture is incomplete: ' + str(folder))
    verify_snapshot(receipt['model_inputs'] + receipt['shared_inputs'])
    if receipt.get('scope') not in ('static', 'full'):
        raise ValueError('Unknown capture scope.')
    expected = capture_plan(Path(receipt['fixture']), receipt['scope'] == 'static', receipt['raw_export_only'])
    inventory = [{key: sequence[key] for key in ('kind', 'name', 'folder', 'frames')}
                 for sequence in receipt['sequences']]
    if receipt.get('plan') != expected or inventory != expected:
        raise ValueError('Capture sequence inventory is incomplete or differs from its full plan.')
    if receipt.get('total_frames') != sum(len(sequence['frames']) for sequence in expected):
        raise ValueError('Capture total_frames differs from its full plan.')
    for sequence in receipt['sequences']:
        verify_snapshot(sequence['captured_files'])
    return folder, receipt


def compare(reference, candidate, output):
    reference, before = load_capture(reference)
    candidate, after = load_capture(candidate)
    output = output.resolve()
    if output.exists():
        raise ValueError('Use a new comparison file.')
    shared_equal = before['shared_inputs'] == after['shared_inputs']
    scope_equal = all(before.get(key) == after.get(key) for key in ('scope', 'raw_export_only', 'fixture'))
    inventory = lambda value: [(s['kind'], s['name'], s['frames']) for s in value['sequences']]
    inventory_equal = inventory(before) == inventory(after)
    report = dict(version=1, reference=str(reference), candidate=str(candidate),
                  shared_inputs_equal=shared_equal, scope_equal=scope_equal,
                  sequence_inventory_equal=inventory_equal, pixel_rule='decoded RGBA must be identical',
                  approval='pending', adoption='not_requested', frames_compared=0,
                  unequal_frames=0, changed_pixels=0, trace_differences=[], frame_differences=[])
    if shared_equal and scope_equal and inventory_equal:
        for left, right in zip(before['sequences'], after['sequences']):
            if left['trace'] != right['trace']:
                report['trace_differences'].append(left['kind'] + '/' + left['name'])
            for filename in left['frames']:
                with Image.open(reference / left['folder'] / filename) as picture:
                    a = np.asarray(picture.convert('RGBA')).copy()
                with Image.open(candidate / right['folder'] / filename) as picture:
                    b = np.asarray(picture.convert('RGBA')).copy()
                report['frames_compared'] += 1
                if a.shape != b.shape:
                    difference = dict(frame=left['folder'] + '/' + filename,
                                      size_mismatch=[list(a.shape), list(b.shape)])
                else:
                    mask = np.any(a != b, axis=2)
                    count = int(np.count_nonzero(mask))
                    if not count:
                        continue
                    yy, xx = np.nonzero(mask)
                    delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
                    difference = dict(frame=left['folder'] + '/' + filename, changed_pixels=count,
                                      max_channel_delta=int(delta.max()),
                                      bbox=[int(xx.min()), int(yy.min()), int(xx.max() + 1), int(yy.max() + 1)])
                    report['changed_pixels'] += count
                report['unequal_frames'] += 1
                report['frame_differences'].append(difference)
    report['equivalent'] = (shared_equal and scope_equal and inventory_equal
                            and not report['trace_differences'] and report['unequal_frames'] == 0
                            and report['frames_compared'] > 0)
    output.parent.mkdir(parents=True, exist_ok=True)
    write(output, report)
    print(json.dumps({key: report[key] for key in ('equivalent', 'frames_compared', 'unequal_frames',
                                                 'changed_pixels', 'trace_differences')}, ensure_ascii=False))
    return report['equivalent']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    capture_args = sub.add_parser('capture')
    capture_args.add_argument('--model', type=Path, required=True)
    capture_args.add_argument('--fixture', type=Path, required=True)
    capture_args.add_argument('--output', type=Path, required=True)
    capture_args.add_argument('--static-only', action='store_true')
    capture_args.add_argument('--raw', action='store_true')
    capture_args.add_argument('--resume', action='store_true', help='Revalidate and continue a failed capture; never overwrite PNGs.')
    capture_args.add_argument('--previous-tool', type=Path, help='Exact archived tool source, required if it changed since the failed capture.')
    compare_args = sub.add_parser('compare')
    compare_args.add_argument('--reference', type=Path, required=True)
    compare_args.add_argument('--candidate', type=Path, required=True)
    compare_args.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'capture':
            capture(args.model, args.fixture, args.output, args.static_only, args.raw,
                    args.resume, args.previous_tool)
        elif not compare(args.reference, args.candidate, args.output):
            return 1
    except (ValueError, OSError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
