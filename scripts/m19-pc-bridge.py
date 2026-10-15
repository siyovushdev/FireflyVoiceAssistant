#!/usr/bin/python3
"""SSH transport for M19 fixture/files/audio; never launches a browser or Codex.

Offline services reuse the existing firefly-admin/m19_test_worker and mock.
Live playback requires an armed, unconsumed window and the AI_PASS marker.
"""
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
# 90s speech + 5s diagnostics + 15s GET click + 15s confirmation + 15s margin.
OWNER_VOICE_RESERVE_SECONDS = 140

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run(argv, timeout=20):
    p = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f'{argv}: {p.stderr or p.stdout}')
    return p.stdout.strip()

def load_module(name, file):
    spec = importlib.util.spec_from_file_location(name, REPO / 'scripts' / file)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

def canonical_root(value):
    root = Path(value)
    if (root.parent != Path('/var/lib') or root.resolve() != root or
            not root.name.startswith(('firefly-m19-pc-offline-', 'firefly-m19-window-')) or
            root.stat().st_uid != 0 or root.stat().st_mode & 0o077):
        raise RuntimeError('Unknown/private M19 root required')
    return root

def fixture(root):
    data = json.loads((root / 'offline.json').read_text())
    if data['root'] != str(root): raise RuntimeError('Fixture root changed')
    return data

def hardware(root):
    recovery = load_module('pc_recovery', 'm19-hardware-recovery.py')
    window = recovery.Window(root)
    arm = window.armed()
    if ((root / 'CLOSED').exists() or not (root / 'TEST_STARTED').exists() or
            time.monotonic() >= arm['deadline']):
        raise RuntimeError('Hardware window is not admitted/active')
    window.pc_connection()
    return window

def offline_start():
    user = pwd.getpwnam('firefly')
    root = Path(tempfile.mkdtemp(prefix='firefly-m19-pc-offline-', dir='/var/lib'))
    work = REPO / 'build' / root.name; work.mkdir(mode=0o700)
    os.chown(work, user.pw_uid, user.pw_gid)
    store = work / 'store'; store.mkdir(mode=0o700); os.chown(store, user.pw_uid, user.pw_gid)
    app = REPO / 'build/firefly-admin'; worker = REPO / 'build/src/admin/m19_test_worker'
    mock_log = work / 'mock.jsonl'  # Existing mock creates the fresh log with exclusive open.
    backend_unit = root.name + '-backend.service'; mock_unit = root.name + '-mock.service'
    data = {'root':str(root), 'work':str(work), 'store':str(store),
            'control':str(work / 'voice'), 'mock_log':str(mock_log),
            'backend_unit':backend_unit, 'mock_unit':mock_unit,
            'app_sha256':sha(app), 'worker_sha256':sha(worker),
            'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
    (root / 'offline.json').write_text(json.dumps(data)); (root / 'offline.json').chmod(0o600)
    def service(unit, log, argv, env=()):
        return ['systemd-run', '--unit=' + unit, '--property=User=firefly', '--property=Group=firefly',
                '--property=Restart=no', '--property=RuntimeMaxSec=240', '--property=KillMode=control-group',
                '--property=TimeoutStopSec=10', '--property=MemoryMax=512M',
                '--property=DevicePolicy=closed', '--property=NoNewPrivileges=true',
                '--property=IPAddressDeny=any', '--property=IPAddressAllow=localhost',
                '--property=StandardOutput=append:' + str(log), '--property=StandardError=append:' + str(log),
                *env, *argv]
    try:
        run(service(mock_unit, work / 'mock.stdout', ['/usr/bin/python3', '-B', REPO / 'scripts/mock-http-device.py',
                '--port', '0', '--request-log', mock_log]))
        run(service(backend_unit, work / 'backend.log', [app, '--store', store,
                '--init-config', REPO / 'config/empty-home.json', '--runtime-worker', worker, '--admin-port', '0'],
                ['--setenv=FIREFLY_M19_VOICE_CONTROL=' + data['control']]))
        end = time.monotonic() + 20
        while time.monotonic() < end:
            backend = (work / 'backend.log').read_text() if (work / 'backend.log').exists() else ''
            mock = (work / 'mock.stdout').read_text() if (work / 'mock.stdout').exists() else ''
            a = re.search(r'ADMIN_LISTEN (http://127\.0\.0\.1:(\d+))', backend)
            m = re.search(r'MOCK_LISTEN (http://127\.0\.0\.1:\d+)/relay/1', mock)
            if a and m:
                data.update(admin_url=a[1], admin_port=int(a[2]), mock_url=m[1])
                (root / 'offline.json').write_text(json.dumps(data)); (root / 'offline.json').chmod(0o600)
                return data
            time.sleep(.1)
        raise RuntimeError('Offline listeners did not become ready')
    except Exception:
        for unit in (backend_unit, mock_unit): subprocess.run(['systemctl', 'stop', unit], capture_output=True)
        raise

def offline_root(value):
    root = Path(value)
    # Metadata is root-owned/private; only the separate work directory is user-owned.
    if (root.parent != Path('/var/lib') or root.resolve() != root or
            not root.name.startswith('firefly-m19-pc-offline-') or root.stat().st_uid != 0 or root.stat().st_mode & 0o077 or
            (root / 'offline.json').stat().st_uid != 0):
        raise RuntimeError('Unknown offline fixture')
    return root

def memory(root):
    offline = (root / 'offline.json').exists()
    unit = fixture(root)['backend_unit'] if offline else hardware(root).plan['test_unit']
    props = dict(line.split('=', 1) for line in run(['systemctl','show',unit,'-p','MainPID','-p','ControlGroup']).splitlines())
    pid = int(props['MainPID'])
    if pid <= 0: raise RuntimeError('Backend is not running')
    group = Path('/sys/fs/cgroup') / props['ControlGroup'].lstrip('/')
    return {'host':os.uname().nodename, 'meminfo':Path('/proc/meminfo').read_text(),
            'backendPid':pid, 'backendPss':Path(f'/proc/{pid}/smaps_rollup').read_text(),
            'backendCgroup':props['ControlGroup'], 'backendCgroupBytes':(group / 'memory.current').read_text()}

def audio_worker(root):
    window = hardware(root)
    if window.plan.get('voice_mode') != 'synthetic-acoustic': raise RuntimeError('Playback forbidden in owner-live mode')
    if not (root / 'AI_PASS').exists(): raise RuntimeError('AI has not passed')
    on = json.loads((root / 'AUDIO_REQUEST.json').read_text())['on']
    if type(on) is not bool: raise RuntimeError('Invalid playback direction')
    label = 'on' if on else 'off'
    (root / ('AUDIO_' + label.upper() + '_STARTED')).open('x').close()
    wav = Path(window.plan['audio_files'][label]['path'])
    if sha(wav) != window.plan['audio_files'][label]['sha256']: raise RuntimeError('Audio changed')
    subprocess.run(['/usr/bin/aplay','-q','-D','plughw:CARD=rockchipes8388,DEV=0',str(wav)],
                   check=True, timeout=15)

def dispatch(request):
    action = request['action']
    if action == 'offline-start': return offline_start()
    if action == 'record-gate':
        proof = request['proof']
        root = Path(tempfile.mkdtemp(prefix='firefly-m19-pc-gate-', dir='/var/lib'))
        (root / 'results.json').write_text(json.dumps(proof, indent=2)); (root / 'results.json').chmod(0o600)
        gate = {'proof_path':str(root / 'results.json'), 'proof_sha256':sha(root / 'results.json'),
                'runner_sha256':sha(REPO / 'tests/admin/v2/M19ChromiumTests.js'),
                'adapter_sha256':sha(REPO / 'tests/admin/v2/M19Remote.js'),
                'bridge_sha256':sha(__file__),
                'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                'checked_monotonic':time.monotonic()}
        (root / 'gate.json').write_text(json.dumps(gate, indent=2)); (root / 'gate.json').chmod(0o600)
        launch = load_module('pc_launch','m19-hardware-start.py'); launch.verify_browser_gate({'browser_gate':gate})
        return {'gate_path':str(root / 'gate.json'), 'gate':gate}
    if action == 'prepare':
        launch = load_module('pc_launch','m19-hardware-start.py')
        profile = request.get('profile', 'voice')
        if profile not in ('voice', 'admin-stress', 'admin-diagnostic'): raise RuntimeError('Unknown M19 test profile')
        root = (launch.prepare(Path(request['gate_path']), 'owner-live', True, request['diagnostic_candidate']) if profile == 'admin-diagnostic' else
                launch.prepare(Path(request['gate_path']), 'owner-live', True) if profile == 'admin-stress' else
                launch.prepare(Path(request['gate_path']), request.get('voice_mode','synthetic-acoustic')))
        plan = json.loads((root / 'plan.json').read_text())
        return {'root':str(root),'admin_port':plan['admin_port'],'mock_url':plan['mock_url'],
                'mock_log':plan['mock_log'],'lease_token':plan['pc_lease_token'],'plan':plan}
    root = offline_root(request['root']) if 'pc-offline-' in request['root'] else canonical_root(request['root'])
    if action == 'offline-stop':
        data = fixture(root)
        for unit in (data['backend_unit'], data['mock_unit']): run(['systemctl','stop',unit])
        return {'stopped':True, 'backend_exit':run(['systemctl','show',data['backend_unit'],'-p','ExecMainStatus','--value'])}
    if action == 'offline-stress':
        data = fixture(root)
        work = Path(data['work'])
        (work / 'offline-test-marker').write_text('offline scripted transport gate only\n')
        output = work / 'admin-stress-offline.json'
        client = REPO / 'benchmarks/web-admin-b4/DialogueSmoke.py'
        client_sha = sha(client)
        diagnostic = request.get('diagnostic_only', False)
        if type(diagnostic) is not bool: raise RuntimeError('Invalid diagnostic mode')
        flags = ['--diagnostic-only'] if diagnostic else []
        workload = json.loads(run(['/usr/bin/python3', '-B', client, '--validate-workload', *flags]))
        run(['/usr/bin/python3', '-B', REPO / 'benchmarks/web-admin-b4/DialogueSmoke.py',
             '--stress-url', data['admin_url'], '--store', data['store'], '--output', output,
             '--mock-log', data['mock_log'],
             '--deadline-monotonic', str(time.monotonic() + 90), '--offline', *flags], timeout=100)
        proof = json.loads(output.read_text())
        if sha(client) != client_sha or not proof['passed'] or proof['real_rkllm']:
            raise RuntimeError('Offline stress client changed or proof invalid')
        proof_path = root / 'admin-stress-offline.json'
        proof_path.write_bytes(output.read_bytes()); proof_path.chmod(0o600)
        gate = {'proof_path': str(proof_path), 'proof_sha256': sha(proof_path),
                'client_sha256': client_sha, 'workload': workload,
                'workload_sha256': hashlib.sha256(json.dumps(workload, sort_keys=True, ensure_ascii=False).encode()).hexdigest(),
                'boot_id': data['boot_id'], 'offline_root': str(root), 'diagnostic_only': diagnostic,
                'checked_monotonic': time.monotonic()}
        receipt = root / 'admin-stress-gate.json'
        receipt.write_text(json.dumps(gate)); receipt.chmod(0o600)
        return {**proof, 'gate': {'path': str(receipt), 'sha256': sha(receipt)}}
    if action == 'diagnostic-result':
        window = load_module('diagnostic_recovery','m19-hardware-recovery.py').Window(root)
        if window.plan.get('profile') != 'ADMIN4096_DIAGNOSTIC': raise RuntimeError('Not a diagnostic window')
        report = json.loads((root / 'evidence/admin-stress.json').read_text())
        if report['profile'] != 'ADMIN4096_DIAGNOSTIC' or not report['real_rkllm']: raise RuntimeError('Not a real diagnostic result')
        return report
    if action == 'memory': return memory(root)
    if action == 'rows':
        log = fixture(root)['mock_log'] if (root / 'offline.json').exists() else hardware(root).plan['mock_log']
        return [json.loads(line) for line in Path(log).read_text().splitlines() if line]
    if action == 'voice':
        data = fixture(root)
        control = Path(data['control']); partial = control.with_suffix('.tmp')
        partial.write_text(json.dumps(request['fixture'])); os.chown(partial, pwd.getpwnam('firefly').pw_uid, pwd.getpwnam('firefly').pw_gid)
        partial.replace(control)
        end = time.monotonic() + 15
        while time.monotonic() < end:
            result = Path(str(control) + '.result')
            if result.exists():
                value = json.loads(result.read_text())
                if value['id'] == request['fixture']['id']:
                    if control.exists(): raise RuntimeError('Fixture not consumed')
                    return value
            time.sleep(.05)
        raise RuntimeError('Voice fixture timeout')
    if action == 'heartbeat':
        plan = json.loads((root / 'plan.json').read_text())
        if request['token'] != plan['pc_lease_token'] or (root / 'CLOSED').exists(): raise RuntimeError('Unknown/closed lease')
        partial = root / 'PC_HEARTBEAT.tmp'
        partial.write_text(json.dumps({'token':request['token'],'monotonic':time.monotonic(),'boot_id':plan['boot_id']}))
        partial.replace(root / 'PC_HEARTBEAT.json'); return {'ok':True}
    if action == 'start-window':
        launch = load_module('pc_launch','m19-hardware-start.py'); launch.start_window(root); return {'submitted':True}
    if action == 'test-status':
        recovery = load_module('pc_recovery','m19-hardware-recovery.py')
        window = recovery.Window(root)
        arm = window.armed()
        if (root/'CLOSED').exists() or time.monotonic() >= arm['deadline']:
            raise RuntimeError('Watchdog closed/expired test window')
        window.pc_connection()
        try:
            status = json.load(urllib.request.urlopen(
                'http://127.0.0.1:' + str(window.plan['admin_port']) + '/api/status',timeout=3))
        except (OSError, ValueError): return {'ready':False}
        if not (status.get('runtime_ready') and status.get('stt_ready') and status.get('voice')=='WAITING_WAKE'):
            return {'ready':False,'status':status}
        pid = int(run(['systemctl','show',window.plan['test_unit'],'-p','MainPID','--value']))
        if pid <= 0 or status.get('worker_pid') != pid or (Path('/proc')/str(pid)/'exe').resolve() != Path(window.plan['candidate_binary']):
            raise RuntimeError('Test readiness executable/PID mismatch')
        return {'ready':True,'status':status,'arm':arm}
    if action == 'ai-pass': hardware(root); (root / 'AI_PASS').open('x').close(); return {'ok':True}
    if action == 'owner-voice-ready':
        window = hardware(root)
        if window.plan.get('voice_mode') != 'owner-live' or not (root / 'AI_PASS').exists():
            raise RuntimeError('Owner voice is not admitted')
        if window.armed()['deadline'] - time.monotonic() < OWNER_VOICE_RESERVE_SECONDS:
            raise RuntimeError('Insufficient time for owner voice and HTTP confirmation')
        status = json.load(urllib.request.urlopen(
            'http://127.0.0.1:' + str(window.plan['admin_port']) + '/api/status',timeout=3))
        pid = int(run(['systemctl','show',window.plan['test_unit'],'-p','MainPID','--value']))
        if (pid <= 0 or status.get('worker_pid') != pid or
                (Path('/proc')/str(pid)/'exe').resolve() != Path(window.plan['candidate_binary']) or
                not status.get('runtime_ready') or not status.get('stt_ready') or
                status.get('voice') != 'WAITING_WAKE'):
            raise RuntimeError('Test voice is not ready after Apply')
        return {'ready':True, 'status':status, 'mode':'owner-live',
                'remainingSeconds':window.armed()['deadline'] - time.monotonic(),
                'readyReserveSeconds':OWNER_VOICE_RESERVE_SECONDS}
    if action == 'replay':
        window = hardware(root)
        if window.plan.get('voice_mode') != 'synthetic-acoustic': raise RuntimeError('Playback forbidden in owner-live mode')
        if not (root / 'AI_PASS').exists() or type(request['on']) is not bool: raise RuntimeError('Audio not admitted')
        (root / 'AUDIO_REQUEST.json').write_text(json.dumps({'on':request['on']}))
        run(['systemctl','start',window.plan['audio_unit']])
        end = time.monotonic() + 20
        while time.monotonic() < end:
            hardware(root)
            state = run(['systemctl','show',window.plan['audio_unit'],'-p','ActiveState','--value'])
            if state in ('inactive','failed'): break
            time.sleep(.1)
        if state != 'inactive' or run(['systemctl','show',window.plan['audio_unit'],'-p','ExecMainStatus','--value']) != '0': raise RuntimeError('Audio failed')
        return window.plan['audio_files']['on' if request['on'] else 'off']
    if action == 'recover':
        plan = json.loads((root / 'plan.json').read_text())
        if not (root / 'ARMED.json').exists(): return {'unarmed':True}
        run(['systemctl','start','--no-block',plan['recovery_unit']]); return {'submitted':True}
    if action == 'recovery-status':
        recovery = load_module('pc_recovery','m19-hardware-recovery.py')
        window = recovery.Window(root)
        evidence = root / 'evidence/RECOVERY_CHECK_PASS.json'
        if evidence.exists():
            window.identity()
            current = window.gates(window.plan['old'])
            return {'passed':True,'evidence':json.loads(evidence.read_text()),'current':current}
        state = run(['systemctl','show',window.plan['recovery_unit'],'-p','ActiveState','--value'])
        return {'passed':False,'state':state,'closed':(root/'CLOSED').exists()}
    if action == 'stop-prepared-mock':
        if (root / 'ARMED.json').exists(): raise RuntimeError('Cannot dispose of an armed window')
        plan = json.loads((root / 'plan.json').read_text()); run(['systemctl','stop',plan['mock_unit']]); return {'stopped':True}
    raise RuntimeError('Unknown operation')

if __name__ == '__main__':
    try:
        if len(sys.argv) == 3 and sys.argv[1] == 'audio-worker':
            audio_worker(canonical_root(sys.argv[2]))
        else:
            # Launcher preparation prints progress; reserve stdout for JSON transport.
            import contextlib
            with contextlib.redirect_stdout(sys.stderr): value = dispatch(json.load(sys.stdin))
            print(json.dumps(value))
    except Exception as error:
        print(str(error), file=sys.stderr); sys.exit(1)
