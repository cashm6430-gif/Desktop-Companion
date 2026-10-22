import json
from pathlib import Path
from PIL import Image, ImageChops, ImageStat
here=Path(__file__).parent
raw=json.loads((here/'equivalence-check.json').read_text())
rows=[]
for i,pair in enumerate(raw['pairs']):
    a=Image.open(here/'equivalence-native'/f'pose-{i*2:02d}.png').convert('RGBA')
    b=Image.open(here/'equivalence-native'/f'pose-{i*2+1:02d}.png').convert('RGBA')
    row={**pair,'raw_rms_channels':ImageStat.Stat(ImageChops.difference(a,b)).rms,
         'silhouette_mask_changed_pixels':sum(x!=y for x,y in zip(
             a.getchannel('A').point(lambda x:255 if x>=128 else 0).get_flattened_data(),
             b.getchannel('A').point(lambda x:255 if x>=128 else 0).get_flattened_data()))}
    desktop=[]
    for background in ('#e9e9ef','#252b36'):
        planes=[]
        for image in (a,b):
            image=image.resize((280,280),Image.Resampling.LANCZOS)
            plane=Image.new('RGBA',image.size,background)
            plane.alpha_composite(image)
            planes.append(plane.convert('RGB'))
        diff=ImageChops.difference(*planes)
        desktop.append({'background':background,'rms':ImageStat.Stat(diff).rms,
                        'mean_abs':ImageStat.Stat(diff).mean,
                        'max_channel_delta':max(hi for lo,hi in diff.getextrema())})
    row['desktop_composites']=desktop
    rows.append(row)
summary={'strict_pixel_equal':raw['passed'],'pairs':len(rows),
         'exact_pairs':sum(x['changed_pixels']==0 for x in rows),
         'differing_pairs':sum(x['changed_pixels']!=0 for x in rows),
         'max_changed_pixels_840':max(x['changed_pixels'] for x in rows),
         'max_changed_fraction_840':max(x['changed_pixels'] for x in rows)/(840*840),
         'max_silhouette_mask_changed_pixels':max(x['silhouette_mask_changed_pixels'] for x in rows),
         'max_desktop_composite_rms':max(v for r in rows for c in r['desktop_composites'] for v in c['rms']),
         'max_desktop_composite_mean_abs':max(v for r in rows for c in r['desktop_composites'] for v in c['mean_abs']),
         'maintenance_assessment':'Effective keys/timing unchanged; no visible difference in four worst sparse-diff samples on light/dark backgrounds. Not pixel-identical. Existing choreography approval retained.',
         'visual_sample_pair_indices':sorted(range(len(rows)),key=lambda i:rows[i]['changed_pixels'],reverse=True)[:4],
         'limitations':['static requested poses, not Qt state-transition/physics equivalence',
                        'small float-coordinate differences cause sparse raster sample changes'],
         'capture':{k:v for k,v in raw.items() if k not in ('pairs','passed')},'samples':rows}
(here/'equivalence-summary.json').write_text(json.dumps(summary,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:v for k,v in summary.items() if k not in ('samples','capture')},indent=2))
