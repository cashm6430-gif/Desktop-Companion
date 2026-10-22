"""Read-only Native Core comparison of old stretch's three rejected channels.

This intentionally does not use a renderer, textures, physics, or post-Core
ground/material/neck transforms. Root separately compares paired real renders.
"""
import ctypes as c
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).with_name('axis-audit.json')
MOC = ROOT / 'assets/live2d/whale-girl/whale-girl-layered-draft.moc3'
CORE = ROOT / 'build/Live2DCubismCore.dll'
CLIP = ROOT / 'assets/motions/busy-stretch.motion.json'
REMOVED = {'ParamBrowLY', 'ParamBrowRY', 'ParamShiftY'}
started = datetime.now(timezone.utc).isoformat()
core = c.WinDLL(str(CORE))
pointer = c.c_void_p


def function(name, result, arguments):
    value = getattr(core, name)
    value.restype, value.argtypes = result, arguments
    return value


revive = function('csmReviveMocInPlace', pointer, [pointer, c.c_uint])
size_model = function('csmGetSizeofModel', c.c_uint, [pointer])
initialize = function('csmInitializeModelInPlace', pointer, [pointer, pointer, c.c_uint])
count_parameters = function('csmGetParameterCount', c.c_int, [pointer])
parameter_ids = function('csmGetParameterIds', c.POINTER(c.c_char_p), [pointer])
parameter_values = function('csmGetParameterValues', c.POINTER(c.c_float), [pointer])
parameter_defaults = function('csmGetParameterDefaultValues', c.POINTER(c.c_float), [pointer])
parameter_min = function('csmGetParameterMinimumValues', c.POINTER(c.c_float), [pointer])
parameter_max = function('csmGetParameterMaximumValues', c.POINTER(c.c_float), [pointer])
count_drawables = function('csmGetDrawableCount', c.c_int, [pointer])
drawable_ids = function('csmGetDrawableIds', c.POINTER(c.c_char_p), [pointer])
vertex_counts = function('csmGetDrawableVertexCounts', c.POINTER(c.c_int), [pointer])
positions = function('csmGetDrawableVertexPositions', c.POINTER(c.POINTER(c.c_float)), [pointer])
opacities = function('csmGetDrawableOpacities', c.POINTER(c.c_float), [pointer])
update = function('csmUpdateModel', None, [pointer])


def aligned(size, alignment):
    storage = c.create_string_buffer(size + alignment - 1)
    return storage, (c.addressof(storage) + alignment - 1) & ~(alignment - 1)


raw = MOC.read_bytes()
moc_storage, moc_address = aligned(len(raw), 64)
c.memmove(moc_address, raw, len(raw))
moc = revive(moc_address, len(raw))
size = size_model(moc)
model_storage, model_address = aligned(size, 16)
model = initialize(moc, model_address, size)
assert model
identifiers = [parameter_ids(model)[i].decode() for i in range(count_parameters(model))]
defaults = [parameter_defaults(model)[i] for i in range(len(identifiers))]
drawables = [drawable_ids(model)[i].decode() for i in range(count_drawables(model))]
counts = [vertex_counts(model)[i] for i in range(len(drawables))]


def pose(parameters):
    values = parameter_values(model)
    for index, value in enumerate(defaults):
        values[index] = value
    for name, value in parameters.items():
        values[identifiers.index(name)] = value
    update(model)
    vertices, alpha = positions(model), opacities(model)
    return [(tuple(vertices[i][j] for j in range(counts[i] * 2)), alpha[i])
            for i in range(len(drawables))]


def difference(a, b):
    geometry = [max(abs(x - y) for x, y in zip(av, bv))
                for (av, _), (bv, _) in zip(a, b)]
    opacity = [abs(ao - bo) for (_, ao), (_, bo) in zip(a, b)]
    return {'max_abs_vertex_coordinate_delta_model_units': max(geometry),
            'max_abs_drawable_opacity_delta': max(opacity),
            'drawable_geometry_changes_over_1e_7': [name for name, value in zip(drawables, geometry) if value > 1e-7],
            'drawable_opacity_changes_over_1e_7': [name for name, value in zip(drawables, opacity) if value > 1e-7]}


clip = json.loads(CLIP.read_text(encoding='utf8'))
keys = clip['keyframes']
times = sorted(set(round(index / 15, 8) for index in range(61)) |
               {float(key['time']) for key in keys})


def sample(time):
    if time <= keys[0]['time']:
        return keys[0]['parameters']
    if time >= keys[-1]['time']:
        return keys[-1]['parameters']
    first, last = next((a, b) for a, b in zip(keys, keys[1:])
                       if a['time'] <= time <= b['time'])
    ratio = (time - first['time']) / (last['time'] - first['time'])
    ratio = ratio * ratio * (3 - 2 * ratio)
    return {name: value + (last['parameters'][name] - value) * ratio
            for name, value in first['parameters'].items()}


samples = []
for time in times:
    old = sample(time)
    cleaned = {name: value for name, value in old.items() if name not in REMOVED}
    reference = pose(cleaned)
    brows = pose({**cleaned, **{name: old[name] for name in REMOVED if name != 'ParamShiftY'}})
    samples.append({'time_seconds': time,
                    'original_removed_values': {name: old[name] for name in sorted(REMOVED)},
                    'brow_only_difference': difference(reference, brows),
                    'all_three_difference': difference(reference, pose(old))})

extremes = []
for regime, base in [('neutral-standing', {}), ('seated-desk-key-0.7', sample(0.7))]:
    base = {name: value for name, value in base.items() if name not in REMOVED}
    reference = pose(base)
    for axis in sorted(REMOVED):
        index = identifiers.index(axis)
        for value in [parameter_min(model)[index], parameter_max(model)[index]]:
            extremes.append({'regime': regime, 'axis': axis, 'value': value,
                             **difference(reference, pose({**base, axis: value}))})

data = {
    'version': 1,
    'capture_started_at_utc': started,
    'capture_finished_at_utc': datetime.now(timezone.utc).isoformat(),
    'moc': {'path': str(MOC), 'sha256': hashlib.sha256(raw).hexdigest()},
    'core': {'path': str(CORE), 'sha256': hashlib.sha256(CORE.read_bytes()).hexdigest()},
    'original_clip': {'path': str(CLIP), 'sha256': hashlib.sha256(CLIP.read_bytes()).hexdigest(),
                      'revision': clip.get('revision'), 'approval': clip.get('approval')},
    'method': {
        'api': 'Cubism Core csmInitializeModelInPlace / reset all parameters to native defaults / set sampled raw parameter float values / csmUpdateModel',
        'comparison': 'For every sample compare original full pose against identical pose with exactly ParamBrowLY, ParamBrowRY, ParamShiftY omitted; separately compare brows without ShiftY.',
        'interpolation': 'Current old clip keyframes, smoothstep; 15 Hz endpoints plus every authored key time; float inputs stored in Core float32.',
        'geometry_scope': 'Every XY coordinate of every vertex of every Native drawable, including hidden drawables; no opacity/visibility filtering.',
        'opacity_scope': 'Every Native drawable opacity.',
        'excluded': ['Pixels/textures/render rasterization', 'Qt input/player temporal smoothing', 'Physics', 'Post-Core renderer ground/material/neck/desk corrections', 'Render order/color outputs'],
        'interpretation': 'This is a Native geometry/opacity probe, not a pixel equivalence claim. Real paired renders are a separate gate. Extreme axis probes do not establish arbitrary combined-pose capability.'},
    'inventory': {'parameters': len(identifiers), 'drawables': len(drawables),
                  'vertices': sum(counts), 'xy_coordinates': sum(counts) * 2,
                  'drawable_vertex_counts': dict(zip(drawables, counts))},
    'sample_count': len(samples),
    'samples': samples,
    'max_original_vs_cleaned_vertex_delta_model_units': max(item['all_three_difference']['max_abs_vertex_coordinate_delta_model_units'] for item in samples),
    'max_original_vs_cleaned_opacity_delta': max(item['all_three_difference']['max_abs_drawable_opacity_delta'] for item in samples),
    'max_brow_only_vertex_delta_model_units': max(item['brow_only_difference']['max_abs_vertex_coordinate_delta_model_units'] for item in samples),
    'extreme_axis_probes': extremes,
}
OUT.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
print(json.dumps({key: value for key, value in data.items() if key not in ['samples', 'inventory', 'method']}, ensure_ascii=False))
