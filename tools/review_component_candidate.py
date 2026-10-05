"""One fresh, read-only engineering review for a staged Live2D candidate.

Required evidence: public CMO roundtrip, dense actual Core gate bound to the
same MOC bytes, actual texture/UV inventory, and named joint alpha coverage.
Return zero only when all engineering checks pass. A zero result still leaves
Native motion review, user visual approval and adoption pending.

--output is a new JSON file; a new sibling <stem>.materials directory holds
the component reports. Existing files/directories are never overwritten.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
from datetime import datetime, timezone
import json
import itertools
from pathlib import Path
import re

from probe_cubism_core import digest, model_family
from verify_component_asset_inventory import run as run_inventory
from verify_component_coverage import run as run_coverage


EDIT_KIND='public_parent_local_CMO_edit_closed_loop'
EDIT_KINDS={EDIT_KIND,'public_multipage_parent_local_CMO_edit_closed_loop'}
DENSE_KIND='independent_full_ancestor_raw_core_sit_gate'
CLOSED_LEG_DENSE_KIND='independent_closed_leg_authored_target_raw_core_gate'
EDIT_BOOLEANS=['inputs_rechecked_after','baseline_MOC_byte_exact',
               'direct_vs_saved_CMO_readback_all_runtime_values_exact',
               'untargeted_runtime_nodes_equal','saved_CMO_atlas_binding_matches_declared',
               'readback_page_indices_restored_from_saved_CMO']


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def sha(value):
    return isinstance(value,str) and re.fullmatch(r'[0-9a-fA-F]{64}',value) is not None


def validate_edit_report(document,moc_sha256,texture_sha256):
    if not isinstance(document,dict):return ['Author roundtrip report must be a JSON object.']
    reasons=[]
    if document.get('kind') not in EDIT_KINDS:reasons.append('Unsupported author roundtrip report kind.')
    reasons += [field+' must be exactly true.' for field in EDIT_BOOLEANS if document.get(field) is not True]
    outputs=document.get('output_sha256',{})
    if not isinstance(outputs,dict):outputs={}
    if outputs.get('edited.moc3')!=moc_sha256:reasons.append('Author report edited.moc3 SHA does not match the current candidate MOC.')
    pages=document.get('cmo_exact_embedded_png_pages',[])
    if not isinstance(pages,list) or any(not isinstance(page,dict) for page in pages):
        pages=[];reasons.append('Invalid author embedded PNG page manifest.')
    expected=set(range(len(texture_sha256)))
    if len(pages)!=len(texture_sha256) or any(type(p.get('page')) is not int for p in pages) or {p.get('page') for p in pages}!=expected:
        reasons.append('Author report embedded PNG pages do not cover the current texture pages exactly.')
    for page in pages:
        index=page.get('page')
        if type(index) is not int or not 0<=index<len(texture_sha256):continue
        if page.get('input_png_bytes_exact') is not True:reasons.append(f'CMO embedded texture page {index} is not byte exact.')
        if page.get('png_sha256')!=texture_sha256[index]:reasons.append(f'Author PNG page {index} SHA does not match the current texture.')
    return reasons


def validate_dense_report(document,moc_sha256):
    if not isinstance(document,dict):return ['Dense Core report must be a JSON object.'],[]
    reasons=[]
    if document.get('kind') not in {DENSE_KIND,CLOSED_LEG_DENSE_KIND}:reasons.append('A full ancestor actual Core dense report is required.')
    if document.get('kind')==CLOSED_LEG_DENSE_KIND:
        scope=document.get('scope',{});result=document.get('results',{})
        if scope.get('phase')!='full' or scope.get('ancestor_contexts')!=625 or scope.get('primary_201_sit_poses',0)<125625:
            reasons.append('Closed-leg target evidence must cover the full ancestor/Sit domain.')
        requirements={'original_sit0_bitexact':True,'original_sit0_opacity_float32_bits_exact':True,
                      'new_leg_inversion_occurrences':0,'local_sit_interval_crossing_occurrences':0}
        if any(result.get(key)!=value for key,value in requirements.items()):
            reasons.append('Closed-leg dense geometry or standing preservation failed.')
        for key in ['maximum_shoe_heel_error_source_px','maximum_shoe_vertex_error_to_authored_target_source_px',
                    'maximum_hidden_footwear_minY_difference_to_same_context_sit0_px']:
            value=result.get(key)
            if type(value) not in (int,float) or not 0<=value<.1:reasons.append('Closed-leg strict contact threshold failed: '+key)
        value=result.get('minimum_stocking_length_ratio_to_same_context_sit0')
        if type(value) not in (int,float) or not value>=.995:reasons.append('Closed-leg strict stocking length threshold failed.')
    if document.get('engineering_passed_within_scope') is not True:reasons.append('Dense Core engineering gate did not pass.')
    inputs=document.get('inputs',{})
    if not isinstance(inputs,dict):inputs={}
    bindings=[{'path':path,'sha256':value.get('sha256') if isinstance(value,dict) else value}
              for path,value in inputs.items() if str(path).lower().endswith('.moc3')]
    if not any(binding['sha256']==moc_sha256 for binding in bindings):
        reasons.append('Dense report inputs contain no matching current candidate MOC SHA.')
    return reasons,bindings


def report_reference(path,document):
    return {'path':str(path),'sha256':digest(path),'kind':document.get('kind')}


def source_ground_nodes(plan,ids,pins,depth=0):
    """Reconstruct a geometry continuation from its verified source export.

    This permits direct multipage CMO edits while keeping the fixture tied to
    the full original charts and every later geometry replacement.
    """
    children={child['id']:copy.deepcopy(child) for child in plan.get('insert_meshes',[])}
    if all(name in children for name in ids):return children
    if depth>=8:raise ValueError('Author continuation chain exceeds eight exports.')
    binding=plan.get('provenance',{}).get('source_edit_report',{})
    path=Path(binding.get('path',''))
    if not path.is_file() or digest(path)!=binding.get('sha256'):
        raise ValueError('Incremental author plan needs its hash-bound source edit report.')
    pins[str(path.resolve())]=digest(path);report=read(path)
    if report.get('kind') not in EDIT_KINDS or any(report.get(k) is not True for k in EDIT_BOOLEANS):
        raise ValueError('Source author export did not pass its closed-loop gates.')
    for filename,key in [('edited.cmo3','cmo_sha256'),('edited.moc3','moc_sha256')]:
        artifact=path.parent/filename;expected=plan.get('source',{}).get(key)
        if report.get('output_sha256',{}).get(filename)!=expected or not artifact.is_file() or digest(artifact)!=expected:
            raise ValueError('Incremental source author artifact is stale: '+filename)
        pins[str(artifact.resolve())]=expected
    sources=[]
    for filename,pin in report.get('input_sha256',{}).items():
        source=Path(filename)
        if not source.is_file() or digest(source)!=pin:raise ValueError('Source export input is stale: '+filename)
        pins[str(source.resolve())]=pin
        if filename.lower().endswith('-plan.json'):sources.append(read(source))
    if len(sources)!=1:raise ValueError('Source export needs one hash-bound author plan.')
    children=source_ground_nodes(sources[0],ids,pins,depth+1)
    for patch in plan.get('mesh_grids',[]):
        if patch['id'] not in ids:continue
        if set(patch)-{'id','append_axes','keyforms'}:
            raise ValueError('Ground continuation supports complete geometry replacements only.')
        node=children[patch['id']]
        axes=node['axes']+copy.deepcopy(patch.get('append_axes',[]))
        if len({a['parameter'] for a in axes})!=len(axes):raise ValueError('Duplicate continued Ground axis.')
        coordinates={tuple(v) for v in itertools.product(*(range(len(a['keys'])) for a in axes))}
        cells=patch.get('keyforms',[])
        if len(cells)!=len(coordinates) or {tuple(c['coordinate']) for c in cells}!=coordinates:
            raise ValueError('Continued Ground replacement must cover its complete Cartesian grid.')
        node['axes']=axes;node['keyforms']=copy.deepcopy(cells)
    return children


def validate_fixture_motion(fixture_document,edit_document):
    """A changed sleeve/palm trajectory must not reuse old opacity-phase ROIs."""
    ids=[name for name in fixture_document.get('expected_topology',{}) if name.startswith('ArtMeshGround')]
    if not ids:return [],{},{}
    reasons=[];pins={};plans=[]
    binding=fixture_document.get('author_motion_binding',{})
    if binding.get('kind')=='candidate-moc-pinned':
        moc=Path(binding.get('moc_path',''));expected=edit_document.get('output_sha256',{}).get('edited.moc3')
        if not sha(expected) or binding.get('moc_sha256')!=expected or not moc.is_file() or digest(moc)!=expected:
            reasons.append('Fixture actual candidate MOC binding is missing or stale.')
        elif fixture_document.get('inputs',{}).get(str(moc))!=expected:
            reasons.append('Fixture candidate MOC must also be an explicit hashed input.')
        else:pins[str(moc.resolve())]=expected
    for label,records in [('fixture',fixture_document.get('inputs',{})),('current_author',edit_document.get('input_sha256',{}))]:
        found=[]
        for name,pin in records.items():
            path=Path(name)
            if not str(name).lower().endswith('-plan.json'):continue
            if not path.is_file() or digest(path)!=pin:
                reasons.append(label+' author plan evidence is missing or stale: '+name);continue
            document=read(path);children={child['id']:child for child in document.get('insert_meshes',[])}
            if label=='current_author' and not all(name in children for name in ids):
                if binding.get('kind')!='candidate-moc-pinned':
                    reasons.append('Regenerated incremental-author fixture must bind the actual candidate MOC.');continue
                try:children=source_ground_nodes(document,ids,pins)
                except (ValueError,KeyError,TypeError,OSError) as error:
                    reasons.append(str(error));continue
            if all(name in children for name in ids):
                found.append((path,children));pins[str(path.resolve())]=pin
        if len(found)!=1:reasons.append(label+' needs one hash-bound author plan containing all ground sleeve/palm components.')
        plans.append(found)
    fingerprints={}
    if all(len(found)==1 for found in plans):
        fields=['parent_id','part_id','texture_page','positions','uvs','indices','axes','keyforms','channels','draw_order','opacity']
        for name in ids:
            values=[]
            for found in plans:
                node=found[0][1][name]
                canonical=json.dumps({field:node.get(field) for field in fields},sort_keys=True,separators=(',',':'),allow_nan=False).encode()
                values.append(hashlib.sha256(canonical).hexdigest())
            fingerprints[name]={'fixture_author':values[0],'current_author':values[1]}
            if values[0]!=values[1]:reasons.append('Ground motion/material changed; regenerate hover/ground/return fixture instead of reusing old opacity thresholds: '+name)
    return reasons,pins,fingerprints


def review(model,metadata,fixture,edit_report,dense_report,output):
    model,metadata,fixture,edit_report,dense_report,output=map(lambda p:Path(p).resolve(),[model,metadata,fixture,edit_report,dense_report,output])
    material_dir=output.parent/(output.stem+'.materials')
    inputs=[model,metadata,fixture,edit_report,dense_report]
    if output in inputs:raise ValueError('The review output cannot overwrite an input.')
    if output.exists() or material_dir.exists():raise ValueError('Use a fresh --output; existing gate evidence is never overwritten.')
    output.parent.mkdir(parents=True,exist_ok=True);material_dir.mkdir(exist_ok=False)
    pins={str(Path(__file__).resolve()):digest(__file__)}
    pin_conflicts=[]
    def merge_pins(extra):
        for path,pin in extra.items():
            if path in pins and pins[path]!=pin:pin_conflicts.append({'path':path,'before':pins[path],'after':pin})
            else:pins[path]=pin
    stages=[];references={};moc_sha256=None
    for path in inputs:
        if path.is_file():pins[str(path)]=digest(path)
    try:
        moc,family=model_family(model)
        merge_pins({row['path']:row['sha256'] for row in family})
        moc_sha256=digest(moc)
        metadata_document=read(metadata);edit_document=read(edit_report);dense_document=read(dense_report)
        if not isinstance(metadata_document,dict):raise ValueError('Material metadata must be a JSON object.')
        model_document=read(model)
        texture_hashes=[digest((model.parent/reference).resolve()) for reference in model_document['FileReferences']['Textures']]
        edit_reasons=validate_edit_report(edit_document,moc_sha256,texture_hashes)
        # Recheck the actual roundtrip MOC artifact as well as its recorded SHA.
        exported_moc=edit_report.parent/'edited.moc3'
        if not exported_moc.is_file() or digest(exported_moc)!=moc_sha256:
            edit_reasons.append('The author report sibling edited.moc3 artifact is missing or stale.')
        else:pins[str(exported_moc)]=digest(exported_moc)
        stages.append({'id':'public_author_roundtrip','passed':not edit_reasons,'reasons':edit_reasons})
        dense_reasons,dense_bindings=validate_dense_report(dense_document,moc_sha256)
        stages.append({'id':'dense_actual_Core','passed':not dense_reasons,'reasons':dense_reasons,'MOC_input_bindings':dense_bindings})
        references['author_roundtrip']=report_reference(edit_report,edit_document)
        references['dense_Core']=report_reference(dense_report,dense_document)
        motion_reasons,motion_pins,motion_fingerprints=validate_fixture_motion(read(fixture),edit_document)
        merge_pins(motion_pins)
        stages.append({'id':'fixture_author_motion_binding','passed':not motion_reasons,'reasons':motion_reasons,
                       'ground_material_motion_fingerprints':motion_fingerprints})
    except (OSError,ValueError,KeyError,TypeError,RuntimeError,AttributeError) as error:
        metadata_document={}
        stages.append({'id':'input_provenance','passed':False,'error_type':type(error).__name__,'reasons':[str(error)]})

    for name,runner,filename,field in [('texture_UV_mask_inventory',run_inventory,'component-asset-inventory.json','structural_passed'),
                                       ('named_joint_material_coverage',run_coverage,'component-coverage.json','passed')]:
        path=material_dir/filename
        try:
            document=(runner(model,path,metadata=metadata) if runner is run_inventory
                      else runner(model,fixture,path,metadata=metadata))
            stage={'id':name,'passed':document.get(field) is True,'report':report_reference(path,document)}
            if document.get(field) is not True:stage['reasons']=['The required component gate failed; see its report.']
            stages.append(stage);references[name]=stage['report'];merge_pins(document.get('inputs',{}))
            merge_pins({str(path):digest(path)})
        except (OSError,ValueError,KeyError,TypeError,RuntimeError,AttributeError) as error:
            stages.append({'id':name,'passed':False,'error_type':type(error).__name__,'reasons':[str(error)]})

    neck=metadata_document.get('runtimeNeckConnection',{}).get('independentSurface')
    if neck:
        path=material_dir/'independent-neck-asset-inventory.json'
        try:
            neck_model=(metadata.parent/neck['model']).resolve();neck_document=read(neck_model)
            for reference,key in [(neck_document['FileReferences']['Moc'],'mocSha256'),
                                  (neck_document['FileReferences']['Textures'][0],'atlasSha256')]:
                asset=(neck_model.parent/reference).resolve();pins[str(asset)]=digest(asset)
                if not sha(neck.get(key)) or pins[str(asset)]!=neck[key]:raise ValueError('Runtime independent neck asset SHA is stale: '+key)
            rig=(metadata.parent/neck['rig']).resolve();pins[str(rig)]=digest(rig)
            if pins[str(rig)]!=neck.get('rigSha256'):raise ValueError('Runtime independent neck rig SHA is stale.')
            document=run_inventory(neck_model,path)
            stages.append({'id':'independent_neck_asset_inventory','passed':document.get('structural_passed') is True,
                           'report':report_reference(path,document),'final_mapped_surface_verified':False})
            references['independent_neck_assets']=report_reference(path,document);merge_pins(document.get('inputs',{}));merge_pins({str(path):digest(path)})
        except (OSError,ValueError,KeyError,TypeError,RuntimeError,AttributeError) as error:
            stages.append({'id':'independent_neck_asset_inventory','passed':False,'error_type':type(error).__name__,'reasons':[str(error)]})
    changed=[]
    for path,pin in pins.items():
        try:
            if digest(path)!=pin:changed.append(path)
        except OSError:changed.append(path)
    stages.append({'id':'inputs_stable_after_review','passed':not changed and not pin_conflicts,
                   'changed_inputs':changed,'cross_stage_pin_conflicts':pin_conflicts})
    passed=bool(stages) and all(stage['passed'] is True for stage in stages)
    gate={'schema_version':1,'kind':'component_candidate_engineering_review',
          'created_utc':datetime.now(timezone.utc).isoformat(),'model':str(model),'candidate_moc_sha256':moc_sha256,
          'inputs':pins,'reports':references,'stages':stages,'engineering_passed_within_scope':passed,
          'status':'engineering_passed_visual_pending' if passed else 'blocked_engineering',
          'adopted':False,'adoption':'not_adopted','automatic_adoption':False,
          'visual_review':{'Native_continuous_motion':'pending','user_visual_approval':'pending',
                           'independent_neck_final_mapping':'pending','rendered_pixels_verified':False},
          'required_remaining':['Native continuous motion with actual runtime floor/neck/physics/AA/clipping.','User visual approval of approved poses and transitions.','Explicit adoption after the required gates.'],
          'scope':'Author/MOC/PNG byte binding, full dense engineering evidence, actual UV alpha/Parts/masks, named semantic joint coverage. No GPU capture or asset adoption.'}
    with output.open('x',encoding='utf8') as stream:json.dump(gate,stream,indent=2,allow_nan=False);stream.write('\n')
    return gate


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for field in ['model','material-metadata','fixture','edit-report','dense-report','output']:
        parser.add_argument('--'+field,type=Path,required=True)
    args=parser.parse_args()
    try:gate=review(args.model,args.material_metadata,args.fixture,args.edit_report,args.dense_report,args.output)
    except (OSError,ValueError) as error:
        print(json.dumps({'status':'blocked_engineering','adopted':False,'error':str(error)}));return 1
    print(json.dumps({'status':gate['status'],'adopted':False,'engineering_passed_within_scope':gate['engineering_passed_within_scope'],
                      'failed_stages':[stage['id'] for stage in gate['stages'] if not stage['passed']],'report':str(args.output.resolve())}))
    return 0 if gate['engineering_passed_within_scope'] else 1


if __name__=='__main__':raise SystemExit(main())
