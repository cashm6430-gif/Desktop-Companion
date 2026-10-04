"""Compare two unpatched Cubism Core reports against still-current inputs.

python tools/compare_cubism_probes.py --before before.core.json \
    --after after.core.json --output comparison.json

MOC/CMO3/JSON byte changes are provenance, not semantic failure by themselves.
Native IDs, ranges, parenting, UV/index buffers, texture pixels, opacity and
posed vertices are compared. Unselected changed vertex buffers are incomplete
evidence rather than a pass. This tool neither calls Core nor exports/renders.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile

MAX_REPORT_BYTES = 64 * 1024 * 1024
MAX_VERTEX_SAMPLES = 200_000
MAX_ATLAS_PIXELS = 32_000_000


class ComparisonError(ValueError):
    pass


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def snapshot(path):
    path = Path(path).resolve()
    if path.stat().st_size > MAX_REPORT_BYTES:
        raise ComparisonError(f'JSON input exceeds 64 MiB: {path}')
    raw = path.read_bytes()
    return json.loads(raw.decode('utf-8-sig')), hashlib.sha256(raw).hexdigest()


def verify(hashes):
    for path, expected in hashes.items():
        if not Path(path).is_file() or digest(path) != expected:
            raise ComparisonError(f'Current input missing or changed: {path}')


def numeric(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ComparisonError('Probe contains a non-finite or non-numeric value.')
    return float(value)


def buffer_hash(values, code, byte_order):
    prefix = '<' if byte_order == 'little' else '>'
    value = hashlib.sha256()
    for row in values:
        items = row if isinstance(row, list) else [row]
        for item in items:
            numeric(item)
        value.update(struct.pack(prefix + code * len(items), *items))
    return value.hexdigest()


def read_probe(path):
    path = Path(path).resolve()
    report, report_hash = snapshot(path)
    if (report.get('schema_version') != 1 or report.get('kind') != 'cubism_core_export_probe'
            or report.get('status') != 'complete' or not report.get('input_hashes_verified_after_probe')
            or not report.get('scope', {}).get('raw_export_only')
            or report['scope'].get('cubism_canvas_used') is not False
            or report['scope'].get('physics_evaluated') is not False):
        raise ComparisonError('A complete unpatched schema 1 Core probe is required.')
    hashes = {str(path): report_hash}
    roles = {}
    for row in report['model_inputs']:
        local = str(Path(row['path']).resolve())
        if local in hashes and hashes[local] != row['sha256']:
            raise ComparisonError('Conflicting input hashes in one report.')
        hashes[local] = row['sha256']
        for role in row['roles']:
            if role in roles:
                raise ComparisonError(f'Duplicate model input role: {role}')
            roles[role] = {'path': local, 'sha256': row['sha256']}
    if ('Moc' not in roles or 'model3' not in roles
            or report['model_moc_sha256'] != roles['Moc']['sha256']
            or str(Path(report['model']).resolve()) != roles['model3']['path']):
        raise ComparisonError('Model/MOC provenance is inconsistent.')
    for field in ('cubism_core', 'pose_input'):
        record = report[field]
        hashes[str(Path(record['path']).resolve())] = record['sha256']
    probe_tool = Path(__file__).with_name('probe_cubism_core.py').resolve()
    hashes[str(probe_tool)] = report['probe_tool_sha256']
    verify(hashes)
    model, model_hash = snapshot(roles['model3']['path'])
    if model_hash != roles['model3']['sha256']:
        raise ComparisonError('Model JSON changed during comparison.')
    refs = model['FileReferences']
    linked = {'Moc': refs['Moc'], **{f'Textures[{i}]': name for i, name in enumerate(refs.get('Textures', []))}}
    for role, relative in linked.items():
        target = str((Path(roles['model3']['path']).parent / relative).resolve())
        if role not in roles or roles[role]['path'] != target:
            raise ComparisonError(f'Model references disagree with Core report: {role}')
    poses, poses_hash = snapshot(report['pose_input']['path'])
    if poses_hash != report['pose_input']['sha256']:
        raise ComparisonError('Pose JSON changed during comparison.')
    # Extra analytical provenance in this project's pose documents is also
    # current evidence. It never changes the Native pose values.
    if isinstance(poses, dict):
        hashes.update({str(Path(key).resolve()): value for key, value in poses.get('source_inputs', {}).items()})
        verify(hashes)
    requested = poses.get('poses') if isinstance(poses, dict) else poses
    if not isinstance(requested, list) or not requested:
        raise ComparisonError('Pose document needs a nonempty pose list.')
    names = [row.get('name', f'pose-{i:03d}') for i, row in enumerate(requested)]
    if len(set(names)) != len(names) or names != [row['name'] for row in report['poses']]:
        raise ComparisonError('Reported poses do not match the current pose document.')
    topology = report['topology']
    if report['drawable_count'] != len(topology) or report['parameter_count'] != len(report['parameters']):
        raise ComparisonError('Native counts disagree with report IDs.')
    encoding = report['array_hash_encoding']
    if encoding != {'vertices_uv': 'float32', 'indices': 'uint16', 'byte_order': encoding.get('byte_order')} or encoding.get('byte_order') not in ('little', 'big'):
        raise ComparisonError('Unsupported Native buffer encoding.')
    selected = set(report['selected_drawables'])
    if not selected or selected - set(topology) or len(selected) != len(report['selected_drawables']):
        raise ComparisonError('Invalid raw drawable selection.')
    samples = sum(int(topology[name]['vertex_count']) for name in selected) * len(report['poses'])
    if samples > MAX_VERTEX_SAMPLES:
        raise ComparisonError('Report raw geometry exceeds 200000 vertex samples.')
    for name in selected:
        info = topology[name]
        if len(info['uvs']) != info['vertex_count'] or len(info['indices']) != info['index_count']:
            raise ComparisonError(f'Incomplete selected topology: {name}')
        if (buffer_hash(info['uvs'], 'f', encoding['byte_order']) != info['uvs_sha256']
                or buffer_hash(info['indices'], 'H', encoding['byte_order']) != info['indices_sha256']):
            raise ComparisonError(f'Selected topology array/hash mismatch: {name}')
    for request, pose in zip(requested, report['poses']):
        if set(pose['parameters']) != set(report['parameters']) or set(pose['drawables']) != set(topology) or set(pose['part_opacities']) != set(report['parts']):
            raise ComparisonError(f'Incomplete Native pose: {pose["name"]}')
        for name, bounds in report['parameters'].items():
            values = pose['parameters'][name]
            raw = request.get('parameters', {}).get(name, bounds['default'])
            clamped = min(bounds['max'], max(bounds['min'], raw))
            if numeric(values['raw']) != numeric(raw) or numeric(values['clamped']) != numeric(clamped):
                raise ComparisonError(f'Pose request/clamping differs from current input: {pose["name"]}.{name}')
        for name in selected:
            record = pose['drawables'][name]
            vertices = record['vertices']
            if len(vertices) != topology[name]['vertex_count'] or any(len(row) != 2 for row in vertices):
                raise ComparisonError(f'Incomplete selected vertices: {pose["name"]}.{name}')
            if buffer_hash(vertices, 'f', encoding['byte_order']) != record['vertices_sha256']:
                raise ComparisonError(f'Vertex array/hash mismatch: {pose["name"]}.{name}')
    return report, hashes, roles


def texture_evidence(roles):
    result = {}
    for role, record in roles.items():
        match = re.fullmatch(r'Textures\[(\d+)\]', role)
        if not match:
            continue
        evidence = {'path': record['path'], 'file_sha256': record['sha256'], 'rgba_sha256': None}
        try:
            from PIL import Image
        except ImportError:
            evidence['pixel_verification_unavailable'] = 'Pillow is absent; no software installed.'
        else:
            with Image.open(record['path']) as image:
                if image.width * image.height > MAX_ATLAS_PIXELS:
                    raise ComparisonError(f'Atlas exceeds pixel comparison budget: {record["path"]}')
                evidence['size'] = list(image.size)
                with image.convert('RGBA') as rgba:
                    evidence['rgba_sha256'] = hashlib.sha256(rgba.tobytes()).hexdigest()
        result[int(match.group(1))] = evidence
    return result


def parent_id(parts, record):
    index = record['parent_index']
    if index == -1:
        return None
    if not 0 <= index < len(parts):
        raise ComparisonError('Native part has an invalid parent index.')
    return list(parts)[index]


def mask_ids(topology, record):
    by_index = {value['index']: name for name, value in topology.items()}
    if len(by_index) != len(topology) or any(index not in by_index for index in record['masks']):
        raise ComparisonError('Native drawable mask/index mapping is invalid.')
    return [by_index[index] for index in record['masks']]


def compare_reports(before, after, before_roles, after_roles, geometry_tolerance_px=0.01, opacity_tolerance=1e-6):
    differences, incomplete, rows = [], [], []

    def changed(category, item, field, left, right, tolerance=0):
        equal = abs(numeric(left) - numeric(right)) <= tolerance if tolerance else left == right
        if not equal:
            differences.append({'category': category, 'item': item, 'field': field, 'before': left, 'after': right})

    def ids(category, left, right):
        changed(category, 'IDs', 'removed', sorted(set(left) - set(right)), [])
        changed(category, 'IDs', 'added', [], sorted(set(right) - set(left)))
        return sorted(set(left) & set(right))

    if before['pose_input']['sha256'] != after['pose_input']['sha256']:
        raise ComparisonError('Before/after must use the identical current pose JSON.')
    if before['cubism_core']['sha256'] != after['cubism_core']['sha256']:
        raise ComparisonError('Before/after must use the same Cubism Core binary.')
    if before['array_hash_encoding'] != after['array_hash_encoding']:
        raise ComparisonError('Before/after buffer encodings differ.')
    if geometry_tolerance_px < 0 or opacity_tolerance < 0:
        raise ComparisonError('Comparison tolerances must be nonnegative.')
    numeric(geometry_tolerance_px)
    numeric(opacity_tolerance)
    changed('canvas', 'model', 'canvas', before['canvas'], after['canvas'])
    scale = numeric(before['canvas']['pixels_per_unit'])
    if scale <= 0:
        raise ComparisonError('Canvas pixel scale must be positive.')
    parameter_ids = ids('parameters', before['parameters'], after['parameters'])
    for name in parameter_ids:
        for field in ('min', 'max', 'default'):
            changed('parameters', name, field, before['parameters'][name][field], after['parameters'][name][field], 1e-6)
        for field in ('type', 'default_in_range'):
            changed('parameters', name, field, before['parameters'][name][field], after['parameters'][name][field])
    part_ids = ids('parts', before['parts'], after['parts'])
    for name in part_ids:
        changed('parts', name, 'parent_id', parent_id(before['parts'], before['parts'][name]), parent_id(after['parts'], after['parts'][name]))
        changed('parts', name, 'default_opacity', before['parts'][name]['default_opacity'], after['parts'][name]['default_opacity'], opacity_tolerance)
    drawable_ids = ids('drawables', before['topology'], after['topology'])
    for name in drawable_ids:
        left, right = before['topology'][name], after['topology'][name]
        for field in ('vertex_count', 'index_count', 'uvs_sha256', 'indices_sha256', 'texture_index', 'parent_part', 'constant_flags'):
            changed('topology', name, field, left[field], right[field])
        changed('topology', name, 'mask_ids', mask_ids(before['topology'], left), mask_ids(after['topology'], right))
        changed('topology', name, 'default_opacity', left['default_opacity'], right['default_opacity'], opacity_tolerance)
    left_textures, right_textures = texture_evidence(before_roles), texture_evidence(after_roles)
    texture_ids = ids('textures', left_textures, right_textures)
    for index in texture_ids:
        left, right = left_textures[index], right_textures[index]
        if left['rgba_sha256'] is not None and right['rgba_sha256'] is not None:
            changed('textures', index, 'pixel_size', left['size'], right['size'])
            changed('textures', index, 'rgba_sha256', left['rgba_sha256'], right['rgba_sha256'])
        elif left['file_sha256'] != right['file_sha256']:
            incomplete.append({'category': 'textures', 'item': index, 'reason': 'Atlas bytes changed; decoded pixel comparison is unavailable.'})
    for side, report, textures in (('before', before, left_textures), ('after', after, right_textures)):
        if any(info['texture_index'] not in textures for info in report['topology'].values()):
            raise ComparisonError(f'{side} drawable has an unbound texture index.')
    left_poses = {pose['name']: pose for pose in before['poses']}
    right_poses = {pose['name']: pose for pose in after['poses']}
    pose_ids = ids('poses', left_poses, right_poses)
    for pose_name in pose_ids:
        left_pose, right_pose = left_poses[pose_name], right_poses[pose_name]
        for name in parameter_ids:
            left, right = left_pose['parameters'][name], right_pose['parameters'][name]
            for field in ('raw', 'clamped', 'native_input', 'native_value'):
                changed('pose_parameters', f'{pose_name}.{name}', field, left[field], right[field], 1e-6)
            for field in ('specified', 'was_clamped'):
                changed('pose_parameters', f'{pose_name}.{name}', field, left[field], right[field])
        for name in part_ids:
            changed('pose_parts', f'{pose_name}.{name}', 'opacity', left_pose['part_opacities'][name], right_pose['part_opacities'][name], opacity_tolerance)
        for name in drawable_ids:
            left, right = left_pose['drawables'][name], right_pose['drawables'][name]
            opacity_error = abs(numeric(left['opacity']) - numeric(right['opacity']))
            changed('pose_drawables', f'{pose_name}.{name}', 'opacity', left['opacity'], right['opacity'], opacity_tolerance)
            for field in ('visible_flag', 'draw_order', 'render_order'):
                changed('pose_drawables', f'{pose_name}.{name}', field, left[field], right[field])
            row = {'pose': pose_name, 'drawable': name, 'opacity_error': opacity_error,
                   'vertex_hash_equal': left['vertices_sha256'] == right['vertices_sha256']}
            if 'vertices' in left and 'vertices' in right and len(left['vertices']) == len(right['vertices']):
                squared = [(numeric(a[0]) - numeric(b[0])) ** 2 + (numeric(a[1]) - numeric(b[1])) ** 2
                           for a, b in zip(left['vertices'], right['vertices'])]
                worst = max(range(len(squared)), key=squared.__getitem__) if squared else None
                row.update(geometry_evidence='raw_vertices', max_error_px=math.sqrt(max(squared, default=0)) * scale,
                           rms_error_px=math.sqrt(sum(squared) / len(squared)) * scale if squared else 0,
                           worst_vertex_index=worst)
                if row['max_error_px'] > geometry_tolerance_px:
                    differences.append({'category': 'pose_geometry', 'item': f'{pose_name}.{name}',
                                        'field': 'max_error_px', 'before': 0, 'after': row['max_error_px']})
            elif row['vertex_hash_equal']:
                row.update(geometry_evidence='identical_float32_buffer_hash', max_error_px=0, rms_error_px=0)
            elif 'vertices' in left and 'vertices' in right:
                before_count, after_count = len(left['vertices']), len(right['vertices'])
                row.update(geometry_evidence='topology_vertex_count_mismatch', max_error_px=None, rms_error_px=None,
                           before_vertex_count=before_count, after_vertex_count=after_count)
                incomplete.append({'category': 'pose_geometry', 'item': f'{pose_name}.{name}',
                                   'reason_code': 'topology_vertex_count_mismatch',
                                   'before_vertex_count': before_count, 'after_vertex_count': after_count,
                                   'reason': 'Both raw vertex arrays are complete, but topology vertex counts differ; index-by-index geometry comparison is undefined.'})
            else:
                row.update(geometry_evidence='unresolved_changed_buffer', max_error_px=None, rms_error_px=None)
                incomplete.append({'category': 'pose_geometry', 'item': f'{pose_name}.{name}',
                                   'reason': 'Vertex buffers changed without matching full raw arrays; select this drawable in both probes.'})
            rows.append(row)
    file_changes = [{'role': role, 'before': before_roles.get(role), 'after': after_roles.get(role)}
                    for role in sorted(set(before_roles) | set(after_roles))
                    if before_roles.get(role, {}).get('sha256') != after_roles.get(role, {}).get('sha256')]
    status = 'changed' if differences else 'incomplete' if incomplete else 'equivalent_within_tolerance'
    return {'status': status, 'tolerances': {'geometry_source_pixels': geometry_tolerance_px, 'opacity': opacity_tolerance, 'parameter_values': 1e-6},
            'summary': {'parameter_ids_compared': len(parameter_ids), 'part_ids_compared': len(part_ids),
                        'drawable_ids_compared': len(drawable_ids), 'poses_compared': len(pose_ids),
                        'difference_count': len(differences), 'incomplete_count': len(incomplete),
                        'max_geometry_error_px': max((row['max_error_px'] for row in rows if row['max_error_px'] is not None), default=0),
                        'max_opacity_error': max((row['opacity_error'] for row in rows), default=0)},
            'differences': differences, 'incomplete': incomplete, 'geometry_and_opacity': rows,
            'file_byte_changes_are_not_semantic_failure': file_changes,
            'textures': {'before': left_textures, 'after': right_textures},
            'limits': ['Finite pose sampling does not prove all intermediate parameter combinations.',
                       'Raw Core equivalence does not validate CubismCanvas mouth/material/neck/grass patches or physics.',
                       'Changed MOC/atlas bytes require separately updating hash-bound ownership data before runtime adoption.']}


def compare(before_path, after_path, output_path, geometry_tolerance_px=0.01, opacity_tolerance=1e-6):
    before, before_hashes, before_roles = read_probe(before_path)
    after, after_hashes, after_roles = read_probe(after_path)
    tool = str(Path(__file__).resolve())
    hashes = {**before_hashes, **after_hashes, tool: digest(tool)}
    result = compare_reports(before, after, before_roles, after_roles, geometry_tolerance_px, opacity_tolerance)
    verify(hashes)
    result.update(schema_version=1, kind='cubism_core_roundtrip_comparison', created_utc=datetime.now(timezone.utc).isoformat(),
                  before_report={'path': str(Path(before_path).resolve()), 'sha256': digest(before_path)},
                  after_report={'path': str(Path(after_path).resolve()), 'sha256': digest(after_path)},
                  comparison_tool_sha256=hashes[tool], current_inputs_verified_before_and_after=True,
                  current_inputs=hashes, scope={'raw_core_only': True, 'core_or_editor_called': False, 'runtime_patches_applied': False})
    output = Path(output_path).resolve()
    if str(output) in hashes:
        raise ComparisonError('Comparison output must not replace any input.')
    payload = json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n'
    if len(payload.encode('utf8')) > MAX_REPORT_BYTES:
        raise ComparisonError('Comparison output exceeds 64 MiB.')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', encoding='utf8', newline='\n', prefix=output.name + '.', suffix='.tmp',
                                     dir=output.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(payload)
    try:
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--geometry-tolerance-px', type=float, default=0.01)
    parser.add_argument('--opacity-tolerance', type=float, default=1e-6)
    args = parser.parse_args(argv)
    try:
        result = compare(args.before, args.after, args.output, args.geometry_tolerance_px, args.opacity_tolerance)
    except (ComparisonError, OSError, ValueError, KeyError, struct.error) as error:
        print(f'Cubism comparison failed: {error}', file=sys.stderr)
        return 2
    print(json.dumps({'status': result['status'], 'output': str(args.output.resolve()), **result['summary']}))
    return 0 if result['status'] == 'equivalent_within_tolerance' else 1


if __name__ == '__main__':
    raise SystemExit(main())
