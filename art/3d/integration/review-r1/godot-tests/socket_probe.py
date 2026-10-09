import json, socket, subprocess, time, pathlib, threading, traceback

ROOT = pathlib.Path('E:/projects/Desktop-Companion/.local/authoring/host-behavior-20261009-r1')
GODOT = 'D:/tool/godot/Godot_v4.7.2-stable_win64_console.exe'
CAPTURE = ROOT / 'socket-runtime'
TOKEN = 'f' * 64
messages = []
assertions = []
server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen(1)
server.settimeout(8)
port = server.getsockname()[1]
log = (ROOT/'socket-runtime.log').open('w', encoding='utf8')
process = subprocess.Popen([GODOT, '--headless', '--path', str(ROOT/'project'), '--',
 '--host-port', str(port), '--host-token', TOKEN, '--review-output', str(CAPTURE), '--quit-after', '22'], stdout=log, stderr=subprocess.STDOUT)

class Channel:
 def __init__(self, number):
  self.socket, _ = server.accept()
  self.socket.settimeout(5)
  self.file = self.socket.makefile('rb')
  self.session = 'test-session'
  self.connection = f'connection-{number}'
  self.seq = 1
  hello = self.read()
  assert hello == {'v':1, 'type':'hello', 'token':TOKEN}
  self.socket.sendall((json.dumps({'v':1, 'type':'welcome', 'session_id':self.session, 'connection_id':self.connection, 'seq':1})+'\n').encode())
  ready = self.read()
  assert ready['type'] == 'ready' and ready['seq'] == 1 and 'activity.busy' in ready['capabilities']
 def read(self):
  line = self.file.readline()
  if not line: raise EOFError('closed')
  value = json.loads(line)
  messages.append({key:value for key,value in value.items() if key != 'token'})
  return value
 def send(self, type_name, **fields):
  self.seq += 1
  fields.update(v=1, type=type_name, session_id=self.session, connection_id=self.connection, seq=self.seq)
  self.socket.sendall((json.dumps(fields)+'\n').encode())
 def ack(self, name, status):
  message = self.read()
  assert message['type']=='action.ack' and message['request_id']==name and message['status']==status, message
 def close(self):
  self.file.close(); self.socket.close()

try:
 channel = Channel(1)
 channel.send('activity.snapshot', revision=1, busy=False, active_turns=0)
 channel.send('activity.snapshot', revision=2, busy=True, active_turns=1)
 time.sleep(0.7)
 channel.send('action.request', request_id='delete-enter', name='delete.react', ttl_ms=12000)
 channel.ack('delete-enter', 'started')
 channel.send('action.request', request_id='wave-busy', name='wave', ttl_ms=12000)
 channel.ack('wave-busy', 'rejected')
 channel.ack('delete-enter', 'finished')
 time.sleep(1.0)
 channel.send('activity.snapshot', revision=3, busy=False, active_turns=0)
 channel.send('action.request', request_id='work-end', name='work.finished', ttl_ms=12000)
 channel.ack('work-end', 'started')
 time.sleep(0.25)
 channel.send('action.cancel', request_id='work-end', reason='priority')
 channel.ack('work-end', 'interrupted')
 channel.send('action.request', request_id='delete-exit', name='delete.react', ttl_ms=12000)
 channel.ack('delete-exit', 'started')
 channel.send('action.request', request_id='lower-finish', name='work.finished', ttl_ms=12000)
 channel.ack('lower-finish', 'rejected')
 channel.ack('delete-exit', 'finished')
 channel.send('activity.snapshot', revision=4, busy=True, active_turns=1)
 time.sleep(0.6)
 channel.close()
 time.sleep(1.15)
 channel = Channel(2)
 channel.send('activity.snapshot', revision=1, busy=True, active_turns=2)
 # Old connection/sequence must not advance the new connection's sequence.
 stale = dict(v=1,type='activity.snapshot',session_id=channel.session,connection_id='connection-1',seq=500,revision=999,busy=False)
 channel.socket.sendall((json.dumps(stale)+'\n').encode())
 channel.send('activity.snapshot', revision=2, busy=True, active_turns=1)
 time.sleep(2.6)
 channel.send('action.request', request_id='ttl', name='delete.react', ttl_ms=100)
 channel.ack('ttl', 'started')
 channel.ack('ttl', 'interrupted')
 channel.send('activity.snapshot', revision=3, busy=False, active_turns=0)
 channel.send('action.request', request_id='work-end-2', name='work.finished', ttl_ms=12000)
 channel.ack('work-end-2', 'started')
 time.sleep(.2)
 channel.send('activity.snapshot', revision=4, busy=True, active_turns=1)
 channel.ack('work-end-2', 'interrupted')
 channel.send('activity.snapshot', revision=5, busy=False, active_turns=0)
 channel.send('action.request', request_id='work-end-3', name='work.finished', ttl_ms=12000)
 channel.ack('work-end-3', 'started')
 channel.ack('work-end-3', 'finished')
 time.sleep(2.7)
 channel.send('host.shutdown')
 channel.close()
 process.wait(timeout=6)
 log.close()
 report=json.loads((CAPTURE/'report.json').read_text(encoding='utf8'))
 trace=json.loads((CAPTURE/'trace.json').read_text(encoding='utf8'))['samples']
 assert process.returncode == 0 and report['errors']==[]
 assert report['host_mode']['capability_details']['typing'] is False
 assert any(entry['state']=='host-work' for entry in trace)
 assert any(entry['host']['sit_progress']>0 and entry['host']['sit_progress']<1 for entry in trace)
 assert any(e.get('reason')=='stale_connection_ignored' for e in report['host_events'])
 assert len([e for e in report['host_events'] if e['event']=='connected']) == 2
 assert trace[-1]['state']=='host-idle' and trace[-1]['host']['sit_progress']==0
 assert all(all(max(abs(s-1) for s in bone['scale']) < 1e-5 for bone in frame['bones'].values()) for frame in trace)
 anchors = {name: trace[0]['bones'][name]['global_position'] for name in ['foot.L','foot.R']}
 max_foot_error=max(sum((frame['bones'][name]['global_position'][i]-point[i])**2 for i in range(3))**.5 for frame in trace for name,point in anchors.items())
 assert max_foot_error < .002, max_foot_error
 output={'status':'passed','gpu':False,'messages':messages,'trace_frames':len(trace),'maximum_foot_anchor_error_m':max_foot_error,'foot_anchor_tolerance_m':.002,'limits':['headless transport/base-pose numerical verification only; no visual approval','30Hz authored IK samples interpolate with sub-millimeter foot drift; no exact-zero claim']}
 (ROOT/'socket-check.json').write_text(json.dumps(output, indent=2),encoding='utf8')
 print(json.dumps({'status':'passed','trace_frames':len(trace),'maximum_foot_anchor_error_m':max_foot_error}))
except Exception:
 traceback.print_exc()
 raise
finally:
 if process.poll() is None: process.terminate(); process.wait(timeout=5)
 if not log.closed: log.close()
 server.close()
