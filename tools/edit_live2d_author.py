"""Run the pinned public CMO editor in a fresh independent JVM/workspace.

This driver creates unique output/store/temp/classes paths.  It accepts an
existing multipage author CMO directly; it does not regenerate a PSD or replay
the former primary-CMO plan.  See schema2 source.atlas_pages in the JSON recipe.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

API = Path(__file__).resolve().parent
ROOT = API.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--cmo', type=Path, required=True)
    parser.add_argument('--reference-moc', type=Path, required=True)
    parser.add_argument('--install', type=Path, default=Path('D:/tool/psd2live'))
    parser.add_argument('--jdk', type=Path, default=ROOT/'.local/toolchains/jdk-21.0.12.1+1/bin')
    parser.add_argument('--run-root', type=Path, default=ROOT/'.local/authoring/public-cmo-edit')
    parser.add_argument('--name', default='public-multipage-edit')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z][a-z0-9-]{0,63}', args.name):
        parser.error('--name must be a lowercase run label, not a path')
    plan, cmo, moc, install, jdk = (p.resolve(strict=True) for p in (
        args.plan, args.cmo, args.reference_moc, args.install, args.jdk))
    run_root = args.run_root.resolve()
    run_root.mkdir(parents=True, exist_ok=True)
    run = run_root/f'{args.name}-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{uuid.uuid4().hex[:8]}'
    run.mkdir()
    for name in ('temp', 'classes'):
        (run/name).mkdir()
    source = API/'PSD2LiveParentModelEdit.java'
    library = str(install/'app/*')
    commands = [
        [str(jdk/'javac.exe'), '-encoding', 'UTF-8', '-cp', library,
         '-d', str(run/'classes'), str(source)],
        [str(jdk/'java.exe'), '-Dpsd2live.agent.store='+str(run/'store'),
         '-Djava.io.tmpdir='+str(run/'temp'), '-cp', str(run/'classes')+';'+library,
         'PSD2LiveParentModelEdit', '--install', str(install), '--cmo', str(cmo),
         '--reference-moc', str(moc), '--edit-json', str(plan),
         '--output', str(run/'export'), '--store', str(run/'store')],
    ]
    provenance = {'argv': commands, 'source_sha256': sha(source),
                  'driver_sha256': sha(Path(__file__)), 'plan_sha256': sha(plan),
                  'cmo_sha256': sha(cmo), 'reference_moc_sha256': sha(moc)}
    (run/'commands.json').write_text(json.dumps(provenance, indent=2)+'\n', encoding='utf8')
    print('RUN '+str(run), flush=True)
    for i, command in enumerate(commands):
        result = subprocess.run(command, text=True, encoding='utf8', errors='replace',
                                capture_output=True, timeout=600)
        (run/f'step-{i}.log').write_text(result.stdout+result.stderr, encoding='utf8')
        print(result.stdout+result.stderr, flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
    print('PUBLIC_MULTIPAGE_EDIT_PASS '+str(run/'export'), flush=True)


if __name__ == '__main__':
    main()
