"""Compare a new authored sitting pose with the verified formal capture.

Standing frames must retain decoded RGBA exactly. Sitting frames may change
visually, but controller traces and all shared renderer/motion inputs must
still match. This is an engineering gate, never visual approval or adoption.
"""
from pathlib import Path
import argparse
import hashlib
import json

import numpy as np
from PIL import Image
from review_model_equivalence import load_capture


def parameters(trace):
    if 'source_keyframe' in trace:return trace['source_keyframe'].get('parameters',{})
    return trace.get('requested_parameters',trace.get('parameters',{}))


def compare(reference,candidate,output):
    output=Path(output).resolve()
    if output.exists():raise ValueError('Use a fresh comparison output.')
    reference,left=load_capture(Path(reference));candidate,right=load_capture(Path(candidate))
    same_inputs=left['shared_inputs']==right['shared_inputs']
    inventory=lambda receipt:[(s['kind'],s['name'],s['frames']) for s in receipt['sequences']]
    same_inventory=inventory(left)==inventory(right)
    same_scope=all(left.get(k)==right.get(k) for k in ('scope','fixture','raw_export_only'))
    sequences=[];unexpected=[];traces=[];standing=sitting=changed_sitting=0
    if same_inputs and same_inventory and same_scope:
        for a,b in zip(left['sequences'],right['sequences']):
            if a['trace']!=b['trace']:traces.append(a['folder'])
            counts={'folder':a['folder'],'standing_frames':0,'sitting_frames':0,'changed_sitting_frames':0,'unexpected_standing_changes':0}
            for index,filename in enumerate(a['frames']):
                x=parameters(a['trace'][index]);y=parameters(b['trace'][index])
                unchanged=x.get('ParamSitPose',0)==0 and y.get('ParamSitPose',0)==0
                with Image.open(reference/a['folder']/filename) as picture:old=np.asarray(picture.convert('RGBA'))
                with Image.open(candidate/b['folder']/filename) as picture:new=np.asarray(picture.convert('RGBA'))
                equal=old.shape==new.shape and np.array_equal(old,new)
                if unchanged:
                    standing+=1;counts['standing_frames']+=1
                    if not equal:
                        counts['unexpected_standing_changes']+=1
                        unexpected.append({'frame':a['folder']+'/'+filename,'parameters':x,
                            'changed_pixels':int(np.count_nonzero(np.any(old!=new,axis=2))) if old.shape==new.shape else None})
                else:
                    sitting+=1;counts['sitting_frames']+=1
                    if not equal:changed_sitting+=1;counts['changed_sitting_frames']+=1
            sequences.append(counts)
    result={'schema_version':1,'kind':'authored_sit_native_regression_classification',
        'inputs':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in
            [Path(__file__).resolve(),reference/'capture-receipt.json',candidate/'capture-receipt.json']},
        'shared_inputs_equal':same_inputs,'sequence_inventory_equal':same_inventory,'scope_equal':same_scope,
        'controller_trace_differences':traces,'unchanged_standing_frames':standing,
        'sitting_frames_with_authorized_pose_change':sitting,'changed_sitting_frames':changed_sitting,
        'unexpected_standing_changes':unexpected,'sequences':sequences,
        'engineering_passed_within_scope':same_inputs and same_inventory and same_scope and standing>0 and not traces and not unexpected,
        'visual_review':'pending continuous Native and user pose approval','adopted':False,
        'limit':'Changed sitting pixels are counted, not declared correct. Material coverage and actual Core contact remain independent gates.'}
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({key:result[key] for key in ['engineering_passed_within_scope','unchanged_standing_frames',
        'sitting_frames_with_authorized_pose_change','changed_sitting_frames','controller_trace_differences']}))
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['reference','candidate','output']:p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();return 0 if compare(a.reference,a.candidate,a.output)['engineering_passed_within_scope'] else 1


if __name__=='__main__':raise SystemExit(main())
