"""Use real minimal PSD/history/blob bytes to test isolated author packages."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import package_psd2live_project as packaging


def sha(data):
    return hashlib.sha256(data).hexdigest()


def png_chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))


class PSD2LivePackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='psd2live-package-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def fixture(self, name):
        parent = self.root / name
        workspace = parent / '740d9a6a-1e96-4834-933a-7e962c9e7f1e'
        for directory in ('history/nodes', 'history/snapshots', 'blobs'):
            (workspace / directory).mkdir(parents=True, exist_ok=True)
        # Valid 1x1 RGB PSD: header, three empty variable sections, raw planar
        # RGB data. No application, image generation, or official model used.
        psd = parent / 'original.psd'
        psd.write_bytes(struct.pack('>4sH6sHIIHH', b'8BPS', 1, bytes(6), 3, 1, 1, 8, 3)
                        + bytes(12) + struct.pack('>H', 0) + bytes((17, 34, 51)))
        rgba = bytes((17, 34, 51, 255))
        blob_id = sha(rgba)
        png = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
               + png_chunk(b'IDAT', zlib.compress(b'\x00' + rgba)) + png_chunk(b'IEND', b''))
        blob = workspace / 'blobs' / (sha(blob_id.encode('utf8')) + '-1x1.png')
        blob.write_bytes(png)
        node_ids = ['cbac2711-23b8-4657-9e3d-6715fb79e4b1', 'b9656edf-1e48-40c8-8d21-b4862c892a41']
        node_paths, snapshot_paths = [], []
        for index, node_id in enumerate(node_ids):
            snapshot = {'layers': [{'id': 'fixture-layer', 'name': 'Root' if index == 0 else 'Edited label',
                                    'rgbaBlob': blob_id, 'rasterWidth': 1, 'rasterHeight': 1}]}
            snapshot_bytes = (json.dumps(snapshot, separators=(',', ':')) + '\n').encode('utf8')
            snapshot_id = sha(snapshot_bytes)
            snapshot_path = workspace / 'history/snapshots' / packaging.identifier_file(snapshot_id)
            snapshot_path.write_bytes(snapshot_bytes)
            snapshot_paths.append(snapshot_path)
            node = {'id': node_id, 'parentId': None if index == 0 else node_ids[index - 1],
                    'snapshotHash': snapshot_id}
            node_path = workspace / 'history/nodes' / packaging.identifier_file(node_id)
            node_path.write_text(json.dumps(node, indent=2) + '\n', encoding='utf8')
            node_paths.append(node_path)
        # Preserve a BOM/CRLF source as raw bytes rather than reserializing it.
        head = {'headNodeId': node_ids[-1], 'nodeOrder': node_ids}
        (workspace / 'HEAD.json').write_bytes(b'\xef\xbb\xbf' + json.dumps(head, indent=2).replace('\n', '\r\n').encode('utf8') + b'\r\n')
        (workspace / 'extra.bin').write_bytes(bytes(range(256)))
        return {'workspace': workspace, 'psd': psd, 'nodes': node_paths, 'snapshots': snapshot_paths,
                'blob': blob, 'head': head, 'output': parent / 'isolated/fixture.psd2live'}

    def source_bytes(self, fixture):
        return {path: path.read_bytes() for path in [fixture['psd'], *sorted(path for path in fixture['workspace'].rglob('*') if path.is_file())]}

    def assert_rejected_without_output_or_source_changes(self, fixture, output, message):
        before = self.source_bytes(fixture)
        with self.assertRaisesRegex(ValueError, message):
            packaging.package(fixture['workspace'], fixture['psd'], output)
        self.assertEqual(self.source_bytes(fixture), before)
        if output not in before:
            self.assertFalse(output.exists())
        self.assertFalse(output.with_suffix('.package.json').exists())

    def test_complete_package_preserves_every_source_byte_and_head(self):
        fixture = self.fixture('complete')
        before = self.source_bytes(fixture)
        receipt = packaging.package(fixture['workspace'], fixture['psd'], fixture['output'])
        self.assertEqual(self.source_bytes(fixture), before)
        self.assertEqual(receipt['source_head'], fixture['head']['headNodeId'])
        self.assertEqual(receipt['history_nodes'], 2)
        self.assertTrue(receipt['source_hashes_verified_after_packaging'])
        self.assertFalse(receipt['source_HEAD_changed'])
        self.assertEqual(receipt['GUI_open_and_export'], 'pending')
        expected = {'source/original.psd': before[fixture['psd']], 'workspace.json': b'{}\n'}
        expected.update({f'workspace/{fixture["workspace"].name}/{path.relative_to(fixture["workspace"]).as_posix()}': raw
                         for path, raw in before.items() if path != fixture['psd']})
        with zipfile.ZipFile(fixture['output']) as archive:
            manifest = json.loads(archive.read('manifest.json'))
            self.assertEqual(manifest['format'], 'PSD2Live')
            self.assertEqual(manifest['projectId'], fixture['workspace'].name)
            self.assertEqual(len(archive.namelist()), len(set(archive.namelist())))
            self.assertEqual(set(archive.namelist()), set(expected) | {'manifest.json'})
            self.assertEqual(manifest['files'], {name: sha(raw) for name, raw in expected.items()})
            for name, raw in expected.items():
                self.assertEqual(archive.read(name), raw, name)
        self.assertEqual(receipt['archive_sha256'], sha(fixture['output'].read_bytes()))
        self.assertEqual(json.loads(fixture['output'].with_suffix('.package.json').read_text(encoding='utf8')), receipt)

    def test_missing_parent_refuses_partial_history(self):
        fixture = self.fixture('parent')
        child = json.loads(fixture['nodes'][1].read_text(encoding='utf8'))
        child['parentId'] = 'unavailable-history-node'
        fixture['nodes'][1].write_text(json.dumps(child), encoding='utf8')
        self.assert_rejected_without_output_or_source_changes(fixture, fixture['output'], 'missing parent')

    def test_missing_snapshot_or_pixel_blob_refuses_partial_package(self):
        for missing in ('snapshot', 'blob'):
            with self.subTest(missing=missing):
                fixture = self.fixture(missing)
                target = fixture['snapshots'][1] if missing == 'snapshot' else fixture['blob']
                target.unlink()
                message = 'missing snapshot' if missing == 'snapshot' else 'recoverable source pixels'
                self.assert_rejected_without_output_or_source_changes(fixture, fixture['output'], message)

    def test_output_cannot_enter_workspace_or_replace_psd_source(self):
        fixture = self.fixture('source-protection')
        inside = fixture['workspace'] / 'new.psd2live'
        self.assert_rejected_without_output_or_source_changes(fixture, inside, 'source workspace')
        self.assert_rejected_without_output_or_source_changes(fixture, fixture['psd'], 'psd2live filename')

    def test_existing_archive_and_receipt_are_preserved(self):
        for protected in ('archive', 'receipt'):
            with self.subTest(protected=protected):
                fixture = self.fixture('existing-' + protected)
                output = fixture['output']
                target = output if protected == 'archive' else output.with_suffix('.package.json')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b'previous valuable artifact\x00\xff')
                before = self.source_bytes(fixture)
                with self.assertRaisesRegex(ValueError, 'already exists'):
                    packaging.package(fixture['workspace'], fixture['psd'], output)
                self.assertEqual(target.read_bytes(), b'previous valuable artifact\x00\xff')
                self.assertEqual(self.source_bytes(fixture), before)
                other = output.with_suffix('.package.json') if protected == 'archive' else output
                self.assertFalse(other.exists())

    def test_wrong_extension_rejects_archive_receipt_collision(self):
        for extension in ('.zip', '.package.json', ''):
            with self.subTest(extension=extension):
                fixture = self.fixture('extension-' + (extension.replace('.', '-') or 'none'))
                output = fixture['output'].with_name('invalid' + extension)
                self.assert_rejected_without_output_or_source_changes(fixture, output, 'psd2live filename')


if __name__ == '__main__':
    unittest.main()
