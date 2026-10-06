"""Fresh public master-grid restore and hidden-floor-clone experiment."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,subprocess,sys,uuid

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
PYTHON=Path(sys.executable)
sys.path.insert(0,str(ROOT/'.local/authoring/sit-pose-20261004-r1/geometry'))
import generate_first_geometry as s

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    source=ROOT/'.local/authoring/sit-pose-20261004-r1/api/closed-legs-r8-return-r3b-20261005T060828Z-abca9aff/export'
    master=ROOT/'art/live2d/whale-girl-layered-draft.cmo3'
    master_export=ROOT/'.local/authoring/whole-model-migration-20261004-r1/api-audit/moc-recovery-20261004T082620Z/export'
    reference=master_export/'cmo-parent-corrected.moc3'
    atlas=master_export/'texture_00.png'
    run=HERE/f'probe-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{uuid.uuid4().hex[:8]}'
    run.mkdir()
    for name in ('classes','temp'):(run/name).mkdir()
    current_model=json.loads((source/'edited.model3.json').read_text())
    pages=[source/name for name in current_model['FileReferences']['Textures']]
    rows=[]
    for identifier in ['ArtMeshFootwearL','ArtMeshFootwearR','ArtMeshObjects5','ArtMeshObjects6','ArtMeshBottomwear']:
        n=s.D[identifier];old=n['getChannelGrids']['getGridsByChannel'].get('OPACITY')
        cells=old['getCells'] if old else [{'getCoordinate':[],'getForm':{'getValue':n['getOpacity']}}]
        channel={'channel':'OPACITY','append_axes':[{'parameter':'ParamSitPose','keys':[0.,.0875,1.]}],
                 'keyforms':[{'coordinate':c['getCoordinate']+[i], 'value':c['getForm']['getValue']*(1 if i==0 else 0)} for c in cells for i in range(3)]}
        if not old:channel['initial_value']=n['getOpacity']
        rows.append({'id':identifier,'channels':[channel]})
    plan={'schema_version':2,
          'source':{'cmo_sha256':sha(source/'edited.cmo3'),'moc_sha256':sha(source/'edited.moc3'),
                    'atlas_pages':[{'path':str(p),'sha256':sha(p)} for p in pages]},
          'master_source':{'cmo':str(master),'cmo_sha256':sha(master),'moc':str(reference),'moc_sha256':sha(reference),'atlas':str(atlas),'atlas_sha256':sha(atlas)},
          'restore_meshes':rows,
          'append_pages':[{'path':str(atlas),'sha256':sha(atlas)}],
          'floor_clones':[{'id':'ArtMeshAuthorFloorReference'+side,'name':'Invisible author floor '+side,'source_id':'ArtMeshFootwear'+side,'part_id':'PartExtra','texture_page':len(pages)} for side in ['L','R']],
          'authoredSitPose':{'floorReferenceDrawableIds':['ArtMeshAuthorFloorReferenceL','ArtMeshAuthorFloorReferenceR'],
                            'floorReferenceContract':'Sit0 reads original FootwearL/R; positive Sit reads hidden author plates. Renderer common matrix only.'},
          'description':'Experiment only: restore original old5 geometry/all source channels; minimal Sit opacity; current expanded geometry moves to forever-hidden floor plates.'}
    planpath=run/'standing-restore-plan.json';planpath.write_text(json.dumps(plan,indent=2)+'\n',encoding='utf8')
    jdk=ROOT/'.local/toolchains/jdk-21.0.12.1+1/bin'
    library='D:/tool/psd2live/app/*';java=HERE/'PSD2LiveStandingRestore.java'
    cmds=[[str(jdk/'javac.exe'),'-encoding','UTF-8','-cp',library,'-d',str(run/'classes'),str(java)],
          [str(jdk/'java.exe'),'-Dpsd2live.agent.store='+str(run/'store'),'-Djava.io.tmpdir='+str(run/'temp'),'-cp',str(run/'classes')+';'+library,'PSD2LiveStandingRestore','--install','D:/tool/psd2live','--cmo',str(source/'edited.cmo3'),'--reference-moc',str(source/'edited.moc3'),'--edit-json',str(planpath),'--output',str(run/'export'),'--store',str(run/'store')]]
    (run/'commands.json').write_text(json.dumps({'argv':cmds,'helper_sha256':sha(java),'driver_sha256':sha(Path(__file__)),'plan_sha256':sha(planpath)},indent=2)+'\n',encoding='utf8')
    print('RUN '+str(run),flush=True)
    for i,cmd in enumerate(cmds):
        r=subprocess.run(cmd,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=600)
        (run/f'step-{i}.log').write_text(r.stdout+r.stderr,encoding='utf8')
        print(r.stdout+r.stderr,flush=True)
        if r.returncode:raise SystemExit(r.returncode)
    print('RESTORE_PROBE_PASS '+str(run/'export'),flush=True)

if __name__=='__main__':main()
