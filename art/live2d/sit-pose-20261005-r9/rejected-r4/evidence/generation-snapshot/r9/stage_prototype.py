"""Stage a non-adopted R9 family, then capture default-domain SDK poses."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
OLD=ROOT/'.local/authoring/sit-pose-20261004-r1'


def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--export',type=Path,required=True)
    ap.add_argument('--label',required=True);a=ap.parse_args()
    run=OLD/a.label
    if run.exists():raise ValueError('Fresh staging required.')
    command=[sys.executable,'-X','utf8',str(OLD/'stage_material_review.py'),
             '--export',str(a.export.resolve()),'--label',a.label,'--stage-only']
    r=subprocess.run(command,cwd=ROOT,check=True,capture_output=True,text=True,encoding='utf8')
    print(r.stdout,flush=True)
    family=run/'assets/live2d/whale-girl'
    model=family/'whale-girl-layered-draft.model3.json'
    meta_path=family/'whale-girl-layered-draft.psd2live.json'
    meta=json.loads(meta_path.read_text(encoding='utf8'));profile=meta['runtimePostureTransition']
    profile['approvedConceptSha256']=sha(ROOT/'art/live2d/review/sit-pose-20261005-r9/skirt-support-concept-r2-approved.png')
    profile['floorReferenceDrawableIds']=['ArtMeshAuthorFloorReferenceL','ArtMeshAuthorFloorReferenceR']
    profile['standingPropRegistration']={'version':1,'owner':'legacy-runtime',
        'drawables':['ArtMeshObjects4','ArtMeshObjects'],'fadeEndSit':.0875,
        'scope':'Nearstanding laptop props only; continuously fades to author identity.'}
    profile['review_scope']='R9 neutral structural prototype, not adopted; full parameter domain pending'
    meta_path.write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    keys=[0.,.35,.65,1.]
    poses=[{'time':float(i),'name':f'R9-Sit{s:.2f}-default',
            'parameters':{'ParamSitPose':s,'ParamBusyLaptop':0.,'ParamLaptopVisible':0.,
                          'ParamArmLA':0.,'ParamArmRA':0.,'ParamBodyAngleX':0.,'ParamBodyAngleY':0.,
                          'ParamBodyAngleZ':0.,'ParamBreath':0.,'ParamBusyTypingL':0.,'ParamLaptopRock':0.}}
           for i,s in enumerate(keys)]
    path=run/'neutral-poses.json';path.write_text(json.dumps({'keyframes':poses},indent=2)+'\n',encoding='utf8')
    exe=OLD/'renderer-build/DesktopCompanion.exe'
    command=[str(exe),'--review-motion',str(run/'neutral-native'),str(path),'sit','--review-model',str(model)]
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=120)
    (run/'neutral-command.json').write_text(json.dumps({'argv':command,'renderer_sha256':sha(exe),
        'source_tool_sha256':sha(Path(__file__)),'model_sha256':sha(model),'pose_sha256':sha(path),
        'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr},indent=2)+'\n',encoding='utf8')
    if result.returncode:raise RuntimeError(result.stderr)
    print(json.dumps({'model':str(model),'capture':str(run/'neutral-native'),'poses':len(poses)},indent=2),flush=True)


if __name__=='__main__':main()
