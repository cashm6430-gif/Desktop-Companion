"""Registered seated body pieces and small independent typing keyforms."""
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]

def seated_layers(size, preview, manifest):
    concept = Image.open(ROOT / 'art/live2d/whale-girl-busy-laptop-concept-v1.png').convert('RGBA')
    plate = Image.open(ROOT / 'art/live2d/whale-girl-busy-laptop-clean-plate-v1.png').convert('RGBA')
    if concept.size != size or plate.size != size:
        raise ValueError('Seated masks require the reviewed 1254 canvas')
    def mask(points, expand=0):
        m = Image.new('L', size, 0)
        ImageDraw.Draw(m).polygon(points, fill=255)
        return np.asarray(m.filter(ImageFilter.MaxFilter(expand*2+1)) if expand else m) > 0
    lid = mask([(344,721),(650,729),(693,896),(394,890)])
    base = mask([(390,846),(738,837),(740,870),(685,908),(391,903)])
    right = mask([(711,695),(765,711),(798,745),(819,794),(822,867),
                  (805,909),(761,910),(739,891),(726,874),(690,875),
                  (675,856),(670,818),(695,802),(708,777)])
    left = mask([(499,699),(520,747),(422,804),(410,889),(372,889),
                 (345,873),(342,813),(393,756),(445,720)])
    skirt = mask([(436,811),(501,779),(740,779),(796,812),(872,877),
                  (982,1010),(927,1114),(337,1114),(221,1030),(267,949)])
    leg_l = mask([(448,895),(529,899),(557,959),(528,1057),(461,1105),
                  (489,1172),(407,1212),(320,1186),(304,1138),(353,1039),(402,955)])
    leg_r = mask([(582,899),(649,899),(702,964),(690,1033),(668,1075),
                  (680,1154),(646,1214),(562,1227),(490,1163),(495,1113),(546,1035),(570,953)])
    # Legs are separated from the skirt along the painted stocking/skin contour,
    # not broad rectangles containing moving lace. Overlap hidden seams only.
    torso = mask([(520,646),(718,646),(750,797),(794,836),(759,897),
                   (470,897),(432,836),(494,785)])
    defs = [('busy laptop lid', concept, lid),
            ('busy sleeve r',concept,right & ~lid),
            ('busy sleeve l',concept,left & ~lid),
            ('busy laptop base',concept,base & ~lid & ~right & ~left),
            ('busy leg l',plate,leg_l),('busy leg r',plate,leg_r),
            ('busy skirt',plate,skirt & ~(leg_l | leg_r)),
            ('busy torso',plate,torso)]
    result=[]
    for name, art, selected in defs:
        p=np.asarray(art).copy()
        p[~selected]=0
        # Register neck/chin to the existing approved head, never replace eyes.
        full=Image.new('RGBA',size,(0,0,0,0))
        full.alpha_composite(Image.fromarray(p),(0,-80))
        full.save(preview/(name.replace(' ','-')+'.png'))
        manifest[name]=name.replace(' ','-')+'.png'
        result.append((name,full))
    return result

def laptop_operations(name, bounds, laptop, typing_l, typing_r, rock):
    x,y,w,h=bounds
    ops=[]
    # All pieces inherit the body's breath deformer. Movement here is in its
    # square canvas units, with the same pivot for prop and supporting hands.
    if name.startswith('busy laptop') or name.startswith('busy sleeve'):
        pivot=[(540-x)/w,(808-y)/h]
        ops += [{'type':'rotate','pivot':pivot,'degrees':rock*2.0}]
    if name.startswith('busy sleeve'):
        typing=typing_r if name.endswith('r') else typing_l
        ops.append({'type':'curve','axis':'y','controls':[0,0,-8*typing/h,-8*typing/h]})
    # The seated painting has no hidden fabric for an extended leg. Keep its
    # approved pose intact; only the standing legs perform the knee bend.
    if not ops: ops=[{'type':'translate','delta':[0,0]}]
    return ops

def leg_operations(bounds, aspect, side, amount):
    x,y,w,h=bounds
    left=side=='l'
    hip=(570,955) if left else (686,955)
    knee=(570,1035) if left else (690,1035)
    pivot=[(hip[0]-x)/w,(hip[1]-y)/h]
    knee_pivot=[(hip[0]+aspect*(knee[0]-hip[0])-x)/w,(knee[1]-y)/h]
    # Selection is measured in the undeformed mesh domain. Feather only the
    # knee seam, with rectangle edges outside the shoe to keep the sole rigid.
    calf={'rect':[-1,(knee[1]-12-y)/h,2,2],'feather':24/h}
    ops=[{'type':'scale','pivot':pivot,'factors':[aspect,1]}]
    ops += [{'type':'scale','pivot':knee_pivot,
             'factors':[1,1-(0.1 if left else 0.35)*amount],'selection':calf},
            {'type':'rotate','pivot':knee_pivot,'degrees':30*amount,'selection':calf},
            {'type':'rotate','pivot':pivot,'degrees':(25 if left else 20)*amount},
            {'type':'scale','pivot':pivot,'factors':[1/aspect,1]}]
    return ops
