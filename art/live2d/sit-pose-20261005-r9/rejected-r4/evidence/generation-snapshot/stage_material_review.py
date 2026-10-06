import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools'))
from probe_cubism_core import NativeModel, array_hash


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--export', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--hide-ground-palms', action='store_true')
    parser.add_argument('--stage-only', action='store_true')
    args = parser.parse_args()
    run = Path(__file__).resolve().parent
    formal = ROOT/'assets/live2d/whale-girl'
    target = run/args.label
    if target.exists(): raise RuntimeError('Review target must be fresh')
    export = args.export.resolve()
    core = ROOT/'CubismSdkForNative-5-r.5/Core/dll/windows/x86_64/Live2DCubismCore.dll'
    models = [NativeModel(core, formal/'whale-girl-layered-draft.moc3'),
              NativeModel(core, export/'edited.moc3')]
    topologies = []
    for n in models:
        uvs = n.get('DrawableVertexUvs'); indices = n.get('DrawableIndices')
        topologies.append({name: {'vertices': n.vertex_counts[i], 'indices': n.index_counts[i],
                                 'uv_sha256': array_hash(uvs[i], n.vertex_counts[i], type(uvs[i].contents)),
                                 'indices_sha256': array_hash(indices[i], n.index_counts[i], c.c_ushort)}
                           for i, name in enumerate(n.drawable_ids)})
    assert all(topologies[1][name] == row for name, row in topologies[0].items())
    assert sha(export/'texture_00.png') == sha(formal/'whale-girl-layered-draft.4096/texture_00.png')
    edit = json.loads((export/'edit-report.json').read_text(encoding='utf8'))
    assert edit['untargeted_runtime_nodes_equal'] and edit['direct_vs_saved_CMO_readback_all_runtime_values_exact']
    shutil.copytree(formal, target/'assets/live2d/whale-girl')
    family = target/'assets/live2d/whale-girl'
    moc = family/'whale-girl-layered-draft.moc3'
    shutil.copyfile(export/'edited.moc3', moc)
    model_path = family/'whale-girl-layered-draft.model3.json'
    model = json.loads(model_path.read_text(encoding='utf8'))
    exported = json.loads((export/'edited.model3.json').read_text(encoding='utf8'))
    model['FileReferences']['Textures'] = []
    for i, name in enumerate(exported['FileReferences']['Textures']):
        page = f'whale-girl-layered-draft.4096/texture_{i:02d}.png'
        shutil.copyfile(export/name, family/page)
        model['FileReferences']['Textures'].append(page)
    model_path.write_text(json.dumps(model, ensure_ascii=False, indent=2)+'\n', encoding='utf8')
    ownership_path = family/'material-ownership/ownership.json'
    ownership = json.loads(ownership_path.read_text(encoding='utf8'))
    ownership['moc_sha256'] = sha(moc)
    ownership['moc_rebinding_evidence'] = {
        'atlas_byte_exact': True, 'all34_native_UV_and_index_arrays_exact': True,
        'source_edit_report_sha256': sha(export/'edit-report.json')}
    ownership_path.write_text(json.dumps(ownership, ensure_ascii=False, indent=2)+'\n', encoding='utf8')
    params = set(models[1].parameter_ids)
    coupled = ({'skirtSpread': 'ParamSkirtSpread', 'handGround': 'ParamHandGround'}
               if {'ParamSkirtSpread', 'ParamHandGround'} <= params else {})
    meta_path = family/'whale-girl-layered-draft.psd2live.json'
    meta = json.loads(meta_path.read_text(encoding='utf8'))
    meta['runtimePostureTransition'] = {
        'version': 2, 'stage': 'structural-review', 'mocSha256': sha(moc),
        'logicalToNative': 'identity', 'geometryOwner': 'author-model',
        'coupledParameters': coupled,
        'approvedConceptSha256': 'b31dbd46c439d3ec56d2bb223de850c3469f8a2d96e2ea28c35b3a9ce76c751b',
        'review_scope': args.label, 'adopted': False}
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2)+'\n', encoding='utf8')
    poses = []
    for busy, sit in [(0,0), (1,.1), (1,.2), (1,.35), (1,.5), (1,.65),
                       (1,.8), (1,.929), (1,.93), (1,.98), (1,1)]:
        poses.append({'time': len(poses), 'name': f'busy{busy}-sit{sit}',
                      'parameters': {'ParamBusyLaptop': busy, 'ParamSitPose': sit,
                                     'ParamArmLA': -40*sit, 'ParamArmRA': -40*sit,
                                     'ParamLaptopVisible': 1 if sit == 1 else 0}})
        if args.hide_ground_palms:
            poses[-1]['parameters']['ParamHandGround'] = 0
    pose_path = target/'poses.json'
    pose_path.write_text(json.dumps({'keyframes': poses}, indent=2)+'\n', encoding='utf8')
    record = {'schema_version': 1, 'formal_moc_sha256': sha(formal/'whale-girl-layered-draft.moc3'),
              'candidate_moc_sha256': sha(moc), 'all34_UV_and_indices_exact': True,
              'native_topology': topologies[1], 'production_runtime_unchanged': True,
              'source_edit_report_sha256': sha(export/'edit-report.json'), 'neck_r2_retained': True}
    (target/'topology-and-staging.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf8')
    if args.stage_only:
        print(json.dumps({'model':str(model_path),'native_capture':'not_run','drawables':len(topologies[1])}))
        return
    command = [str(run/'renderer-build/DesktopCompanion.exe'), '--review-motion',
               str(target/'native'), str(pose_path), 'sit', '--review-model', str(model_path)]
    completed = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True, timeout=90)
    (target/'native-command.json').write_text(json.dumps({'argv': command, 'exit_code': completed.returncode,
                                                         'stdout': completed.stdout, 'stderr': completed.stderr}, indent=2)+'\n', encoding='utf8')
    if completed.returncode: raise RuntimeError(completed.stderr)
    captures = list((target/'native').glob('sit-*.png'))
    assert len(captures) == len(poses), len(captures)
    print(json.dumps({'model': str(model_path), 'capture_dir': str(target/'native'),
                      'captured': len(captures), 'drawables': len(topologies[1])}))


if __name__ == '__main__': main()
