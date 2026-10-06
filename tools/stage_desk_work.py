"""Stage a pending desk candidate without replacing production model assets."""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import random
import shutil

from probe_cubism_core import NativeModel, find_core

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf8')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--export',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--backdrop-cleanup',action='store_true',
                    help='Review duplicate rear-hair islands and the contaminated arm backing.')
    args = ap.parse_args()
    export, output = args.export.resolve(strict=True), args.output.resolve()
    if output.exists():
        raise ValueError('Fresh output required.')
    formal = ROOT/'assets/live2d/whale-girl'
    old = NativeModel(find_core(),formal/'whale-girl-layered-draft.moc3')
    new = NativeModel(find_core(),export/'edited.moc3')
    desk_ids = {'ArtMeshWorkstationDesk','ArtMeshWorkstationDeskTop'}
    sleeve_replacements = {'ArtMeshObjects2':'ArtMeshWorkstationSleeveR'}
    sleeve_ids = set(sleeve_replacements.values())
    added = set(new.drawable_ids)-set(old.drawable_ids)
    complete_laptop = 'ArtMeshWorkstationLaptop' in added
    complete_hands = bool(added & sleeve_ids)
    expected = desk_ids | ({'ArtMeshWorkstationLaptop'} if complete_laptop else set())
    if complete_hands:
        if not complete_laptop:
            raise ValueError('The complete right arm requires the complete workstation laptop.')
        expected |= sleeve_ids
    if added != expected:
        raise ValueError('Only the desk, complete laptop, and complete right arm may be added.')
    if set(new.parameter_ids)-set(old.parameter_ids) != {'ParamDeskVisible'}:
        raise ValueError('Only the desk parameter may be added.')
    report = json.loads((export/'edit-report.json').read_text(encoding='utf8'))
    assert report['untargeted_runtime_nodes_equal']
    assert report['direct_vs_saved_CMO_readback_all_runtime_values_exact']
    assert sha(export/'texture_00.png') == sha(formal/'whale-girl-layered-draft.4096/texture_00.png')
    pairs = [(old.drawable_ids.index(name),new.drawable_ids.index(name)) for name in old.drawable_ids]
    for i,j in pairs:
        assert old.vertex_counts[i] == new.vertex_counts[j]
        assert old.index_counts[i] == new.index_counts[j]
        for field,size in [('DrawableVertexUvs',old.vertex_counts[i]*8),('DrawableIndices',old.index_counts[i]*2)]:
            assert c.string_at(old.get(field)[i],size) == c.string_at(new.get(field)[j],size)
    rng = random.Random(20261006)
    poses = [{}]+[{name:rng.uniform(row['min'],row['max']) for name,row in old.ranges.items()}
                  for _ in range(160)]
    for params in poses:
        old.update({'parameters':params,'part_opacities':{}})
        for desk in [0.,.5,1.]:
            new.update({'parameters':{**params,'ParamDeskVisible':desk},'part_opacities':{}})
            for i,j in pairs:
                assert c.string_at(old.get('DrawableVertexPositions')[i],old.vertex_counts[i]*8) == c.string_at(new.get('DrawableVertexPositions')[j],new.vertex_counts[j]*8)
                assert c.string_at(c.byref(old.get('DrawableOpacities').contents,i*4),4) == c.string_at(c.byref(new.get('DrawableOpacities').contents,j*4),4)
            if complete_hands:
                busy = new.get('ParameterValues')[new.parameter_ids.index('ParamBusyLaptop')]
                for identifier in sleeve_ids:
                    opacity = new.get('DrawableOpacities')[new.drawable_ids.index(identifier)]
                    assert abs(opacity-desk*busy) < 1e-6
    order = new.get(new.render_orders_field)
    desk_order = order[new.drawable_ids.index('ArtMeshWorkstationDeskTop')]
    assert order[new.drawable_ids.index('ArtMeshObjects5')] < desk_order < order[new.drawable_ids.index('ArtMeshObjects4')]
    assert order[new.drawable_ids.index('ArtMeshObjects')] < order[new.drawable_ids.index('ArtMeshWorkstationDesk')]
    old_order = old.get(old.render_orders_field)
    assert sorted(old.drawable_ids,key=lambda x:old_order[old.drawable_ids.index(x)]) == sorted(old.drawable_ids,key=lambda x:order[new.drawable_ids.index(x)])
    family = output/'assets/live2d/whale-girl'
    shutil.copytree(formal,family)
    moc = family/'whale-girl-layered-draft.moc3'
    shutil.copyfile(export/'edited.moc3',moc)
    model_path = family/'whale-girl-layered-draft.model3.json'
    model = json.loads(model_path.read_text(encoding='utf8'))
    exported = json.loads((export/'edited.model3.json').read_text(encoding='utf8'))
    model['FileReferences']['Textures'] = []
    for i,name in enumerate(exported['FileReferences']['Textures']):
        target = f'whale-girl-layered-draft.4096/texture_{i:02d}.png'
        shutil.copyfile(export/name,family/target)
        model['FileReferences']['Textures'].append(target)
    write(model_path,model)
    ownership_path = family/'material-ownership/ownership.json'
    ownership = json.loads(ownership_path.read_text(encoding='utf8'))
    ownership['moc_sha256'] = sha(moc)
    ownership['moc_rebinding_evidence'] = {'old34_UV_and_indices_exact':True,
        'source_atlas_byte_exact':True,'source_edit_report_sha256':sha(export/'edit-report.json')}
    write(ownership_path,ownership)
    metadata_path = family/'whale-girl-layered-draft.psd2live.json'
    metadata = json.loads(metadata_path.read_text(encoding='utf8'))
    assert 'runtimePostureTransition' not in metadata
    metadata['runtimeDeskWorkMode'] = {'version':1,'stage':'preview','adopted':False,
        'mocSha256':sha(moc),'parameter':'ParamDeskVisible','drawableIds':sorted(desk_ids),
        'floorOwner':'runtime','bodySource':'formal-equivalent master; legacy approved registration retained'}
    if complete_laptop:
        metadata['runtimeDeskWorkMode']['replacementLaptop'] = 'ArtMeshWorkstationLaptop'
        metadata['layers'].append({'source':'busy workstation laptop','drawable':'ArtMeshWorkstationLaptop',
            'parameter':'ParamLaptopVisible','type':'scene-prop'})
        hardware_order = order[new.drawable_ids.index('ArtMeshWorkstationLaptop')]
        assert desk_order < hardware_order < order[new.drawable_ids.index('ArtMeshObjects3')]
    if complete_hands:
        metadata['runtimeDeskWorkMode']['replacementSleeves'] = sleeve_replacements
        metadata['runtimeDeskWorkMode']['hiddenLeftHand'] = True
        for side,original in [('R','ArtMeshObjects2')]:
            identifier = sleeve_replacements[original]
            metadata['layers'].append({'source':'busy workstation sleeve '+side.lower(),
                'drawable':identifier,'parameter':'ParamBusyLaptop','type':'toggle',
                'tag':'objects','side':'NONE','switchId':0})
        right_order = order[new.drawable_ids.index('ArtMeshWorkstationSleeveR')]
        assert desk_order < hardware_order < right_order < order[new.drawable_ids.index('ArtMeshWorkstationDesk')]
        assert order[new.drawable_ids.index('ArtMeshObjects3')] < right_order < order[new.drawable_ids.index('ArtMeshObjects2')]
        assert len(new.parameter_ids) == 41 and len(new.drawable_ids) == 38
        assert len(model['FileReferences']['Textures']) == 4
    for identifier in sorted(desk_ids):
        metadata['layers'].append({'source':'workstation '+identifier,'drawable':identifier,
            'parameter':'ParamDeskVisible','type':'scene-prop'})
    if args.backdrop_cleanup:
        metadata['runtimeBackdropCleanup'] = {'version':1,'mocSha256':sha(moc),
            'atlasSha256':sha(formal/'whale-girl-layered-draft.4096/texture_00.png'),
            'backHairOnly':'ArtMeshBackHair2','backingMaterial':'ArtMeshBackHair',
            'keepStrongComponents':3,'supportScale':[1.7,1.25],'status':'experimental_failed_visual'}
        backing = ROOT/'art/live2d/desk-work-20261006-r1/arm-backing-hair-only.png'
        generation = json.loads((backing.parent/'generation-hair-backing.json').read_text(encoding='utf8'))
        assert sha(backing) == generation['sha256'] and generation['source_size'] == [1254,1254]
        target = family/'material-ownership/arm-backing-hair-only.png'
        shutil.copyfile(backing,target)
        metadata['runtimeBackdropCleanup']['backingTexture'] = {'file':'material-ownership/arm-backing-hair-only.png',
            'sha256':sha(backing),'sourceRects':generation['generated_wing_alpha_bbox']}
    write(metadata_path,metadata)
    record = {'schema_version':1,'status':'pending','adopted':False,
        'source_moc_sha256':sha(formal/'whale-girl-layered-draft.moc3'),
        'candidate_moc_sha256':sha(moc),'source_edit_report_sha256':sha(export/'edit-report.json'),
        'old34_geometry_opacity_bit_exact_contexts':len(poses)*3,'old34_topology_exact':True,
        'old34_relative_render_order_exact':True,'desk_between_legs_and_laptop':True,
        'native_counts':{'parameters':len(new.parameter_ids),'meshes':len(new.drawable_ids),'pages':len(model['FileReferences']['Textures'])},
        'visual_review':'pending','full_native_regression':'not_run','production_replaced':False}
    if complete_hands:
        record['replacementSleeves'] = sleeve_replacements
        record['hiddenLeftHand'] = True
        record['complete_sleeves_DeskVisible_times_BusyLaptop_exact_contexts'] = len(poses)*3
        record['complete_prop_render_order_verified'] = True
    write(output/'staging-report.json',record)
    poses = [
        {'time':0,'name':'default-hidden','parameters':{}},
        {'time':1,'name':'standing-covered','parameters':{'ParamDeskVisible':1}},
        {'time':2,'name':'working','parameters':{'ParamDeskVisible':1,'ParamBusyLaptop':1,'ParamSitPose':1,
            'ParamLaptopVisible':1,'ParamArmLA':-40,'ParamArmRA':-40,'ParamAngleY':-8}},
        {'time':3,'name':'working-glance','parameters':{'ParamDeskVisible':1,'ParamBusyLaptop':1,'ParamSitPose':1,
            'ParamLaptopVisible':1,'ParamArmLA':-40,'ParamArmRA':-40,'ParamAngleZ':-7,'ParamEyeBallX':.5}}
    ]
    write(output/'keyposes.json',{'keyframes':poses})
    print(json.dumps({'model':str(model_path),**record},ensure_ascii=False))


if __name__ == '__main__':
    main()
