"""Read-only joint/pivot analysis of analytical PSD2Live Core probe reports.

python tools/analyze_authoring_probe.py --core-report skeleton.core.json \
    --fixture input/fixture.json --poses poses/skeleton.json --kind skeleton \
    --output skeleton.analysis.json

Material anchors are located in the neutral Native triangles, then sampled
with the same barycentric weights in every pose. No editor/export/Core calls
or character-runtime compensation occur here. Tail curvature is measured
separately from displacement and scaling; it is not a visual quality verdict.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import tempfile


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot_json(path):
    raw = Path(path).read_bytes()
    return json.loads(raw.decode('utf-8-sig')), hashlib.sha256(raw).hexdigest()


def add(a, b):
    return [a[0] + b[0], a[1] + b[1]]


def subtract(a, b):
    return [a[0] - b[0], a[1] - b[1]]


def distance(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def cross(a, b):
    return a[0] * b[1] - a[1] * b[0]


def area(a, b, c):
    return cross(subtract(b, a), subtract(c, a))


def rotate(vector, degrees):
    angle = math.radians(degrees)
    cosine, sine = math.cos(angle), math.sin(angle)
    return [vector[0] * cosine - vector[1] * sine, vector[0] * sine + vector[1] * cosine]


def pixels(vertices, canvas):
    ox, oy = canvas['origin']
    scale = canvas['pixels_per_unit']
    return [[ox + value[0] * scale, oy - value[1] * scale] for value in vertices]


def locate(vertices, indices, target):
    for offset in range(0, len(indices), 3):
        tri = indices[offset:offset + 3]
        if len(tri) != 3:
            raise ValueError('Native topology is not triangulated.')
        a, b, c = [vertices[index] for index in tri]
        determinant = area(a, b, c)
        if abs(determinant) < 1e-8:
            continue
        wa = area(target, b, c) / determinant
        wb = area(a, target, c) / determinant
        wc = 1 - wa - wb
        if min(wa, wb, wc) >= -1e-7 and max(wa, wb, wc) <= 1 + 1e-7:
            return {'triangle': tri, 'weights': [wa, wb, wc], 'source_point': list(target)}
    raise ValueError(f'Anchor {target} is outside the neutral Native triangles.')


def sample(vertices, anchor):
    return [sum(vertices[index][axis] * weight for index, weight in zip(anchor['triangle'], anchor['weights']))
            for axis in (0, 1)]


def mesh_split_centerline(points, vertices, indices):
    """Include every triangle-edge crossing so posed material arc is exact."""
    edges = {tuple(sorted((tri[index], tri[(index + 1) % 3])))
             for offset in range(0, len(indices), 3)
             for tri in [indices[offset:offset + 3]] for index in range(3)}
    result = [list(points[0])]
    for start, end in zip(points, points[1:]):
        vector = subtract(end, start)
        squared_length = vector[0] ** 2 + vector[1] ** 2
        if squared_length < 1e-16:
            raise ValueError('Source centerline has coincident consecutive points.')
        crossings = [0.0, 1.0]
        for first, second in edges:
            a, b = vertices[first], vertices[second]
            edge = subtract(b, a)
            offset = subtract(a, start)
            determinant = cross(vector, edge)
            if abs(determinant) > 1e-9:
                position = cross(offset, edge) / determinant
                edge_position = cross(offset, vector) / determinant
                if -1e-9 <= position <= 1 + 1e-9 and -1e-9 <= edge_position <= 1 + 1e-9:
                    crossings.append(max(0.0, min(1.0, position)))
            elif abs(cross(offset, vector)) < 1e-9:
                for point in (a, b):
                    delta = subtract(point, start)
                    position = (delta[0] * vector[0] + delta[1] * vector[1]) / squared_length
                    if -1e-9 <= position <= 1 + 1e-9:
                        crossings.append(max(0.0, min(1.0, position)))
        previous = 0.0
        for position in sorted(crossings):
            if position - previous > 1e-9:
                result.append([start[axis] + position * vector[axis] for axis in (0, 1)])
                previous = position
    return result


def centerline_metrics(points):
    if len(points) < 3 or any(not all(math.isfinite(value) for value in point) for point in points):
        raise ValueError('Centerline measurement needs at least three finite points.')
    root, tip = points[0], points[-1]
    chord = subtract(tip, root)
    chord_length = math.hypot(*chord)
    if chord_length < 1e-8:
        raise ValueError('Centerline endpoints coincide; straight-chord comparison is undefined.')
    vectors = [subtract(b, a) for a, b in zip(points, points[1:])]
    turns = [math.degrees(math.atan2(cross(a, b), a[0] * b[0] + a[1] * b[1]))
             for a, b in zip(vectors, vectors[1:]) if math.hypot(*a) > 1e-8 and math.hypot(*b) > 1e-8]
    deviation = max(abs(cross(subtract(point, root), chord)) / chord_length for point in points)
    total_turn = sum(abs(value) for value in turns)
    return {'sample_count': len(points), 'polyline_arc_length_px': sum(math.hypot(*vector) for vector in vectors),
            'root_tip_chord_length_px': chord_length, 'max_distance_from_chord_px': deviation,
            'total_absolute_turn_degrees': total_turn,
            'max_local_turn_degrees': max((abs(value) for value in turns), default=0),
            'measurable_bending': deviation >= 1.0 and total_turn > 0.1,
            'bending_thresholds': {'deviation_px': 1.0, 'total_absolute_turn_degrees': 0.1}}


def triangle_health(before, current, indices):
    ratios, flipped, collapsed, originally_degenerate = [], [], [], []
    for offset in range(0, len(indices), 3):
        tri = indices[offset:offset + 3]
        old = area(*(before[index] for index in tri))
        new = area(*(current[index] for index in tri))
        if abs(old) < 1e-8:
            originally_degenerate.append(offset // 3)
            continue
        ratio = new / old
        ratios.append(ratio)
        if ratio < 0:
            flipped.append(offset // 3)
        if abs(ratio) < 0.001:
            collapsed.append(offset // 3)
    return {'measured_triangles': len(ratios), 'min_area_ratio': min(ratios) if ratios else None,
            'max_area_ratio': max(ratios) if ratios else None, 'flipped': flipped,
            'collapsed': collapsed, 'originally_degenerate': originally_degenerate}


def neutral_pose(report):
    for pose in report['poses']:
        if all(abs(record['native_value'] - report['parameters'][name]['default']) < 1e-6
               for name, record in pose['parameters'].items()):
            return pose
    raise ValueError('A true Native-default neutral pose is required for material-point calibration.')


def resolve_target(poses, name):
    target = poses.get('target_names', {}).get(name)
    if not isinstance(target, str) or not target.startswith('mesh:'):
        raise ValueError(f'poses.json needs target_names.{name} with its Native mesh ID.')
    return target.removeprefix('mesh:')


def prepare(report, pose_input, declarations):
    neutral = neutral_pose(report)
    meshes = {}
    anchors = {}
    for name, source, point in declarations:
        mesh = resolve_target(pose_input, source)
        vertices = pixels(neutral['drawables'][mesh]['vertices'], report['canvas'])
        meshes[mesh] = {'neutral': vertices, 'indices': report['topology'][mesh]['indices']}
        anchors[name] = {'mesh': mesh, **locate(vertices, meshes[mesh]['indices'], point)}
    return neutral, meshes, anchors


def pose_geometry(report, pose, meshes, anchors):
    positions = {mesh: pixels(pose['drawables'][mesh]['vertices'], report['canvas']) for mesh in meshes}
    points = {name: sample(positions[anchor['mesh']], anchor) for name, anchor in anchors.items()}
    health = {mesh: triangle_health(value['neutral'], positions[mesh], value['indices']) for mesh, value in meshes.items()}
    return points, health


def analyze_skeleton(report, fixture, pose_input):
    shoulder, elbow, wrist = fixture['shoulder'], fixture['elbow'], fixture['wrist']
    neutral, meshes, anchors = prepare(report, pose_input, [
        ('shoulder', 'upper-arm', shoulder), ('upper_elbow', 'upper-arm', elbow),
        ('lower_elbow', 'forearm', elbow), ('wrist', 'forearm', wrist)])
    expected_upper, expected_lower = distance(shoulder, elbow), distance(elbow, wrist)
    rows = []
    for pose in report['poses']:
        points, health = pose_geometry(report, pose, meshes, anchors)
        upper_length = distance(points['shoulder'], points['upper_elbow'])
        lower_length = distance(points['lower_elbow'], points['wrist'])
        shoulder_angle = pose['parameters']['ParamProbeShoulder']['native_value']
        elbow_angle = pose['parameters']['ParamProbeElbow']['native_value']
        fk = {}
        for name, sign in [('positive_clockwise_canvas', 1), ('positive_counterclockwise_native', -1)]:
            expected_elbow = add(shoulder, rotate(subtract(elbow, shoulder), sign * shoulder_angle))
            expected_wrist = add(expected_elbow, rotate(subtract(wrist, elbow), sign * (shoulder_angle + elbow_angle)))
            fk[name] = {'elbow': expected_elbow, 'wrist': expected_wrist,
                        'upper_elbow_error_px': distance(points['upper_elbow'], expected_elbow),
                        'lower_elbow_error_px': distance(points['lower_elbow'], expected_elbow),
                        'wrist_error_px': distance(points['wrist'], expected_wrist)}
        rows.append({'name': pose['name'], 'shoulder_angle': shoulder_angle, 'elbow_angle': elbow_angle,
                     'points': points, 'shoulder_anchor_error_px': distance(points['shoulder'], shoulder),
                     'elbow_gap_px': distance(points['upper_elbow'], points['lower_elbow']),
                     'upper_length_px': upper_length, 'lower_length_px': lower_length,
                     'upper_length_error_px': upper_length - expected_upper,
                     'lower_length_error_px': lower_length - expected_lower, 'fk': fk, 'triangles': health})
    fk_summary = {name: {'max_wrist_error_px': max(row['fk'][name]['wrist_error_px'] for row in rows),
                         'rms_wrist_error_px': math.sqrt(sum(row['fk'][name]['wrist_error_px'] ** 2 for row in rows) / len(rows)),
                         'max_elbow_error_px': max(max(row['fk'][name]['upper_elbow_error_px'], row['fk'][name]['lower_elbow_error_px']) for row in rows)}
                  for name in rows[0]['fk']}
    summary = {'pose_count': len(rows), 'neutral_pose': neutral['name'],
               'max_shoulder_anchor_error_px': max(row['shoulder_anchor_error_px'] for row in rows),
               'max_elbow_gap_px': max(row['elbow_gap_px'] for row in rows),
               'max_upper_length_error_px': max(abs(row['upper_length_error_px']) for row in rows),
               'max_lower_length_error_px': max(abs(row['lower_length_error_px']) for row in rows),
               'flipped_triangles': sum(len(health['flipped']) for row in rows for health in row['triangles'].values()),
               'collapsed_triangles': sum(len(health['collapsed']) for row in rows for health in row['triangles'].values()),
               'fk_conventions': fk_summary,
               'closest_fk_convention_inferred_from_samples': min(fk_summary, key=lambda name: fk_summary[name]['rms_wrist_error_px'])}
    return {'kind': 'skeleton', 'anchors': anchors, 'summary': summary, 'poses': rows}


def analyze_tail(report, fixture, pose_input):
    root, tip = fixture['root'], fixture['tip']
    middle = fixture.get('middle', [190, 380])
    centerline = fixture.get('centerline', [[root[axis] + (tip[axis] - root[axis]) * i / 46 for axis in (0, 1)] for i in range(47)])
    neutral, meshes, anchors = prepare(report, pose_input, [
        ('root', 'tail', root), ('middle', 'tail', middle), ('tip', 'tail', tip)])
    tail = meshes[resolve_target(pose_input, 'tail')]
    centerline = mesh_split_centerline(centerline, tail['neutral'], tail['indices'])
    centerline_names = [f'centerline-{index:03d}' for index in range(len(centerline))]
    neutral, meshes, anchors = prepare(report, pose_input, [
        ('root', 'tail', root), ('middle', 'tail', middle), ('tip', 'tail', tip)] +
        [(name, 'tail', point) for name, point in zip(centerline_names, centerline)])
    first_length, second_length = distance(root, middle), distance(middle, tip)
    expected_arc_length = centerline_metrics(centerline)['polyline_arc_length_px']
    rows = []
    for pose in report['poses']:
        points, health = pose_geometry(report, pose, meshes, anchors)
        a, b = subtract(points['middle'], points['root']), subtract(points['tip'], points['middle'])
        chord = subtract(points['tip'], points['root'])
        turn = math.degrees(math.atan2(cross(a, b), a[0] * b[0] + a[1] * b[1]))
        middle_distance = abs(cross(subtract(points['middle'], points['root']), chord)) / math.hypot(*chord) if math.hypot(*chord) else None
        measured_centerline = [points[name] for name in centerline_names]
        centerline_measurement = centerline_metrics(measured_centerline)
        landmark_points = {name: points[name] for name in ('root', 'middle', 'tip')}
        rows.append({'name': pose['name'], 'points': landmark_points,
                     'root_displacement_px': distance(points['root'], root),
                     'middle_displacement_px': distance(points['middle'], middle), 'tip_displacement_px': distance(points['tip'], tip),
                     'centerline_turn_degrees': turn, 'middle_distance_from_root_tip_chord_px': middle_distance,
                     'first_segment_length_px': math.hypot(*a), 'second_segment_length_px': math.hypot(*b),
                     'first_segment_length_error_px': math.hypot(*a) - first_length,
                     'second_segment_length_error_px': math.hypot(*b) - second_length,
                     'centerline': {'points': measured_centerline, **centerline_measurement,
                                    'arc_length_error_px': centerline_measurement['polyline_arc_length_px'] - expected_arc_length},
                     'triangles': health})
    summary = {'pose_count': len(rows), 'neutral_pose': neutral['name'],
               'max_root_displacement_px': max(row['root_displacement_px'] for row in rows),
               'max_middle_displacement_px': max(row['middle_displacement_px'] for row in rows),
               'max_tip_displacement_px': max(row['tip_displacement_px'] for row in rows),
               'max_centerline_turn_degrees': max(abs(row['centerline_turn_degrees']) for row in rows),
               'max_middle_distance_from_chord_px': max(row['middle_distance_from_root_tip_chord_px'] for row in rows),
               'max_first_segment_length_error_px': max(abs(row['first_segment_length_error_px']) for row in rows),
               'max_second_segment_length_error_px': max(abs(row['second_segment_length_error_px']) for row in rows),
               'segment_lengths_are_chords_not_material_arc_lengths': True,
               'neutral_source_centerline_arc_length_px': expected_arc_length,
               'centerline_arc_method': 'piecewise affine material sampling at every neutral triangle-edge crossing',
               'centerline_sample_count': len(centerline),
               'max_centerline_arc_length_error_px': max(abs(row['centerline']['arc_length_error_px']) for row in rows),
               'centerline_arc_length_min_px': min(row['centerline']['polyline_arc_length_px'] for row in rows),
               'centerline_arc_length_max_px': max(row['centerline']['polyline_arc_length_px'] for row in rows),
               'measurable_bending_pose_count': sum(row['centerline']['measurable_bending'] for row in rows),
               'flipped_triangles': sum(len(health['flipped']) for row in rows for health in row['triangles'].values()),
               'collapsed_triangles': sum(len(health['collapsed']) for row in rows for health in row['triangles'].values()),
               'displacement_is_not_proof_of_flexible_bending': True}
    return {'kind': 'tail', 'anchors': anchors, 'summary': summary, 'poses': rows}


def analyze(core_path, fixture_path, poses_path, output_path, kind):
    paths = [Path(path).resolve() for path in (core_path, fixture_path, poses_path)]
    snapshots = [snapshot_json(path) for path in paths]
    report, fixture, poses = [item[0] for item in snapshots]
    hashes = {str(path): item[1] for path, item in zip(paths, snapshots)}
    hashes[str(Path(__file__).resolve())] = digest(__file__)
    if not report.get('scope', {}).get('raw_export_only') or not report.get('input_hashes_verified_after_probe'):
        raise ValueError('Analysis requires a complete unpatched Core report.')
    if report['pose_input']['sha256'] != hashes[str(paths[2])]:
        raise ValueError('Pose input does not match the Core evidence.')
    if report['canvas']['size'] != fixture['canvas']:
        raise ValueError('Fixture and Core canvas dimensions differ.')
    current_exports = {str(Path(row['path']).resolve()): row['sha256'] for row in report['model_inputs']}
    current_exports[str(Path(report['cubism_core']['path']).resolve())] = report['cubism_core']['sha256']
    for path, expected in current_exports.items():
        if not Path(path).is_file() or digest(path) != expected:
            raise ValueError(f'Model/Core input missing or changed since the Core probe: {path}')
    source_hashes = []
    for layer in fixture['layers']:
        path = Path(layer['path']).resolve()
        if digest(path) != layer['sha256']:
            raise ValueError(f'Fixture source image changed: {path}')
        source_hashes.append({'path': str(path), 'sha256': layer['sha256']})
    result = analyze_skeleton(report, fixture, poses) if kind == 'skeleton' else analyze_tail(report, fixture, poses)
    result.update(schema_version=1, status='complete', created_utc=datetime.now(timezone.utc).isoformat(),
                  analysis_tool_sha256=digest(__file__), inputs=hashes, fixture_source_images=source_hashes,
                  model_moc_sha256=report['model_moc_sha256'], model_inputs=report['model_inputs'], cubism_core=report['cubism_core'],
                  current_model_and_core_hashes_verified=True,
                  method={'anchors': 'source pixel → neutral Native triangle barycentric weights → posed vertices',
                          'coordinates': 'canvas pixels; x right, y down',
                          'no_exports_or_runtime_fixes': True, 'sampling_limits': 'only declared analytical landmarks and mesh triangles; not a visual verdict'})
    expected_inputs = {**hashes, **current_exports, **{row['path']: row['sha256'] for row in source_hashes}}
    if any(not Path(path).is_file() or digest(path) != expected for path, expected in expected_inputs.items()):
        raise ValueError('Analysis inputs changed while reading.')
    output_path = Path(output_path).resolve()
    protected = {Path(path) for path in expected_inputs}
    if output_path in protected:
        raise ValueError('Analysis output must not replace an input.')
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', encoding='utf8', prefix=output_path.name + '.', suffix='.tmp',
                                     dir=output_path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write('\n')
    try:
        temporary.replace(output_path)
    finally:
        temporary.unlink(missing_ok=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-report', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--poses', type=Path, required=True)
    parser.add_argument('--kind', choices=('skeleton', 'tail'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.core_report, args.fixture, args.poses, args.output, args.kind)
    print(json.dumps(result['summary'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
