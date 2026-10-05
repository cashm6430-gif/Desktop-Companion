"""Expand geometry-only forms onto axes actually present after public CMO readback."""
import copy
import hashlib
import itertools
import json
from pathlib import Path
import numpy as np

ROOT=Path(__file__).resolve().parents[3]
WORK=Path(__file__).resolve().parent
SOURCE=WORK/'api/closed-legs-r8-complete-20261005T020741Z-17f3fa95/export'
RAW=WORK/'api/ground-left-return-r3/ground-left-return-geometry-plan.json'
OUT=WORK/'api/ground-left-return-r3b'
FULL=WORK/'geometry/closed-legs-r8-complete-plan.json'
def read(p):return json.loads(p.read_text(encoding='utf8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

if OUT.exists():raise FileExistsError(OUT)
source={r['id']:r for r in read(SOURCE/'diagnostic-after-CMO.json')['meshes']}
plan=read(RAW);effective=read(FULL);nodes={r['id']:r for r in effective['insert_meshes']}
records=[]
for patch in plan['mesh_grids']:
    name=patch['id'];node=nodes[name]
    actual=source[name]['geometry']
    actual_axes=[{'parameter':a['getParameterId-WD9NFvw'],'keys':a['getKeys']} for a in actual['getAxes']]
    old_axes=node['axes']
    assert actual_axes[:len(old_axes)]==old_axes
    extras=actual_axes[len(old_axes):]
    if extras:
        assert name.endswith('BridgeL')
        assert [a['parameter'] for a in extras]==['ParamBusyLaptop','ParamHandGround']
        expanded=[]
        for cell in patch['keyforms']:
            for suffix in itertools.product(*(range(len(a['keys'])) for a in extras)):
                expanded.append(dict(cell,coordinate=cell['coordinate']+list(suffix)))
        patch['keyforms']=expanded
    actual_cells={tuple(c['getCoordinate']):c['getForm']['getPositionDeltas'] for c in actual['getCells']}
    maximum_source_delta_rounding=0.
    for cell in patch['keyforms']:
        if cell['coordinate'][0]==0:
            oldcoord=tuple(cell['coordinate'][:len(actual_axes)])
            maximum_source_delta_rounding=max(maximum_source_delta_rounding,float(np.max(np.abs(np.asarray(cell['position_deltas'])-np.asarray(actual_cells[oldcoord])))))
            # Continue the saved author's actual delta representation. CMO
            # readback may normalize base+delta while preserving Native bytes.
            cell['position_deltas']=copy.deepcopy(actual_cells[oldcoord])
            assert np.array_equal(np.asarray(cell['position_deltas'],dtype=np.float32).view(np.uint32),np.asarray(actual_cells[oldcoord],dtype=np.float32).view(np.uint32))
    node['axes']=actual_axes+copy.deepcopy(patch.get('append_axes',[]))
    node['keyforms']=copy.deepcopy(patch['keyforms'])
    records.append({'id':name,'actual_source_axes':actual_axes,'replicated_readback_axes':extras,'target_forms':len(patch['keyforms']),'source_Sit0_delta_float32_bits_exact':True,'maximum_generator_vs_saved_source_delta_normalization':maximum_source_delta_rounding})
plan['provenance']={'source_edit_report':{'path':str(SOURCE/'edit-report.json'),'sha256':sha(SOURCE/'edit-report.json')},'source_generator_plan_sha256':sha(RAW)}
OUT.mkdir()
target=OUT/'ground-left-return-readback-plan.json'
target.write_text(json.dumps(plan,indent=2,allow_nan=False)+'\n',encoding='utf8')
effective.setdefault('provenance',{})['continuation']={'source_report_sha256':sha(SOURCE/'edit-report.json'),'incremental_plan_path':str(target),'incremental_plan_sha256':sha(target),'kind':'reconstructed cumulative source recipe; not separately exported'}
full=OUT/'ground-left-return-effective-plan.json'
full.write_text(json.dumps(effective,indent=2,allow_nan=False)+'\n',encoding='utf8')
(OUT/'readback-grid-adaptation-proof.json').write_text(json.dumps({'kind':'public_saved_CMO_actual_axis_expansion','source_cmo_sha256':sha(SOURCE/'edited.cmo3'),'source_diagnostic_sha256':sha(SOURCE/'diagnostic-after-CMO.json'),'input_plan_sha256':sha(RAW),'output_plan_sha256':sha(target),'effective_plan_sha256':sha(full),'records':records},indent=2)+'\n',encoding='utf8')
print(json.dumps({'plan':str(target),'sha256':sha(target),'effective':str(full),'records':records},indent=2))
