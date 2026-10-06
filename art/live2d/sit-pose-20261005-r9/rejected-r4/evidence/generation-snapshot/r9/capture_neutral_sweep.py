"""Actual SDK neutral parameter sweep; inspection evidence, not motion adoption."""
from pathlib import Path
import argparse,hashlib,json,subprocess
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[3]
OLD=ROOT/'.local/authoring/sit-pose-20261004-r1'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser();p.add_argument('--model',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve()
    if out.exists():raise ValueError('Fresh sweep output required.')
    out.mkdir(parents=True);model=a.model.resolve(strict=True)
    sits=[float(x) for x in np.linspace(0,1,41)]+[float(x) for x in np.linspace(1,0,41)[1:]]
    rows=[{'name':f'neutral-Sit{s:.3f}','time':i/15.,'parameters':{'ParamSitPose':s,
        'ParamBusyLaptop':1.,'ParamLaptopVisible':0.,'ParamArmLA':0.,'ParamArmRA':0.,
        'ParamBodyAngleX':0.,'ParamBodyAngleY':0.,'ParamBodyAngleZ':0.,'ParamBreath':0.,
        'ParamBusyTypingL':0.,'ParamBusyTypingR':0.,'ParamLaptopRock':0.,'ParamAngleX':0.,'ParamAngleY':0.,'ParamAngleZ':0.,
        'ParamHairFront':0.,'ParamHairBack':0.}} for i,s in enumerate(sits)]
    poses=out/'poses.json';poses.write_text(json.dumps({'keyframes':rows},indent=2)+'\n',encoding='utf8')
    exe=OLD/'renderer-build/DesktopCompanion.exe'
    cmd=[str(exe),'--review-motion',str(out/'native'),str(poses),'sit','--review-model',str(model)]
    r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=240)
    receipt={'argv':cmd,'source_tool_sha256':sha(Path(__file__)),'renderer_sha256':sha(exe),
             'model_sha256':sha(model),'poses_sha256':sha(poses),'exit_code':r.returncode,
             'stdout':r.stdout,'stderr':r.stderr,'scope':'default Body/Arm/Typing/Head/physics frozen parameter sweep; not real event integration'}
    (out/'command.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf8')
    if r.returncode:raise RuntimeError(r.stderr)
    files=sorted((out/'native').glob('sit-*.png'))
    if len(files)!=len(rows):raise ValueError('Missing Native frame.')
    originals=[Image.open(f).convert('RGBA') for f in files];bounds=[im.getchannel('A').getbbox() for im in originals]
    crop=(min(b[0] for b in bounds),min(b[1] for b in bounds),max(b[2] for b in bounds),max(b[3] for b in bounds))
    scale=min(400/(crop[2]-crop[0]),420/(crop[3]-crop[1]))
    size=(round((crop[2]-crop[0])*scale),round((crop[3]-crop[1])*scale));frames=[]
    for im in originals:
        preview=im.crop(crop).resize(size,Image.Resampling.LANCZOS)
        matte=Image.new('RGB',(420,440),(65,69,78));matte.paste(preview,((420-size[0])//2,(440-size[1])//2),preview);frames.append(matte)
    dest=out/'native-neutral-enter-exit.gif'
    frames[0].save(dest,save_all=True,append_images=frames[1:],duration=67,loop=0,disposal=2)
    (out/'gif-sources.json').write_text(json.dumps({'kind':'SDK_evidence_only_common_scale_preview','gif_sha256':sha(dest),
        'png_sha256':{f.name:sha(f) for f in files},'crop_xyxy':crop,'common_scale':scale,
        'material_edit':False,'runtime_motion_approval':False},indent=2)+'\n',encoding='utf8')
    print(json.dumps({'capture':str(out),'frames':len(files),'gif':str(dest)},indent=2),flush=True)


if __name__=='__main__':main()
