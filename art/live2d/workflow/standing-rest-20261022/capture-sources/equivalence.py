import json,sys
from pathlib import Path
from PIL import Image,ImageChops
ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tools'))
from review_motion import run_exe
from review_session import digest
manifest=HERE/'stretch-equivalence-poses.json'
poses=json.loads(manifest.read_text(encoding='utf8'))['keyframes']
out=HERE/'equivalence-native'
out.mkdir(exist_ok=False)
receipt={'renderer_sha256':digest(ROOT/'build/DesktopCompanion.exe'),
         'moc_sha256':digest(ROOT/'assets/live2d/whale-girl/whale-girl-layered-draft.moc3'),
         'core_sha256':digest(ROOT/'build/Live2DCubismCore.dll'),
         'manifest_sha256':digest(manifest),'scope':'static_requested_pose_native_capture',
         'comparison':'RGBA pixel equality after Native renderer, 15FPS authored interpolation and all 10 keys',
         'physics_evaluated':False}
run_exe(['--review-motion',out,manifest,'pose'],watch=(out,len(poses),'old/clean Native poses'))
pairs=[]
for i in range(0,len(poses),2):
    a=Image.open(out/f'pose-{i:02d}.png').convert('RGBA')
    b=Image.open(out/f'pose-{i+1:02d}.png').convert('RGBA')
    diff=ImageChops.difference(a,b)
    changed=sum(any(p) for p in diff.getdata())
    pairs.append({'label':poses[i]['label'],'changed_pixels':changed,
                  'max_channel_delta':max(hi for lo,hi in diff.getextrema())})
receipt.update(pairs=pairs,passed=all(x['changed_pixels']==0 for x in pairs))
(HERE/'equivalence-check.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:v for k,v in receipt.items() if k!='pairs'}),flush=True)
if not receipt['passed']: raise SystemExit('Native images differ; do not preserve approval without inspection')
