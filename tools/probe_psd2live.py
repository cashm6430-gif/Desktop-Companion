"""Stage an analytical PSD2Live fixture in its own recoverable workspace.

This is a tool capability experiment, not new character art. All editing uses
fresh state handles; exports stay outside assets and the approved authoring
state file is never written. Never run while another user is editing PSD2Live.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from uuid import uuid4

from PIL import Image, ImageDraw
from psd2live_client import call, initialize


ROOT = Path(__file__).resolve().parents[1]
PROBES = ROOT / 'build/authoring-probes/psd2live-2.0.2'


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf8')


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def fixture(folder):
    folder.mkdir(parents=True, exist_ok=False)
    result = []
    for name in ('anchor', 'mouth-closed', 'mouth-open', 'tail', 'upper-arm', 'forearm'):
        picture = Image.new('RGBA', (640, 512))
        draw = ImageDraw.Draw(picture)
        if name == 'anchor':
            for x, y in ((110, 88), (70, 380), (420, 120), (420, 230), (420, 340)):
                draw.ellipse((x-4, y-4, x+4, y+4), fill=(35, 55, 90, 255))
        elif name == 'mouth-closed':
            draw.rounded_rectangle((88, 86, 132, 90), radius=2, fill=(35, 170, 75, 255))
        elif name == 'mouth-open':
            draw.ellipse((88, 66, 132, 110), fill=(210, 55, 75, 255))
        elif name == 'tail':
            draw.rounded_rectangle((60, 369, 310, 391), radius=11, fill=(35, 180, 215, 255))
            for x in (70, 130, 190, 250, 300):
                draw.ellipse((x-2, 378, x+2, 382), fill=(20, 60, 110, 255))
        elif name == 'upper-arm':
            draw.rounded_rectangle((406, 106, 434, 244), radius=14, fill=(240, 175, 40, 255))
        else:
            draw.rounded_rectangle((406, 216, 434, 354), radius=14, fill=(155, 95, 210, 255))
        path = folder / f'{name}.png'
        picture.save(path)
        result.append({'name': name, 'path': str(path), 'role': 'objects', 'side': 'none', 'x': 0, 'y': 0})
    write_json(folder / 'fixture.json', {
        'canvas': [640, 512], 'kind': 'analytical_test_fixture',
        'not_character_art': True, 'root': [70, 380], 'tip': [300, 380],
        'shoulder': [420, 120], 'elbow': [420, 230], 'wrist': [420, 340],
        'layers': [{**layer, 'sha256': digest(layer['path'])} for layer in result]})
    return result


class ProbeSession:
    def __init__(self, run):
        self.run = Path(run).resolve()
        self.path = self.run / 'probe.json'
        self.manifest = json.loads(self.path.read_text(encoding='utf8')) if self.path.exists() else {
            'schema_version': 1, 'state': None, 'operations': [], 'exports': []}
        reply, self.session = initialize()
        server = reply.get('result', {}).get('serverInfo', {})
        if server.get('version') != '2.0.2':
            raise RuntimeError('This experiment requires the locally inspected PSD2Live 2.0.2 API.')
        self.manifest['server'] = server
        self.ident = 2

    @property
    def state(self):
        return self.manifest['state']

    def invoke(self, name, arguments, label=None):
        response, self.session = call('tools/call', {'name': name, 'arguments': arguments},
                                      self.ident, self.session)
        self.ident += 1
        result = response.get('result', {})
        payload = result.get('structuredContent', {})
        number = len(self.manifest['operations'])
        label = f'{number:03}-{label or name}'
        images = []
        for block in result.get('content', []):
            if block.get('type') == 'image':
                directory = self.run / 'views'
                directory.mkdir(exist_ok=True)
                path = directory / f'{label}-{len(images)}.png'
                path.write_bytes(base64.b64decode(block['data']))
                images.append({'path': path.relative_to(self.run).as_posix(), 'sha256': digest(path)})
        record = {'tool': name, 'arguments': arguments, 'response': payload,
                  'rpc_error': response.get('error'), 'is_error': result.get('isError', False),
                  'text': [block.get('text', '')[:8000] for block in result.get('content', [])
                           if block.get('type') == 'text'], 'images': images}
        directory = self.run / 'operations'
        directory.mkdir(exist_ok=True)
        write_json(directory / f'{label}.json', record)
        self.manifest['operations'].append({'file': f'operations/{label}.json', 'tool': name})
        read_only = name in ('inspect', 'view', 'export') or (
            name == 'skeleton' and arguments.get('request', {}).get('mode') in ('get', 'propose', 'pose'))
        if payload.get('state'):
            if (self.state and read_only
                    and payload['state'] != self.state):
                raise RuntimeError('The active editor state changed outside this experiment; stop before further edits.')
            self.manifest['state'] = payload['state']
        write_json(self.path, self.manifest)
        if response.get('error') or result.get('isError') or payload.get('error'):
            raise RuntimeError(f'{label}: ' + json.dumps(payload or response.get('error') or record['text'],
                                                       ensure_ascii=False)[:1500])
        if payload.get('applied') is False:
            raise RuntimeError(f'{label}: editor explicitly reported applied:false (no authoring change).')
        print(label, payload.get('summary', 'ok'), flush=True)
        return payload

    def request(self, name, fields, label=None):
        state = {'state': self.state} if self.state else {}
        return self.invoke(name, {'request': {**state, **fields}}, label)

    def export(self, label):
        directory = self.run / 'exports' / f'{len(self.manifest["exports"]):02}-{label}'
        directory.mkdir(parents=True, exist_ok=False)
        result = self.invoke('export', {'state': self.state, 'output_directory': str(directory)}, label)
        self.manifest['exports'].append({'label': label, 'directory': directory.relative_to(self.run).as_posix(),
                                         'state': self.state, 'response': result})
        write_json(self.path, self.manifest)
        return directory

    def objects(self):
        payload = self.invoke('inspect', {'scope': 'objects', 'limit': 64})
        return {item['name']: item['target'] for item in payload.get('objects', payload.get('items', []))}

    def layers(self):
        payload = self.invoke('inspect', {'scope': 'layers', 'limit': 32})
        return {item['name']: item['id'] for item in payload.get('layers', payload.get('items', []))}

    def restore_source(self):
        self.invoke('inspect', {'scope': 'project'})
        source = self.manifest['exports'][0]['state']
        if self.state != source:
            self.invoke('revision', {'request': {'mode': 'restore', 'node_id': source}}, 'restore-source')

    def view(self, label, poses):
        self.invoke('view', {'request': {'mode': 'poses',
            'viewport': {'mode': 'canvas_rect', 'left': 0, 'top': 0, 'width': 640, 'height': 512},
            'poses': poses, 'target_long_edge': 1024}}, label)

    def pose_file(self, name, poses, targets):
        directory = self.run / 'poses'
        directory.mkdir(exist_ok=True)
        write_json(directory / f'{name}.json', {'poses': poses,
            'drawables': [target.split(':', 1)[1] for target in targets.values()
                          if target.startswith('mesh:')], 'target_names': targets})


def create_probe(probe):
    project = probe.invoke('inspect', {'scope': 'project'})
    if project.get('loaded'):
        raise RuntimeError('PSD2Live has a loaded project. Close it or use a separate instance before creating a probe.')
    layers = fixture(probe.run / 'input')
    probe.request('asset', {'mode': 'create', 'width': 640, 'height': 512, 'layers': layers})
    probe.invoke('inspect', {'scope': 'settings'})
    probe.invoke('settings', {'state': probe.state, 'changes': {
        'atlasSize': 1024, 'meshSpacing': 16, 'texturePadding': 8, 'meshOuterMargin': 2,
        'generatePhysics': False, 'physicsEyeJelly': False, 'mouthOutlineEnabled': False,
        'exportMotions': False, 'exportCmo3': True, 'exportMoc3': True}})
    probe.invoke('inspect', {'scope': 'layers', 'limit': 32})
    probe.objects()
    probe.invoke('inspect', {'scope': 'parameters', 'limit': 64})
    probe.export('source')


def auto_switch(probe, create_parameter=True):
    probe.restore_source()
    label = 'auto-switch' if create_parameter else 'switch-generated-range'
    if create_parameter:
        probe.request('parameter', {'mode': 'create', 'parameter_id': 'ParamProbeMouth',
            'name': 'Probe mouth switch', 'min': 0, 'max': 1, 'default': 0})
    layers = probe.layers()
    for name, switch in (('mouth-closed', 0), ('mouth-open', 1)):
        probe.invoke('layer', {'state': probe.state, 'layer_id': layers[name],
            'role': 'face_detail', 'side': 'none', 'type': 'switch',
            'parameter': 'ParamProbeMouth', 'switch_id': switch})
    targets = probe.objects()
    for name in ('mouth-closed', 'mouth-open'):
        probe.invoke('inspect', {'target': targets[name]})
    probe.invoke('inspect', {'scope': 'parameters', 'query': 'ParamProbeMouth'})
    poses = [{'name': f'switch-{value}', 'parameters': {'ParamProbeMouth': value}}
             for value in (0, 0.25, 0.5, 0.75, 1)]
    probe.pose_file(label, poses, targets)
    probe.view(f'{label}-view', [pose['parameters'] for pose in poses])
    probe.export(label)


def manual_opacity(probe):
    probe.restore_source()
    probe.request('parameter', {'mode': 'create', 'parameter_id': 'ParamProbeOpacity',
        'name': 'Probe manual opacity', 'min': 0, 'max': 1, 'default': 0})
    targets = probe.objects()
    changes = [{'op': 'set', 'target': targets[name], 'key': {'ParamProbeOpacity': value},
                'channels': {'opacity': opacity}}
               for name, values in (('mouth-closed', (1, 0)), ('mouth-open', (0, 1)))
               for value, opacity in zip((0, 1), values)]
    probe.invoke('form', {'state': probe.state, 'changes': changes}, 'manual-opacity-keys')
    for name in ('mouth-closed', 'mouth-open'):
        probe.invoke('inspect', {'target': targets[name]})
    poses = [{'name': f'opacity-{value}', 'parameters': {'ParamProbeOpacity': value}}
             for value in (0, 0.5, 1)]
    probe.pose_file('manual-opacity', poses, targets)
    probe.view('manual-opacity-view', [pose['parameters'] for pose in poses])
    probe.export('manual-opacity')


def single_visibility(probe, binding='switch'):
    probe.restore_source()
    label = 'single-switch-one' if binding == 'switch' else 'single-toggle'
    layers = probe.layers()
    fields = {'state': probe.state, 'layer_id': layers['mouth-open'],
        'role': 'face_detail', 'side': 'none', 'type': binding, 'parameter': 'ParamProbeSingle'}
    if binding == 'switch':
        fields['switch_id'] = 1
    probe.invoke('layer', fields)
    probe.invoke('inspect', {'scope': 'parameters', 'query': 'ParamProbeSingle'})
    targets = probe.objects()
    probe.invoke('inspect', {'target': targets['mouth-open']})
    poses = [{'name': f'{binding}-{value}', 'parameters': {'ParamProbeSingle': value}}
             for value in (0, 0.25, 0.5, 0.75, 1, 1.5, 2)]
    probe.pose_file(label, poses, targets)
    probe.view(f'{label}-view', [pose['parameters'] for pose in poses[:5]])
    probe.export(label)


def range_lifecycle(probe):
    probe.restore_source()
    identifier = 'ParamProbeRange'
    probe.request('parameter', {'mode': 'create', 'parameter_id': identifier,
        'name': 'Probe range', 'min': 0, 'max': 1, 'default': 0.25})
    targets = probe.objects()
    tail = targets['tail']
    probe.invoke('form', {'state': probe.state, 'changes': [
        {'op': 'seed', 'target': tail, 'key': {identifier: 0}},
        {'op': 'seed', 'target': tail, 'key': {identifier: 1}}]}, 'range-seed')
    probe.invoke('deform', {'state': probe.state, 'changes': [
        {'target': tail, 'key': {identifier: 1},
         'operations': [{'type': 'translate', 'delta': [0.2, 0]}]}]}, 'range-geometry')
    for label, values in (('range-created', (0, 0.25, 1)),
                          ('range-updated', (-1, 0, 0.25, 0.5, 0.75, 1, 1.5, 2))):
        if label == 'range-updated':
            probe.request('parameter', {'mode': 'update', 'parameter_id': identifier,
                'min': -1, 'max': 2, 'default': 0.5})
        probe.invoke('inspect', {'scope': 'parameters', 'query': identifier})
        probe.pose_file(label, [{'name': str(value), 'parameters': {identifier: value}}
                               for value in values], targets)
        probe.export(label)
    probe.request('parameter', {'mode': 'delete', 'parameter_id': identifier})
    probe.invoke('inspect', {'scope': 'parameters', 'query': identifier})
    probe.invoke('inspect', {'target': tail})
    probe.pose_file('range-deleted', [{'name': 'deleted-default', 'parameters': {}}], targets)
    probe.export('range-deleted')


def skeleton(probe):
    probe.restore_source()
    targets = probe.objects()
    bones = [
        {'id': 'probe_upper', 'role': 'UPPER_ARM', 'side': 'RIGHT',
         'head': [420, 120], 'tail': [420, 230], 'direction': 1,
         'drawables': [targets['upper-arm'].split(':', 1)[1]],
         'minAngle': -30, 'maxAngle': 30, 'parameterOverride': 'ParamProbeShoulder'},
        {'id': 'probe_lower', 'role': 'FOREARM', 'side': 'RIGHT',
         'parent': 'probe_upper', 'connected': True,
         'head': [420, 230], 'tail': [420, 340], 'direction': 1,
         'drawables': [targets['forearm'].split(':', 1)[1]],
         'minAngle': -45, 'maxAngle': 45, 'parameterOverride': 'ParamProbeElbow'}]
    probe.request('skeleton', {'mode': 'put', 'spec': {'enabled': True, 'bones': bones}})
    probe.invoke('skeleton', {'request': {'mode': 'get'}}, 'skeleton-readback')
    probe.invoke('inspect', {'scope': 'parameters', 'query': 'ParamProbe', 'limit': 64})
    targets = probe.objects()
    for name in ('upper-arm', 'forearm'):
        probe.invoke('inspect', {'target': targets[name]})
    poses = [{'name': f'shoulder-{shoulder}-elbow-{elbow}',
              'parameters': {'ParamProbeShoulder': shoulder, 'ParamProbeElbow': elbow}}
             for shoulder in (-30, -15, 0, 15, 30) for elbow in (-45, -22.5, 0, 22.5, 45)]
    probe.pose_file('skeleton', poses, targets)
    probe.view('skeleton-view', [pose['parameters'] for pose in poses[::6]])
    probe.export('skeleton')


def unbound_delete(probe):
    probe.restore_source()
    identifier = 'ParamProbeUnbound'
    probe.request('parameter', {'mode': 'create', 'parameter_id': identifier,
        'name': 'Probe unbound deletion', 'min': 0, 'max': 1, 'default': 0.25})
    probe.invoke('inspect', {'scope': 'parameters', 'query': identifier})
    probe.request('parameter', {'mode': 'delete', 'parameter_id': identifier})
    probe.invoke('inspect', {'scope': 'parameters', 'query': identifier})
    probe.pose_file('unbound-deleted', [{'name': 'default', 'parameters': {}}], probe.objects())
    probe.export('unbound-deleted')


def tail_swing(probe, kind='lateral'):
    probe.restore_source()
    targets = probe.objects()
    label = 'tail-swing' if kind == 'lateral' else 'tail-bend'
    probe.request('swing', {'mode': 'put', 'id': 'probe-tail', 'name': 'Probe flexible tail',
        'targets': [targets['tail'].split(':', 1)[1]], 'kind': kind, 'segments': 1,
        'parameters': ['ParamProbeTail'], 'magnitude': 0.35, 'softness': 0.7,
        'parallel': 0.7 if kind == 'lateral' else 0,
        'lift': 0.04 if kind == 'lateral' else 0,
        'fulcrum': 'left', 'physics_enabled': False})
    probe.invoke('inspect', {'scope': 'swings', 'limit': 32})
    probe.invoke('inspect', {'scope': 'parameters', 'query': 'ParamProbeTail'})
    targets = probe.objects()
    probe.invoke('inspect', {'target': targets['tail']})
    poses = [{'name': f'tail-{value}', 'parameters': {'ParamProbeTail': value}}
             for value in (-1, -0.5, 0, 0.5, 1)]
    probe.pose_file(label, poses, targets)
    probe.view(f'{label}-view', [pose['parameters'] for pose in poses])
    probe.export(label)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, help='existing isolated experiment directory')
    parser.add_argument('--stage', choices=('create', 'auto-switch', 'switch-generated-range',
        'single-switch-one', 'single-toggle', 'manual-opacity', 'range', 'unbound-delete',
        'skeleton', 'tail-swing', 'tail-bend'), default='create')
    args = parser.parse_args(argv)
    run = args.run
    if run is None:
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        run = PROBES / f'probe-{stamp}-{uuid4().hex[:8]}'
        run.mkdir(parents=True, exist_ok=False)
    elif not run.exists():
        parser.error('--run must point to an existing isolated experiment')
    probe = ProbeSession(run)
    if args.stage == 'create':
        if probe.state:
            parser.error('create cannot overwrite an existing experiment state')
        create_probe(probe)
    elif args.stage == 'auto-switch':
        auto_switch(probe)
    elif args.stage == 'switch-generated-range':
        auto_switch(probe, create_parameter=False)
    elif args.stage == 'manual-opacity':
        manual_opacity(probe)
    elif args.stage == 'single-switch-one':
        single_visibility(probe)
    elif args.stage == 'single-toggle':
        single_visibility(probe, binding='toggle')
    elif args.stage == 'range':
        range_lifecycle(probe)
    elif args.stage == 'unbound-delete':
        unbound_delete(probe)
    elif args.stage == 'skeleton':
        skeleton(probe)
    elif args.stage == 'tail-swing':
        tail_swing(probe)
    elif args.stage == 'tail-bend':
        tail_swing(probe, kind='vertical')
    print('Probe:', run)


if __name__ == '__main__':
    main()
