"""Neutral-body diagnostic: planted contact overrides all free-arm selectors.

This fixes accidental interpolation back to historical stretched Ground forms
when the busy motion drives ArmLA/RA=-40. Typing/Rock contact modes are held
while planted; return wrist modulation still needs its own all-domain gate.
"""
from pathlib import Path
import argparse,copy,hashlib,json

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser();p.add_argument('--plan',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    if a.output.exists():raise ValueError('Fresh plan required.')
    d=json.loads(a.plan.read_text(encoding='utf8'))
    for mesh in d['mesh_grids']:
        if not mesh['id'].startswith('ArtMeshGround'):continue
        result=[]
        for f in mesh['keyforms']:
            old=f['coordinate']
            for arm in range(5):
                tr=[(t,r) for t in range(2) for r in range(3)] if mesh['id'].endswith('L') else [None]
                for value in tr:
                    row=copy.deepcopy(f);row['coordinate'][3]=arm
                    if value is not None:row['coordinate'][5:7]=value
                    result.append(row)
        if len({tuple(f['coordinate']) for f in result})!=len(result):raise ValueError('Duplicate ground form.')
        mesh['keyforms']=result
    d['provenance']['ground_scope_source_plan_sha256']=sha(a.plan)
    d['provenance']['ground_scope_tool_sha256']=sha(Path(__file__))
    d['limits'].append('Planted hands ignore free-arm/Typing/Rock selectors. Modulated busy return and full Body context remain pending.')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(d,separators=(',',':'),allow_nan=False)+'\n',encoding='utf8')
    print(json.dumps({'plan':str(a.output.resolve()),'sha256':sha(a.output)},indent=2))

if __name__=='__main__':main()
