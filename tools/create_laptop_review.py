"""Build approval artifacts exclusively from native model captures."""
import json
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[1]
def main():
    folder=ROOT/'build/laptop-v1-review'
    folder.mkdir(parents=True,exist_ok=True)
    motion=json.loads((ROOT/'assets/motions/busy-laptop.motion.json').read_text(encoding='utf8'))
    frames=motion['keyframes']
    if '--manifest' in sys.argv or not (folder/'grass-00.png').exists():
        poses=[frames[i] for i in (0,1,3,4)]
        poses.append({'time':9,'label':'连续闭眼','parameters':{**frames[0]['parameters'],'ParamEyeLOpen':0,'ParamEyeROpen':0}})
        poses.append({'time':10,'label':'回到待机','parameters':{'ParamBusyLaptop':0,'ParamEyeLOpen':1,'ParamEyeROpen':1}})
        (folder/'poses.motion.json').write_text(json.dumps({'keyframes':poses},ensure_ascii=False,indent=2),encoding='utf8')
        print(folder/'poses.motion.json');return
    poses=json.loads((folder/'poses.motion.json').read_text(encoding='utf8'))['keyframes']
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',22)
    sheet=Image.new('RGB',(1120,1730),'#e9edf5')
    draw=ImageDraw.Draw(sheet)
    draw.text((24,16),'抱电脑工作 v1 · 实际 Live2D Native 渲染 · 动作待审',font=font,fill='#243654')
    for i,pose in enumerate(poses):
        x,y=(i%2)*560,60+(i//2)*550
        draw.rounded_rectangle((x+10,y+8,x+550,y+540),radius=18,fill='#f8faff')
        sprite=Image.open(folder/f'grass-{i:02}.png').convert('RGBA')
        sprite.thumbnail((490,490),Image.Resampling.LANCZOS)
        sheet.paste(sprite,(x+(560-sprite.width)//2,y+8),sprite)
        draw.text((x+24,y+500),pose['label'],font=font,fill='#243654')
    out=ROOT/'art/live2d/review/busy-laptop-keyframes-v1.png'
    sheet.save(out);print(out)
    sequence=sorted((ROOT/'build/laptop-v1-sequence').glob('frame-*.png'))
    if len(sequence) == 195:
        rgb=[]
        for path in sequence:
            sprite=Image.open(path).convert('RGBA').resize((420,420),Image.Resampling.LANCZOS)
            background=Image.new('RGBA',sprite.size,'#eef3fa')
            background.alpha_composite(sprite)
            rgb.append(background.convert('RGB'))
        # One palette for the full cycle avoids per-frame color flicker.
        overview=Image.new('RGB',(420*len(rgb[::10]),420))
        for i,frame in enumerate(rgb[::10]): overview.paste(frame,(i*420,0))
        palette=overview.quantize(colors=255)
        gif=[frame.quantize(palette=palette) for frame in rgb]
        target=ROOT/'art/live2d/review/busy-laptop-motion-v1.gif'
        gif[0].save(target,save_all=True,append_images=gif[1:],duration=[[70,70,60][i%3] for i in range(len(gif))],loop=0,disposal=2)
        print(target)
if __name__=='__main__':main()
