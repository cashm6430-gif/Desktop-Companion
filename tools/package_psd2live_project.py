"""Package an existing author workspace without rebuilding its rig or moving HEAD.

The archive layout matches the locally audited PSD2Live 2.0.2 ProjectArchive.
GUI opening/export is a separate acceptance step, not implied by ZIP validity.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def json_file(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def identifier_file(identifier):
    return hashlib.sha256(identifier.encode('utf8')).hexdigest() + '.json'


def package(workspace, psd, output):
    workspace, psd, output = (Path(p).resolve() for p in (workspace, psd, output))
    uuid.UUID(workspace.name)
    if output.suffix.lower() != '.psd2live':
        raise ValueError('Archive output must use a .psd2live filename.')
    receipt_path = output.with_suffix('.package.json')
    if output.is_relative_to(workspace) or output == psd:
        raise ValueError('Archive output must not replace or enter an author source workspace.')
    if output.exists() or receipt_path.exists():
        raise ValueError('Archive output or receipt already exists; select a new isolated filename.')
    if psd.read_bytes()[:4] != b'8BPS':
        raise ValueError('Source must be the actual PSD, not an exported model or ZIP.')
    head = json_file(workspace / 'HEAD.json')
    node_files = sorted((workspace / 'history/nodes').glob('*.json'))
    nodes = {node['id']: node for node in map(json_file, node_files)}
    if (len(nodes) != len(node_files) or len(head['nodeOrder']) != len(nodes)
            or set(head['nodeOrder']) != set(nodes) or head['headNodeId'] not in nodes):
        raise ValueError('HEAD and author nodes are incomplete or ambiguous.')
    roots = 0
    for path in node_files:
        node = json_file(path)
        if path.name != identifier_file(node['id']):
            raise ValueError('Author node filename does not match its identifier.')
        parent = node.get('parentId')
        if parent is None:
            roots += 1
        elif parent not in nodes:
            raise ValueError('Author history has a missing parent; refusing a partial archive.')
        snapshot = workspace / 'history/snapshots' / identifier_file(node['snapshotHash'])
        if not snapshot.is_file():
            raise ValueError('Author history has a missing snapshot.')
        document = json_file(snapshot)
        for layer in document['layers']:
            blob = layer.get('rgbaBlob')
            if not blob:
                continue
            stem = (hashlib.sha256(blob.encode('utf8')).hexdigest()
                    + f'-{layer["rasterWidth"]}x{layer["rasterHeight"]}')
            if not any((workspace / 'blobs' / (stem + suffix)).is_file()
                       for suffix in ('.png', '.rgba.gz')):
                raise ValueError('Author layer has no recoverable source pixels.')
    if roots != 1:
        raise ValueError('Author history must have exactly one root.')
    for identifier in nodes:
        seen, cursor = set(), identifier
        while cursor is not None:
            if cursor in seen:
                raise ValueError('Author history contains a parent cycle.')
            seen.add(cursor)
            cursor = nodes[cursor].get('parentId')
    sources = {'source/original.psd': psd}
    for path in sorted(p for p in workspace.rglob('*') if p.is_file()):
        if not path.resolve().is_relative_to(workspace):
            raise ValueError('Author workspace contains a link outside its source root.')
        sources[f'workspace/{workspace.name}/{path.relative_to(workspace).as_posix()}'] = path
    hashes = {name: digest(path) for name, path in sources.items()}
    ui_state = b'{}\n'
    archive_manifest = {'format': 'PSD2Live', 'version': 1, 'projectId': workspace.name,
        'files': {**hashes, 'workspace.json': hashlib.sha256(ui_state).hexdigest()}}
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        archive.writestr('manifest.json', json.dumps(archive_manifest, indent=2).encode('utf8'))
        archive.writestr('workspace.json', ui_state)
        for name, path in sources.items():
            archive.write(path, name)
    with zipfile.ZipFile(output) as archive:
        for name, expected in archive_manifest['files'].items():
            if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                raise ValueError('Packaged author source differs from the source snapshot.')
    if any(digest(path) != hashes[name] for name, path in sources.items()):
        raise ValueError('Author source changed during packaging; archive is not accepted.')
    receipt = {'schema_version': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
        'kind': 'isolated_psd2live_author_package', 'source_workspace': str(workspace),
        'source_psd': str(psd), 'source_head': head['headNodeId'], 'history_nodes': len(nodes),
        'archive': str(output), 'archive_sha256': digest(output), 'archive_bytes': output.stat().st_size,
        'archive_manifest': archive_manifest, 'source_hashes_verified_after_packaging': True,
        'source_HEAD_changed': False, 'GUI_open_and_export': 'pending',
        'does_not_establish_current_runtime_equivalence': True}
    with receipt_path.open('x', encoding='utf8') as stream:
        stream.write(json.dumps(receipt, ensure_ascii=False, indent=2)+'\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', required=True, type=Path)
    parser.add_argument('--psd', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    receipt = package(args.workspace, args.psd, args.output)
    print(json.dumps({key: receipt[key] for key in ('archive', 'archive_bytes', 'history_nodes', 'source_head')}, ensure_ascii=False))


if __name__ == '__main__':
    main()
