"""Source-bound, neutral-body author prototype for the lower back-hair fold.

Authors only a public deformer_grids JSON fragment. No model, mesh, atlas,
body-rig source, renderer, approval or existing capture is modified. The
forward evaluator was independently matched to the actual eight-page Core;
the new fragment still requires an actual compiled-MOC and Native review.
"""
from pathlib import Path
import argparse
import itertools
import json
import sys
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from head_parent_geometry import HeadParentRig, SOURCE, GRAPH, HAIR_GRAPH, ROOT, weights, read
from probe_cubism_core import digest

NAME = 'DeformHairBackFollow'
SIT_KEYS = [0., .35, .5, .65, .8, .92, 1.]
FOLD_WEIGHTS = [0., 0., .4, 1., 1., 0., 0.]
DEEP_DROP = 328.
CAP_Y = 1180.
OUTWARD_PX = 28.
MESHES = ['ArtMeshBackHair2', 'ArtMeshBackHair']


class HairFoldRig(HeadParentRig):
    """Additional warp overrides; the inherited BodyRig file stays untouched."""
    def __init__(self):
        super().__init__()
        self.warp_overrides = {}

    def cp(self, name, parameters, edited):
        if not edited or name not in self.warp_overrides:
            return super().cp(name, parameters, edited)
        axes, cells = self.warp_overrides[name]
        selected = [([], 1.)]
        for parameter, keys in axes:
            selected = [(coord + [i], w * coef) for coord, w in selected
                        for i, coef in weights(keys, parameters.get(parameter, self.default[parameter]))]
        return sum(w * cells[tuple(coord)] for coord, w in selected)


def twice_area(points, triangles):
    a, b, c = np.asarray(points)[triangles].transpose(1, 0, 2)
    return (b[:, 0]-a[:, 0])*(c[:, 1]-a[:, 1])-(b[:, 1]-a[:, 1])*(c[:, 0]-a[:, 0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=HERE/'hair-fold-r1-plan.json')
    args = parser.parse_args()
    output = args.output.resolve()
    proof_path = output.with_name(output.stem+'-proof.json')
    if output.exists() or proof_path.exists() or not output.is_relative_to(ROOT/'.local'):
        raise ValueError('Plan and proof must both be fresh .local paths.')
    source_model = read(SOURCE/'edited.model3.json')
    pages = [(SOURCE/p).resolve() for p in source_model['FileReferences']['Textures']]
    body_target = ROOT/'.local/authoring/sit-pose-20261004-r1/geometry/r9-source-design/r9-body-CP-and-contact-targets-v2.json'
    if read(body_target)['deep_head_drop_source_px'] != DEEP_DROP:
        raise ValueError('Fragment requires the source-bound v2 body profile, drop328.')
    dependencies = [Path(__file__), HERE/'head_parent_geometry.py', GRAPH, HAIR_GRAPH,
                    HAIR_GRAPH.with_name('public-graph-receipt.json'), body_target,
                    ROOT/'.local/authoring/sit-pose-20261004-r1/geometry/r9-source-design/body_crouch_r9.py',
                    ROOT/'.local/authoring/sit-pose-20261004-r1/geometry/full_context_geometry.py',
                    SOURCE/'edited.cmo3', SOURCE/'edited.moc3', SOURCE/'edited.model3.json', *pages]
    pins = {str(p): digest(p) for p in dependencies}
    rig = HairFoldRig()
    rig.set_deep_profile(DEEP_DROP)
    node = rig.W[NAME]
    parent = node['getParent-lmpY1tE']
    if parent != 'DeformHeadContainer' or [a[0] for a in rig.axes[NAME]] != ['ParamAngleX', 'ParamAngleY']:
        raise ValueError('Unexpected source HairBackFollow parent or axis order.')
    if (node['getRows'], node['getColumns']) != (4, 3):
        raise ValueError('This first fold is tied to the actual 5x4 source CP lattice.')
    specs = {'id': NAME, 'append_axes': [{'parameter': 'ParamSitPose', 'keys': SIT_KEYS}], 'keyforms': []}
    cells = {}
    cp_rows = []
    max_inverse = 0.
    min_jacobian = float('inf')
    for bx, by in itertools.product(range(3), repeat=2):
        original = np.asarray(rig.CP[NAME][(bx, by)], np.float32).astype(float)
        p = {'ParamAngleX': rig.axes[NAME][0][1][bx], 'ParamAngleY': rig.axes[NAME][1][1][by],
             'ParamAngleZ': rig.default['ParamAngleZ'], 'ParamSitPose': .65}
        source_world = rig.points(parent, original, dict(p, ParamSitPose=0), False).reshape(5, 4, 2)
        deep_world = rig.points(parent, original, p, True).reshape(5, 4, 2)
        # Only the low perimeter row is authored. The lattice cannot add a row
        # at Y900: interpolation support starts at the unchanged penultimate
        # row (~sourceY745..818). This limitation is explicit in the proof.
        if not np.all(source_world[-1, :, 1] > 900) or np.any(source_world[:-1, :, 1] > 900):
            raise ValueError('Unexpected source low-row ownership.')
        target = deep_world.copy()
        centre_x = float(np.mean(target[2, :, 0]))
        side = np.sign(target[-1, :, 0] - centre_x)
        target[-1, :, 0] += OUTWARD_PX * side
        target[-1, :, 1] = np.minimum(target[-1, :, 1], CAP_Y)
        if np.any(target[-1, :, 1] <= target[-2, :, 1] + 10):
            raise ValueError('Fold collapses the lower lattice; request a denser author mesh.')
        solved, solve = rig.inverse(parent, target[-1], p, True)
        folded = original.copy()
        folded[-4:] = np.asarray(solved, np.float32).astype(float)
        reconstructed = rig.points(parent, folded, p, True).reshape(5, 4, 2)
        residual = float(np.linalg.norm(reconstructed-target, axis=2).max())
        max_inverse = max(max_inverse, residual)
        min_jacobian = min(min_jacobian, solve['minimum_evaluated_jacobian'])
        if residual >= .001:
            raise ValueError(('Float32 parent inverse failed', bx, by, residual))
        for si, (sit, blend) in enumerate(zip(SIT_KEYS, FOLD_WEIGHTS)):
            cp = np.asarray(original + blend*(folded-original), np.float32).astype(float)
            # Direct copying protects all original CP bit patterns, including
            # whole source grids at Sit0/.35/1 and the complete upper four rows.
            cp[:-4] = original[:-4]
            if blend == 0: cp = original.copy()
            cells[(bx, by, si)] = cp
            if blend != 0:
                specs['keyforms'].append({'coordinate': [bx, by, si], 'control_points': cp.reshape(-1).tolist()})
            cp_rows.append({'coordinate': [bx, by, si], 'Sit': sit, 'fold_weight': blend,
                            'source_CP_sha256': digest_bytes(original), 'result_CP_sha256': digest_bytes(cp),
                            'upper_four_rows_float32_exact': bool(np.array_equal(original[:-4].astype(np.float32).view(np.uint32), cp[:-4].astype(np.float32).view(np.uint32))),
                            'whole_source_CP_preserved': bool(np.array_equal(original.astype(np.float32).view(np.uint32), cp.astype(np.float32).view(np.uint32)))})
    rig.warp_overrides[NAME] = (rig.axes[NAME]+[('ParamSitPose', SIT_KEYS)], cells)

    # Independent analytic diagnostics of complete child lattices and meshes.
    # These are not an actual export or a replacement for Root's Core gate.
    rows = []
    maximum_mesh_y = -float('inf')
    minimum_area_ratio = float('inf')
    flips = 0
    zero_areas = 0
    face_residual = 0.
    for sit, bx, by, hair in itertools.product(sorted(set(np.linspace(0, 1, 41).tolist()+SIT_KEYS)), range(3), range(3), [-1., 0., 1.]):
        p = {'ParamSitPose': sit, 'ParamAngleX': rig.axes[NAME][0][1][bx], 'ParamAngleY': rig.axes[NAME][1][1][by],
             'ParamAngleZ': rig.default['ParamAngleZ'], 'ParamHairBack': hair}
        for name in MESHES:
            mesh = rig.D[name]
            local = rig.mesh_local(name, p)
            world = rig.points(mesh['getParentDeformerId-lmpY1tE'], local, p, True)
            # The unfurled baseline includes the same edited328 body, so
            # orientation diagnostics measure the fold alone.
            saved = rig.warp_overrides.pop(NAME)
            unfolded = rig.points(mesh['getParentDeformerId-lmpY1tE'], local, p, True)
            rig.warp_overrides[NAME] = saved
            triangles = np.asarray(mesh['getMesh']['getIndices']).reshape(-1, 3)
            old_area = twice_area(unfolded, triangles)
            new_area = twice_area(world, triangles)
            good = np.abs(old_area) > 1e-6
            flipped = int(np.count_nonzero(old_area[good]*new_area[good] < 0))
            zero = int(np.count_nonzero(np.abs(new_area[good]) <= 1e-6))
            ratio = float(np.min(new_area[good]/old_area[good]))
            flips += flipped; zero_areas += zero
            minimum_area_ratio = min(minimum_area_ratio, ratio)
            if not np.all(np.isfinite(world)): raise ValueError(('Nonfinite folded hair', p, name))
            if sit in [.65, .8]:
                maximum_mesh_y = max(maximum_mesh_y, float(world[:, 1].max()))
            if sit in SIT_KEYS:
                rows.append({'parameters': p, 'drawable': name, 'source_world_bbox': [*world.min(0).tolist(), *world.max(0).tolist()],
                             'flipped_triangles': flipped, 'new_zero_area': zero, 'minimum_area_ratio': ratio})
        face = rig.D['ArtMeshFace']; face_local = rig.mesh_local('ArtMeshFace', p)
        a = rig.points(face['getParentDeformerId-lmpY1tE'], face_local, p, True)
        saved = rig.warp_overrides.pop(NAME)
        b = rig.points(face['getParentDeformerId-lmpY1tE'], face_local, p, True)
        rig.warp_overrides[NAME] = saved
        face_residual = max(face_residual, float(np.linalg.norm(a-b, axis=1).max()))
    if flips or zero_areas or maximum_mesh_y > CAP_Y+.001 or face_residual != 0:
        raise ValueError(('Analytic fold gate failed', flips, zero_areas, maximum_mesh_y, face_residual))
    if any(digest(Path(p)) != sha for p, sha in pins.items()):
        raise ValueError('Input changed during fragment generation.')
    limits = ['Only neutral BodyXYZ/Breath/Shift, default HeadZ, AngleXY nine keys, HairBack three keys checked analytically.',
              'The source parent evaluator was actual-Core verified; this new fragment has not been compiled or Core/Native reviewed.',
              'Body328 profile is a required companion edit; never apply the fragment alone to an adopted model.',
              'Lower lattice row only; unchanged row3 provides interpolation support from sourceY745..818, so deformation does not start at an exact Y900 cut.',
              'Full body/head-angle/interpolated/physics domains and visual layer integrity remain pending.']
    plan = {'schema_version': 2, 'kind': 'R9_neutral_hair_fold_author_fragment_pending_actual_MOC',
            'source': {'cmo_sha256': digest(SOURCE/'edited.cmo3'), 'moc_sha256': digest(SOURCE/'edited.moc3'),
                       'atlas_pages': [{'path': str(p), 'sha256': digest(p)} for p in pages]},
            'deformer_grids': [specs], 'provenance': {'input_sha256': pins, 'body_drop_source_px': DEEP_DROP,
                        'required_body_target_sha256': digest(body_target), 'source_CP_rows_preserved': [0, 1, 2, 3],
                        'unchanged_Sit_keys': [0., .35, .92, 1.]},
            'limits': limits, 'approval': {'engineering': 'analytic_fragment_only', 'visual': 'pending', 'adoption': 'pending'}}
    proof = {'schema_version': 1, 'kind': 'R9_hair_fold_parent_inverse_analytic_diagnostic', 'input_sha256': pins,
             'full_Cartesian_cell_count': len(cells), 'explicit_changed_keyforms': len(specs['keyforms']),
             'maximum_float32_inverse_world_residual_source_px': max_inverse, 'minimum_evaluated_parent_jacobian': min_jacobian,
             'max_hair_world_y_deep_keys': maximum_mesh_y, 'minimum_hair_triangle_area_ratio': minimum_area_ratio,
             'flipped_triangle_count': flips, 'new_zero_triangle_count': zero_areas, 'face_folding_residual_source_px': face_residual,
             'Sit_sampling_step': .025, 'CP_rows': cp_rows, 'mesh_key_rows': rows,
             'source_parameter_order': ['ParamAngleX', 'ParamAngleY', 'ParamSitPose'],
             'limits': limits, 'actual_export_Core': 'pending', 'Native_visual': 'pending', 'adopted': False}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(plan, indent=2, allow_nan=False)+'\n', encoding='utf8')
    proof['plan_sha256'] = digest(output)
    proof_path.write_text(json.dumps(proof, indent=2, allow_nan=False)+'\n', encoding='utf8')
    print(json.dumps({'plan': str(output), 'plan_sha256': digest(output), 'proof': str(proof_path),
                      'maximum_inverse_residual_source_px': max_inverse, 'max_hair_world_y_deep_keys': maximum_mesh_y,
                      'minimum_area_ratio': minimum_area_ratio, 'flips': flips, 'actual_export_Core': 'pending'}, indent=2))


def digest_bytes(array):
    import hashlib
    return hashlib.sha256(np.asarray(array, np.float32).tobytes()).hexdigest()


if __name__ == '__main__':
    main()
