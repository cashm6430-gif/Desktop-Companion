"""Read a model export directly through Cubism Core, without Qt/runtime fixes.

Example:
  python tools/probe_cubism_core.py --model experiment.model3.json \
      --poses poses.json --output core-report.json --drawables Mouth Arm

poses.json accepts a list, or {"poses": [{"name": "closed", "parameters":
{"ParamMouthGape": 0}}], "drawables": ["ArtMeshMouth"]}. Parameters reset to
Native defaults before every pose. Optional part_opacities reset similarly.
All drawables receive opacity/geometry summaries; selected drawables also
include raw model-space vertices, with UVs/indices stored once in topology.
This does not evaluate physics, render pixels, or apply CubismCanvas patches.
"""
from __future__ import annotations

import argparse
import ctypes as c
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAX_POSES = 128
MAX_VERTEX_SAMPLES = 200_000
MAX_TOPOLOGY_INDICES = 240_000
MAX_REPORT_BYTES = 64 * 1024 * 1024


class ProbeError(RuntimeError):
    pass


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def read_json_snapshot(path):
    try:
        raw = Path(path).read_bytes()
        return json.loads(raw.decode('utf-8-sig')), hashlib.sha256(raw).hexdigest()
    except (OSError, ValueError, UnicodeError) as error:
        raise ProbeError(f'Cannot read JSON {path}: {error}') from error


def read_json(path):
    return read_json_snapshot(path)[0]


def number(value, context):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ProbeError(f'{context} must be a finite number.')
    return float(value)


def normalize_poses(document):
    rows = document.get('poses') if isinstance(document, dict) else document
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_POSES:
        raise ProbeError(f'poses must contain 1..{MAX_POSES} poses.')
    result, names = [], set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ProbeError(f'Pose {index} must be an object.')
        name = row.get('name', f'pose-{index:03d}')
        if not isinstance(name, str) or not name or name in names:
            raise ProbeError(f'Pose {index} needs a nonempty unique name.')
        names.add(name)
        pose = {'name': name}
        for field in ('parameters', 'part_opacities'):
            values = row.get(field, {})
            if not isinstance(values, dict) or any(not isinstance(key, str) or not key for key in values):
                raise ProbeError(f'{name}.{field} must map IDs to finite numbers.')
            pose[field] = {key: number(value, f'{name}.{field}.{key}') for key, value in values.items()}
        result.append(pose)
    selected = document.get('drawables') if isinstance(document, dict) else None
    if selected is not None and (not isinstance(selected, list) or not selected
                                or any(not isinstance(item, str) or not item for item in selected)
                                or len(set(selected)) != len(selected)):
        raise ProbeError('drawables must be a nonempty list of unique Native IDs.')
    return result, selected


def model_family(model_path):
    model_path = Path(model_path).resolve()
    document, model_hash = read_json_snapshot(model_path)
    refs = document.get('FileReferences') if isinstance(document, dict) else None
    if not isinstance(refs, dict) or not isinstance(refs.get('Moc'), str) or not refs['Moc']:
        raise ProbeError('model3.json requires FileReferences.Moc.')
    records = {model_path: {'model3'}}

    def include(reference, role):
        if not isinstance(reference, str) or not reference:
            raise ProbeError(f'{role} must reference a local file.')
        path = (model_path.parent / reference).resolve()
        if not path.is_file():
            raise ProbeError(f'Missing model reference {role}: {path}')
        records.setdefault(path, set()).add(role)

    for key in ('Moc', 'Physics', 'DisplayInfo', 'Pose', 'UserData'):
        if key in refs:
            include(refs[key], key)
    if 'Textures' in refs:
        if not isinstance(refs['Textures'], list):
            raise ProbeError('FileReferences.Textures must be a list.')
        for index, reference in enumerate(refs['Textures']):
            include(reference, f'Textures[{index}]')

    def nested(value, role):
        if isinstance(value, dict):
            for key, item in value.items():
                if key == 'File':
                    include(item, f'{role}.File')
                else:
                    nested(item, f'{role}.{key}')
        elif isinstance(value, list):
            for index, item in enumerate(value):
                nested(item, f'{role}[{index}]')
    for key in ('Motions', 'Expressions'):
        if key in refs:
            nested(refs[key], key)
    base = model_path.name.removesuffix('.model3.json')
    for suffix in ('.psd2live.json', '.cmo3'):
        sibling = model_path.with_name(base + suffix)
        if sibling.is_file():
            records.setdefault(sibling.resolve(), set()).add('export_metadata' if suffix.endswith('json') else 'editable_export')
    def relative(path):
        try:
            return os.path.relpath(path, model_path.parent).replace('\\', '/')
        except ValueError:
            return str(path)

    family = [{'path': str(path), 'relative_path': relative(path),
               'roles': sorted(roles), 'bytes': path.stat().st_size, 'sha256': digest(path)}
              for path, roles in sorted(records.items(), key=lambda item: str(item[0]))]
    if digest(model_path) != model_hash or next(item['sha256'] for item in family if Path(item['path']) == model_path) != model_hash:
        raise ProbeError('Model JSON changed while resolving its references.')
    return (model_path.parent / refs['Moc']).resolve(), family


def find_core(explicit=None):
    if explicit:
        path = Path(explicit).resolve()
        if not path.is_file():
            raise ProbeError(f'Cubism Core missing: {path}. Supply --core; no software is installed by this probe.')
        return path
    machine = platform.machine().lower()
    if sys.platform == 'win32':
        if machine in ('arm64', 'aarch64'):
            raise ProbeError('Cubism Core missing for Windows ARM64; supply a matching library with --core.')
        arch = 'x86_64' if c.sizeof(c.c_void_p) == 8 else 'x86'
        candidates = [ROOT / 'build/Live2DCubismCore.dll',
                      ROOT / f'CubismSdkForNative-5-r.5/Core/dll/windows/{arch}/Live2DCubismCore.dll']
    elif sys.platform == 'darwin':
        candidates = [ROOT / 'build/libLive2DCubismCore.dylib',
                      ROOT / 'CubismSdkForNative-5-r.5/Core/dll/macos/libLive2DCubismCore.dylib']
    else:
        candidates = [ROOT / 'build/libLive2DCubismCore.so',
                      ROOT / f'CubismSdkForNative-5-r.5/Core/dll/linux/{machine}/libLive2DCubismCore.so']
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise ProbeError(f'Cubism Core missing for {sys.platform}/{machine}; supply a compatible library with --core.')


class Vec2(c.Structure):
    _fields_ = [('X', c.c_float), ('Y', c.c_float)]


def aligned_buffer(size, alignment):
    storage = c.create_string_buffer(size + alignment - 1)
    address = (c.addressof(storage) + alignment - 1) & ~(alignment - 1)
    return storage, address


class NativeModel:
    def __init__(self, core_path, moc_path):
        try:
            # The 32-bit Windows SDK uses stdcall; x64 has the common ABI.
            loader = c.WinDLL if sys.platform == 'win32' else c.CDLL
            self.library = loader(str(core_path))
        except OSError as error:
            raise ProbeError(f'Cannot load Cubism Core {core_path}: {error}') from error
        pointer = c.c_void_p
        self.api = {}

        def bind(name, result, arguments):
            try:
                function = getattr(self.library, 'csm' + name)
            except AttributeError as error:
                raise ProbeError(f'Core API missing: csm{name}') from error
            function.restype, function.argtypes = result, arguments
            self.api[name] = function
            return function

        bind('GetVersion', c.c_uint, [])
        bind('GetLatestMocVersion', c.c_uint, [])
        bind('GetMocVersion', c.c_uint, [pointer, c.c_uint])
        bind('HasMocConsistency', c.c_int, [pointer, c.c_uint])
        bind('ReviveMocInPlace', pointer, [pointer, c.c_uint])
        bind('GetSizeofModel', c.c_uint, [pointer])
        bind('InitializeModelInPlace', pointer, [pointer, pointer, c.c_uint])
        bind('UpdateModel', None, [pointer])
        bind('ResetDrawableDynamicFlags', None, [pointer])
        bind('ReadCanvasInfo', None, [pointer, c.POINTER(Vec2), c.POINTER(Vec2), c.POINTER(c.c_float)])
        for kind in ('Parameter', 'Part', 'Drawable'):
            bind(f'Get{kind}Count', c.c_int, [pointer])
            bind(f'Get{kind}Ids', c.POINTER(c.c_char_p), [pointer])
        for field in ('MinimumValues', 'MaximumValues', 'DefaultValues', 'Values'):
            bind('GetParameter' + field, c.POINTER(c.c_float), [pointer])
        bind('GetParameterTypes', c.POINTER(c.c_int), [pointer])
        bind('GetPartOpacities', c.POINTER(c.c_float), [pointer])
        bind('GetPartParentPartIndices', c.POINTER(c.c_int), [pointer])
        for field in ('VertexCounts', 'IndexCounts', 'TextureIndices', 'ParentPartIndices', 'DrawOrders', 'MaskCounts'):
            bind('GetDrawable' + field, c.POINTER(c.c_int), [pointer])
        try:
            bind('GetRenderOrders', c.POINTER(c.c_int), [pointer])
            self.render_orders_field = 'RenderOrders'
        except ProbeError:
            bind('GetDrawableRenderOrders', c.POINTER(c.c_int), [pointer])
            self.render_orders_field = 'DrawableRenderOrders'
        for field in ('ConstantFlags', 'DynamicFlags'):
            bind('GetDrawable' + field, c.POINTER(c.c_ubyte), [pointer])
        bind('GetDrawableOpacities', c.POINTER(c.c_float), [pointer])
        bind('GetDrawableVertexPositions', c.POINTER(c.POINTER(Vec2)), [pointer])
        bind('GetDrawableVertexUvs', c.POINTER(c.POINTER(Vec2)), [pointer])
        bind('GetDrawableIndices', c.POINTER(c.POINTER(c.c_ushort)), [pointer])
        bind('GetDrawableMasks', c.POINTER(c.POINTER(c.c_int)), [pointer])
        raw = Path(moc_path).read_bytes()
        if len(raw) < 64 or raw[:4] != b'MOC3' or len(raw) > 0xffffffff:
            raise ProbeError(f'Not a complete MOC3 file: {moc_path}')
        self.moc_storage, moc_address = aligned_buffer(len(raw), 64)
        c.memmove(moc_address, raw, len(raw))
        self.moc_version = self.api['GetMocVersion'](moc_address, len(raw))
        if not self.moc_version or self.moc_version > self.api['GetLatestMocVersion']():
            raise ProbeError(f'MOC version {self.moc_version} is unsupported by this Core.')
        if self.api['HasMocConsistency'](moc_address, len(raw)) != 1:
            raise ProbeError(f'Cubism Core rejected MOC consistency: {moc_path}')
        moc = self.api['ReviveMocInPlace'](moc_address, len(raw))
        size = self.api['GetSizeofModel'](moc) if moc else 0
        if not size:
            raise ProbeError('Cubism Core could not revive the MOC.')
        self.model_storage, model_address = aligned_buffer(size, 16)
        self.model = self.api['InitializeModelInPlace'](moc, model_address, size)
        if not self.model:
            raise ProbeError('Cubism Core could not initialize the model.')
        self.parameter_ids = self.ids('Parameter')
        self.part_ids = self.ids('Part')
        self.drawable_ids = self.ids('Drawable')
        self.ranges = {name: {'min': self.get('ParameterMinimumValues')[i],
                              'max': self.get('ParameterMaximumValues')[i],
                              'default': self.get('ParameterDefaultValues')[i],
                              'type': self.get('ParameterTypes')[i]}
                       for i, name in enumerate(self.parameter_ids)}
        for name, values in self.ranges.items():
            if not all(math.isfinite(values[key]) for key in ('min', 'max', 'default')) or values['min'] > values['max']:
                raise ProbeError(f'Invalid Native parameter range: {name}')
            # An out-of-range exported default is evidence of a binding bug,
            # not a reason to hide the actual range from the report.
            values['default_in_range'] = values['min'] <= values['default'] <= values['max']
        self.part_defaults = [self.get('PartOpacities')[i] for i in range(len(self.part_ids))]
        self.vertex_counts = [self.get('DrawableVertexCounts')[i] for i in range(len(self.drawable_ids))]
        self.index_counts = [self.get('DrawableIndexCounts')[i] for i in range(len(self.drawable_ids))]
        if any(value < 0 for value in self.vertex_counts + self.index_counts):
            raise ProbeError('Native topology contains negative counts.')

    def get(self, suffix):
        return self.api['Get' + suffix](self.model)

    def ids(self, kind):
        count = self.api['Get' + kind + 'Count'](self.model)
        if count < 0:
            raise ProbeError(f'Core could not read {kind} count.')
        values = self.api['Get' + kind + 'Ids'](self.model)
        result = [values[index].decode('utf8') for index in range(count)]
        if len(set(result)) != count:
            raise ProbeError(f'Core contains duplicate {kind} IDs.')
        return result

    def update(self, pose):
        unknown = set(pose['parameters']) - set(self.parameter_ids)
        unknown_parts = set(pose['part_opacities']) - set(self.part_ids)
        if unknown or unknown_parts:
            raise ProbeError(f"{pose['name']}: unbound parameter IDs {sorted(unknown)}; part IDs {sorted(unknown_parts)}")
        parameter_values, parts, result = self.get('ParameterValues'), self.get('PartOpacities'), {}
        for index, name in enumerate(self.parameter_ids):
            bounds = self.ranges[name]
            raw = pose['parameters'].get(name, bounds['default'])
            clamped = min(bounds['max'], max(bounds['min'], raw))
            parameter_values[index] = clamped
            result[name] = {'raw': raw, 'clamped': clamped, 'specified': name in pose['parameters'],
                            'was_clamped': raw != clamped, 'native_input': parameter_values[index]}
        for index, name in enumerate(self.part_ids):
            parts[index] = min(1.0, max(0.0, pose['part_opacities'].get(name, self.part_defaults[index])))
        self.api['ResetDrawableDynamicFlags'](self.model)
        self.api['UpdateModel'](self.model)
        for index, name in enumerate(self.parameter_ids):
            result[name]['native_value'] = parameter_values[index]
        return result

    def positions(self):
        pointer = self.get('DrawableVertexPositions')
        return [[[pointer[mesh][vertex].X, pointer[mesh][vertex].Y] for vertex in range(count)]
                for mesh, count in enumerate(self.vertex_counts)]


def array_hash(pointer, count, element):
    return hashlib.sha256(c.string_at(pointer, count * c.sizeof(element)) if count else b'').hexdigest()


def geometry_summary(vertices, reference):
    if not vertices:
        return {'bounds': None, 'max_displacement_from_default': 0, 'rms_displacement_from_default': 0}
    if any(not all(math.isfinite(value) for value in row) for row in vertices):
        raise ProbeError('Native geometry contains non-finite coordinates.')
    squared = [(row[0] - old[0]) ** 2 + (row[1] - old[1]) ** 2 for row, old in zip(vertices, reference)]
    return {'bounds': [min(row[0] for row in vertices), min(row[1] for row in vertices),
                       max(row[0] for row in vertices), max(row[1] for row in vertices)],
            'max_displacement_from_default': math.sqrt(max(squared)),
            'rms_displacement_from_default': math.sqrt(sum(squared) / len(squared))}


def run_probe(model_path, poses_path, output_path, core_path=None, drawables=None):
    model_path, poses_path, output_path = (Path(path).resolve() for path in (model_path, poses_path, output_path))
    poses_document, poses_hash = read_json_snapshot(poses_path)
    poses, document_selection = normalize_poses(poses_document)
    moc_path, family = model_family(model_path)
    core_path = find_core(core_path)
    protected = {Path(item['path']) for item in family} | {poses_path, core_path, Path(__file__).resolve()}
    if output_path in protected:
        raise ProbeError('Output must not overwrite a model, pose, Core, or tool input.')
    hashes = {path: digest(path) for path in protected}
    if hashes[poses_path] != poses_hash or any(hashes[Path(item['path'])] != item['sha256'] for item in family):
        raise ProbeError('Model or pose inputs changed while initializing the probe.')
    native = NativeModel(core_path, moc_path)
    selected = drawables if drawables is not None else document_selection
    if selected is None:
        selected = native.drawable_ids
    if not selected or len(set(selected)) != len(selected) or set(selected) - set(native.drawable_ids):
        raise ProbeError(f'Select unique Native drawable IDs; unknown: {sorted(set(selected) - set(native.drawable_ids))}')
    selected = set(selected)
    chosen_vertices = sum(count for name, count in zip(native.drawable_ids, native.vertex_counts) if name in selected)
    chosen_indices = sum(count for name, count in zip(native.drawable_ids, native.index_counts) if name in selected)
    if chosen_vertices * len(poses) > MAX_VERTEX_SAMPLES or chosen_indices > MAX_TOPOLOGY_INDICES:
        raise ProbeError('Raw geometry exceeds the report budget; select fewer --drawables or poses.')
    size, origin, ppu = Vec2(), Vec2(), c.c_float()
    native.api['ReadCanvasInfo'](native.model, c.byref(size), c.byref(origin), c.byref(ppu))
    native.update({'name': 'native-default', 'parameters': {}, 'part_opacities': {}})
    reference = native.positions()
    report = {'schema_version': 1, 'kind': 'cubism_core_export_probe', 'status': 'complete',
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'scope': {'raw_export_only': True, 'cubism_canvas_used': False, 'physics_evaluated': False,
                        'rendered_pixels_evaluated': False, 'pose_parameters_reset_to_native_defaults': True,
                        'geometry_coordinates': 'Native model space; x right, y up',
                        'source_pixel_mapping': 'x=origin.x+X*pixels_per_unit; y=origin.y-Y*pixels_per_unit',
                        'dynamic_flags_reference': 'previous Core update, not an independent visibility test'},
              'model': str(model_path), 'model_inputs': family, 'model_moc_sha256': hashes[moc_path],
              'cubism_core': {'path': str(core_path), 'sha256': hashes[core_path],
                              'version': native.api['GetVersion'](), 'latest_moc_version': native.api['GetLatestMocVersion'](),
                              'moc_version': native.moc_version},
              'pose_input': {'path': str(poses_path), 'sha256': hashes[poses_path]},
              'probe_tool_sha256': hashes[Path(__file__).resolve()],
              'array_hash_encoding': {'vertices_uv': 'float32', 'indices': 'uint16', 'byte_order': sys.byteorder},
              'canvas': {'size': [size.X, size.Y], 'origin': [origin.X, origin.Y], 'pixels_per_unit': ppu.value},
              'parameter_count': len(native.parameter_ids), 'parameters': native.ranges,
              'parts': {name: {'default_opacity': native.part_defaults[index], 'parent_index': native.get('PartParentPartIndices')[index]}
                        for index, name in enumerate(native.part_ids)},
              'drawable_count': len(native.drawable_ids), 'selected_drawables': sorted(selected), 'topology': {}, 'poses': []}
    uvs, indices = native.get('DrawableVertexUvs'), native.get('DrawableIndices')
    masks, mask_counts = native.get('DrawableMasks'), native.get('DrawableMaskCounts')
    for index, name in enumerate(native.drawable_ids):
        vertex_count, index_count = native.vertex_counts[index], native.index_counts[index]
        parent = native.get('DrawableParentPartIndices')[index]
        topology = {'index': index, 'vertex_count': vertex_count, 'index_count': index_count,
                    'texture_index': native.get('DrawableTextureIndices')[index], 'parent_part_index': parent,
                    'parent_part': native.part_ids[parent] if 0 <= parent < len(native.part_ids) else None,
                    'constant_flags': native.get('DrawableConstantFlags')[index],
                    'uvs_sha256': array_hash(uvs[index], vertex_count, Vec2),
                    'indices_sha256': array_hash(indices[index], index_count, c.c_ushort),
                    'masks': [masks[index][i] for i in range(mask_counts[index])],
                    'default_opacity': native.get('DrawableOpacities')[index],
                    'default_geometry': geometry_summary(reference[index], reference[index])}
        if name in selected:
            topology['uvs'] = [[uvs[index][i].X, uvs[index][i].Y] for i in range(vertex_count)]
            topology['indices'] = [indices[index][i] for i in range(index_count)]
            if any(value >= vertex_count for value in topology['indices']):
                raise ProbeError(f'Native drawable index outside vertex buffer: {name}')
        report['topology'][name] = topology
    for pose in poses:
        parameter_values = native.update(pose)
        positions = native.positions()
        position_buffers = native.get('DrawableVertexPositions')
        result = {'name': pose['name'], 'parameters': parameter_values,
                  'part_opacities': {name: native.get('PartOpacities')[index] for index, name in enumerate(native.part_ids)},
                  'drawables': {}}
        for index, name in enumerate(native.drawable_ids):
            opacity = native.get('DrawableOpacities')[index]
            if not math.isfinite(opacity):
                raise ProbeError(f'Native drawable opacity is non-finite: {name}')
            flags = native.get('DrawableDynamicFlags')[index]
            record = {'opacity': opacity, 'visible_flag': bool(flags & 1), 'dynamic_flags': flags,
                      'draw_order': native.get('DrawableDrawOrders')[index], 'render_order': native.get(native.render_orders_field)[index],
                      'vertices_sha256': array_hash(position_buffers[index], native.vertex_counts[index], Vec2),
                      **geometry_summary(positions[index], reference[index])}
            if name in selected:
                record['vertices'] = positions[index]
            result['drawables'][name] = record
        report['poses'].append(result)
    changed = [str(path) for path, expected in hashes.items() if digest(path) != expected]
    if changed:
        raise ProbeError('Probe inputs changed while reading: ' + ', '.join(changed))
    report['input_hashes_verified_after_probe'] = True
    # Serialize before creating output: a non-finite Core value cannot leave
    # behind a successful-looking report, or replace existing good evidence.
    payload = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + '\n'
    if len(payload.encode('utf8')) > MAX_REPORT_BYTES:
        raise ProbeError('Report exceeds 64 MiB; select fewer --drawables or poses.')
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf8', newline='\n',
                                     prefix=output_path.name + '.', suffix='.tmp',
                                     dir=output_path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(payload)
    try:
        temporary.replace(output_path)
    finally:
        temporary.unlink(missing_ok=True)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--poses', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--core', type=Path)
    parser.add_argument('--drawables', nargs='+', help='Native drawable IDs; overrides the poses.json selection')
    args = parser.parse_args(argv)
    try:
        report = run_probe(args.model, args.poses, args.output, args.core, args.drawables)
    except (ProbeError, OSError, ValueError) as error:
        print(f'Core probe failed: {error}', file=sys.stderr)
        return 2
    print(json.dumps({'status': report['status'], 'output': str(args.output.resolve()),
                      'parameters': report['parameter_count'], 'drawables': report['drawable_count'],
                      'poses': len(report['poses']), 'raw_export_only': True}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
