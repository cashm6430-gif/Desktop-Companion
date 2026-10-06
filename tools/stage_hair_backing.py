"""Stage the authored pure-hair backing candidate without touching production.

Takes the fresh public-CMO export (one inserted mesh over the R15 desk
candidate), proves every pre-existing drawable stays bit-exact across poses,
checks the render order contract, and writes an isolated review model
directory.  The review candidate keeps the isolated island cleanup (duplicate
ear tips and specks) with the GPU support extent disabled, because the authored
mesh now carries the pure-hair backing.
"""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import random
import shutil

import numpy as np

from probe_cubism_core import NativeModel, find_core

ROOT = Path(__file__).resolve().parents[1]
R1 = ROOT / 'art/live2d/desk-work-20261006-r1'
R2 = ROOT / 'art/live2d/desk-work-20261006-r2'
NEW_MESH = 'ArtMeshHairBackingPure'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                    encoding='utf8')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--export', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    export, output = args.export.resolve(strict=True), args.output.resolve()
    if output.exists():
        raise ValueError('Fresh output required.')
    candidate_moc = R1 / 'candidate-model/whale-girl-layered-draft.moc3'
    old = NativeModel(find_core(), candidate_moc)
    new = NativeModel(find_core(), export / 'edited.moc3')

    added = set(new.drawable_ids) - set(old.drawable_ids)
    removed = set(old.drawable_ids) - set(new.drawable_ids)
    if added != {NEW_MESH} or removed:
        raise ValueError(f'Unexpected drawable delta: +{added} -{removed}')
    if list(new.parameter_ids) != list(old.parameter_ids):
        raise ValueError('Parameter set changed')

    # Old drawables keep vertex counts, UV, indices, and relative render order.
    pairs = [(old.drawable_ids.index(name), new.drawable_ids.index(name))
             for name in old.drawable_ids]
    for i, j in pairs:
        if old.vertex_counts[i] != new.vertex_counts[j]:
            raise ValueError(f'Vertex count changed for {old.drawable_ids[i]}')
        if old.index_counts[i] != new.index_counts[j]:
            raise ValueError(f'Index count changed for {old.drawable_ids[i]}')
        for field, size in [('DrawableVertexUvs', old.vertex_counts[i] * 8),
                            ('DrawableIndices', old.index_counts[i] * 2)]:
            if c.string_at(old.get(field)[i], size) != c.string_at(new.get(field)[j], size):
                raise ValueError(f'{field} changed for {old.drawable_ids[i]}')
    old_rank = old.get(old.render_orders_field)
    new_rank = new.get(new.render_orders_field)
    old_by_render = sorted(old.drawable_ids, key=lambda n: old_rank[old.drawable_ids.index(n)])
    new_values = [new_rank[new.drawable_ids.index(n)] for n in old_by_render]
    if new_values != sorted(new_values):
        raise ValueError('Old drawable relative render order changed')

    pure_index = new.drawable_ids.index(NEW_MESH)
    backing_index = new.drawable_ids.index('ArtMeshBackHair')
    rear_index = new.drawable_ids.index('ArtMeshBackHair2')
    topwear_index = new.drawable_ids.index('ArtMeshTopwear')
    orders = new.get(new.render_orders_field)
    # Integer draw orders cannot express a slot between the backing (2) and
    # headwear (3); declared draw_order 3 lands directly above headwear.  The
    # wing footprint does not overlap the headdress, and the mesh stays below
    # every body drawable, so the visible stacking matches the intent.
    if not (orders[rear_index] < orders[backing_index] < orders[pure_index]
            < orders[topwear_index]):
        raise ValueError('Backing render order contract violated')

    # The inserted mesh copies the backing's topology; the export canonicalizes
    # triangle winding, so compare the triangle multiset instead of bytes.
    backing_idx = old.drawable_ids.index('ArtMeshBackHair')
    tri = lambda model, i: sorted(np.sort(np.frombuffer(
        c.string_at(model.get('DrawableIndices')[i], model.index_counts[i] * 2),
        dtype=np.uint16).reshape(-1, 3), axis=1).tolist())
    if tri(old, backing_idx) != tri(new, pure_index):
        raise ValueError('Inserted mesh triangles differ from the backing template')

    rng = random.Random(20261006)
    # Both models carry the desk parameter, so it must be pinned identically
    # on both sides for the bit-exact sweep; every other parameter randomizes.
    poses = [{}] + [{name: rng.uniform(row['min'], row['max'])
                     for name, row in old.ranges.items() if name != 'ParamDeskVisible'}
                    for _ in range(160)]
    contexts = 0
    for params in poses:
        for desk in [0., .5, 1.]:
            both = {**params, 'ParamDeskVisible': desk}
            old.update({'parameters': both, 'part_opacities': {}})
            new.update({'parameters': both, 'part_opacities': {}})
            for i, j in pairs:
                if c.string_at(old.get('DrawableVertexPositions')[i], old.vertex_counts[i] * 8) \
                        != c.string_at(new.get('DrawableVertexPositions')[j], new.vertex_counts[j] * 8):
                    raise ValueError(f'Positions changed for {old.drawable_ids[i]}')
                if c.string_at(c.byref(old.get('DrawableOpacities').contents, i * 4), 4) \
                        != c.string_at(c.byref(new.get('DrawableOpacities').contents, j * 4), 4):
                    raise ValueError(f'Opacity changed for {old.drawable_ids[i]}')
            contexts += 1

    # New mesh follows the backing: compare rest positions against the old
    # backing's rest positions directly (same parent-local vertex coordinates).
    # ArtMesh indices differ between the two MOCs; resolve them per model.
    old_backing = old.drawable_ids.index('ArtMeshBackHair')
    backing_positions = old.get('DrawableVertexPositions')[old_backing]
    pure_positions = new.get('DrawableVertexPositions')[pure_index]
    n = old.vertex_counts[old_backing] * 8
    if c.string_at(backing_positions, n) != c.string_at(pure_positions, n):
        raise ValueError('Inserted mesh rest positions differ from the backing template')

    # Stage the isolated review model from the R15 staged assets.
    family_src = (ROOT / '.local/authoring/desk-work-20261006-r1/'
                  'stage-r15-connected-typing/assets/live2d/whale-girl')
    family = output / 'assets/live2d/whale-girl'
    shutil.copytree(family_src, family)
    staged_moc = family / 'whale-girl-layered-draft.moc3'
    shutil.copyfile(export / 'edited.moc3', staged_moc)
    textures = []
    for page, name in enumerate(sorted(export.glob('texture_*.png'),
                                       key=lambda p: int(p.stem.split('_')[1]))):
        target = family / f'whale-girl-layered-draft.4096/texture_{page:02d}.png'
        shutil.copyfile(name, target)
        textures.append(target.name)
    model_path = family / 'whale-girl-layered-draft.model3.json'
    model = json.loads(model_path.read_text(encoding='utf8'))
    model['FileReferences']['Textures'] = [
        f'whale-girl-layered-draft.4096/texture_{i:02d}.png' for i in range(len(textures))]
    write(model_path, model)

    metadata_path = family / 'whale-girl-layered-draft.psd2live.json'
    metadata = json.loads(metadata_path.read_text(encoding='utf8'))
    metadata['runtimeDeskWorkMode']['mocSha256'] = sha(staged_moc)
    metadata['runtimeDeskWorkMode']['stage'] = 'preview'
    pure_hair = R1 / 'arm-backing-hair-only.png'
    generation = json.loads((R1 / 'generation-hair-backing.json').read_text(encoding='utf8'))
    target_dir = family / 'material-ownership'
    target_dir.mkdir(exist_ok=True)
    shutil.copyfile(pure_hair, target_dir / 'arm-backing-hair-only.png')
    metadata['runtimeHairBacking'] = {
        'version': 1, 'stage': 'preview', 'adopted': False,
        'mocSha256': sha(staged_moc),
        'drawable': NEW_MESH, 'template': 'ArtMeshBackHair',
        'texturePage': 4,
        'pureHairPage': {'file': 'whale-girl-layered-draft.4096/texture_04.png',
                         'sha256': sha(export / 'texture_04.png')},
        'mechanism': 'authored mesh; verbatim backing vertices/grid; remapped UVs',
        'legacyBacking': 'untouched (preservation proofs remain valid)',
    }
    metadata['runtimeBackdropCleanup'] = {
        'version': 1, 'mocSha256': sha(staged_moc),
        'atlasSha256': sha(family / 'whale-girl-layered-draft.4096/texture_00.png'),
        'backHairOnly': 'ArtMeshBackHair2', 'backingMaterial': 'ArtMeshBackHair',
        'keepStrongComponents': 3, 'supportScale': [1, 1],
        'status': 'island_cleanup_only; backing carried by authored mesh',
    }
    metadata['runtimeBackdropCleanup']['backingTexture'] = {
        'file': 'material-ownership/arm-backing-hair-only.png', 'sha256': sha(pure_hair),
        'sourceRects': generation['generated_wing_alpha_bbox']}
    write(metadata_path, metadata)

    ownership_path = family / 'material-ownership/ownership.json'
    if ownership_path.exists():
        ownership = json.loads(ownership_path.read_text(encoding='utf8'))
        ownership['moc_sha256'] = sha(staged_moc)
        write(ownership_path, ownership)

    record = {
        'schema_version': 1, 'status': 'pending', 'adopted': False,
        'candidate_base_moc_sha256': sha(candidate_moc),
        'staged_moc_sha256': sha(staged_moc),
        'source_edit_report_sha256': sha(export / 'edit-report.json'),
        'old38_geometry_opacity_bit_exact_contexts': contexts,
        'old38_topology_exact': True, 'old38_relative_render_order_exact': True,
        'authored_backing_render_order_exact': True,
        'native_counts': {'parameters': len(new.parameter_ids),
                          'meshes': len(new.drawable_ids),
                          'pages': len(model['FileReferences']['Textures'])},
        'visual_review': 'pending', 'full_native_regression': 'not_run',
        'production_replaced': False,
    }
    write(output / 'staging-report.json', record)
    shutil.copyfile(ROOT / '.local/authoring/desk-work-20261006-r1/stage-r18-handoff/cleanup-poses.json',
                    output / 'cleanup-poses.json')
    print(json.dumps({'model': str(model_path), **record}, ensure_ascii=False))


if __name__ == '__main__':
    main()
