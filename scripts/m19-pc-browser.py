#!/usr/bin/env python3
"""PC transport/orchestration for the existing M19 controlled Chromium driver.

prepare-only executes a fresh offline gate and read-only hardware preparation.
trial is a separate, explicitly authorized invocation; recovery stays on Firefly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import signal
import subprocess
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parents[1]
REMOTE_REPO = '/home/firefly/FireflyVoiceAssistantLinux-m19'
BRIDGE = REMOTE_REPO + '/scripts/m19-pc-bridge.py'

class Transport:
    def __init__(self, host, control_path, remote_repo=REMOTE_REPO):
        self.host = host
        root = Path(remote_repo)
        if not root.is_absolute() or '..' in root.parts or str(root) != remote_repo:
            raise ValueError('remote repository must be an absolute normalized path')
        self.remote_repo = remote_repo
        self.bridge = remote_repo + '/scripts/m19-pc-bridge.py'
        self.options = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5']
        if control_path: self.options += ['-S', control_path]

    def call(self, action, timeout=30, **detail):
        result = subprocess.run(['/usr/bin/ssh', *self.options, self.host,
            'sudo -n /usr/bin/python3 -B ' + shlex.quote(self.bridge)],
            input=json.dumps({'action':action, **detail}), capture_output=True,
            text=True, timeout=timeout)
        if result.returncode: raise RuntimeError(result.stderr or result.stdout)
        return json.loads(result.stdout)

    def tunnel(self, remote_port, open=True):
        # Offline and live use the same browser origin and loopback-only SSH policy.
        result = subprocess.run(['/usr/bin/ssh', *self.options, '-O', 'forward' if open else 'cancel',
            '-L', f'127.0.0.1:18089:127.0.0.1:{remote_port}', self.host],
            capture_output=True, text=True, timeout=10)
        if result.returncode: raise RuntimeError(result.stderr)

def browser_profile(args):
    chrome = Path(args.chromium).resolve()
    return {'host':args.host, 'local_bind':'127.0.0.1:18089', 'remote_bind':'127.0.0.1',
            'remote_repo':args.remote_repo,
            'ssh_options':['BatchMode=yes','ConnectTimeout=5'],
            'chromium':str(chrome), 'chromium_sha256':hashlib.sha256(chrome.read_bytes()).hexdigest(),
            'chromium_version':subprocess.check_output([str(chrome),'--version'],text=True).strip(),
            'node_version':subprocess.check_output([args.node,'--version'],text=True).strip(),
            'uid':os.getuid(),'display':os.environ['DISPLAY'],'pc_host':socket.gethostname(),
            'voice_mode':args.voice_mode,
            'runner_sha256':hashlib.sha256((REPO/'tests/admin/v2/M19ChromiumTests.js').read_bytes()).hexdigest(),
            'adapter_sha256':hashlib.sha256((REPO/'tests/admin/v2/M19Remote.js').read_bytes()).hexdigest(),
            'orchestrator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'node_sha256':hashlib.sha256(Path(args.node).resolve().read_bytes()).hexdigest(),
            'chromium_binary_sha256':hashlib.sha256((chrome.parent/'chrome' if (chrome.parent/'chrome').is_file() else chrome).read_bytes()).hexdigest()}

def run_driver(command, env, log, timeout):
    # This session/group contains only the driver and its own Chromium children.
    child = subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True)
    try:
        return child.wait(timeout=timeout)
    finally:
        try: os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError: pass
        try: child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            try: os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            child.wait(timeout=3)
        # The driver may exit before a Chromium grandchild finishes shutting down.
        end = time.monotonic() + 2
        while time.monotonic() < end:
            try: os.killpg(child.pid, 0)
            except ProcessLookupError: break
            time.sleep(.05)
        try: os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError: pass

def wait_recovery(transport, remote_root, artifacts, timeout=360):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        status = transport.call('recovery-status', root=remote_root)
        if status['passed']:
            (artifacts / 'RECOVERY_CHECK_PASS.json').write_text(json.dumps(status,indent=2))
            return status
        if status['state'] == 'failed': raise RuntimeError('Firefly recovery failed; independent evidence retained')
        time.sleep(1)
    raise RuntimeError('Recovery was not verified within its bounded window')

def wait_test_ready(transport, remote_root, artifacts, timeout=120):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        status = transport.call('test-status', root=remote_root)
        if status['ready']:
            (artifacts / 'runtime-ready.json').write_text(json.dumps(status,indent=2))
            return status
        time.sleep(.5)
    raise RuntimeError('Test runtime readiness deadline exceeded')

def collect_evidence(transport, prepared, artifacts):
    remote_root = prepared['root']
    workspace = str(Path(prepared['plan']['candidate_binary']).parents[2])
    for filename, command in (
            ('firefly-supervisor.tar', ['tar','-C',remote_root,'-cf','-','.']),
            ('firefly-evidence.tar', ['tar','-C',workspace,'-cf','-','evidence','prepared.json'])):
        with (artifacts / filename).open('wb') as output:
            subprocess.run(['/usr/bin/ssh',*transport.options,transport.host,
                            'sudo -n ' + shlex.join(command)],stdout=output,check=True,timeout=30)

def relay_owner_cues(path, stopped, emit=None):
    """Expose existing driver events to the PC controller while its log is saved."""
    if emit is None:
        emit = lambda cue: print(json.dumps(cue,ensure_ascii=False),flush=True)
    seen = 0
    while True:
        if path.exists():
            for line in path.read_text().splitlines(keepends=True)[seen:]:
                if not line.endswith('\n'): break  # A concurrent append may be partial.
                event = json.loads(line); seen += 1
                if event.get('phase') == 'owner_waiting_ready':
                    emit({'phase':'OWNER_WAITING_READY','on':event['on'],
                          'readyFile':event['readyFile'],'requestId':event['requestId'],
                          'timeoutSeconds':event['timeoutSeconds']})
                elif event.get('phase') == 'owner_voice_ready':
                    emit({'phase':'OWNER_VOICE_READY','on':event['on'],
                          'instruction':event['instruction'],'timeoutSeconds':event['timeoutSeconds']})
        if stopped.wait(.2): return

def run_browser(args, transport, root, data, profile, live=False, offline_proof=None):
    artifacts = root / ('browser-live' if live else 'browser-offline')
    artifacts.mkdir(mode=0o700)
    config = {'host':args.host,'controlPath':args.control_path,'bridge':transport.bridge,
              'remoteRoot':data['root'],'remoteHost':'firefly','mockUrl':data['mock_url'],
              'appSha256':data['app_sha256'],'workerSha256':data['worker_sha256'],
              'transport':profile}
    config['voiceMode'] = args.voice_mode
    if live and data['plan']['voice_mode'] != args.voice_mode:
        raise RuntimeError('Prepared voice mode differs from PC request')
    config_path = artifacts / 'remote-plan.json'
    config_path.write_text(json.dumps(config)); config_path.chmod(0o600)
    transport.tunnel(data['admin_port'])
    cue_stop = threading.Event()
    cue_errors = []
    cue_thread = None
    if live and args.voice_mode == 'owner-live':
        def relay():
            try: relay_owner_cues(artifacts / 'controlled-events.jsonl',cue_stop)
            except Exception as error: cue_errors.append(str(error))
        cue_thread = threading.Thread(target=relay,daemon=True); cue_thread.start()
    try:
        command = [args.node, str(REPO / 'tests/admin/v2/M19ChromiumTests.js'),
            transport.remote_repo + '/build/firefly-admin', transport.remote_repo + '/build/src/admin/m19_test_worker',
            transport.remote_repo, args.chromium, str(artifacts),
            '--controlled-live' if live else '--controlled-offline',
            'http://127.0.0.1:18089', data['mock_log'], data['mock_url']]
        env = {**os.environ, 'FIREFLY_M19_REMOTE_PLAN':str(config_path)}
        with (artifacts / 'driver.log').open('w') as log:
            result = run_driver(command, env, log, timeout=600 if live else 180)
        if result: raise RuntimeError('Browser scenario failed: ' + str(artifacts / 'driver.log'))
        if cue_errors: raise RuntimeError('Owner prompt relay failed: ' + cue_errors[0])
        proof = json.loads((artifacts / 'results.json').read_text())
        if proof['transport'] != profile or proof['uid'] != os.getuid() or proof['host'] != socket.gethostname():
            raise RuntimeError('Browser environment did not match the PC')
        if live:
            for field in ('runnerSha256','adapterSha256','browserArgs','display','uid','host','transport'):
                if proof[field] != offline_proof[field]: raise RuntimeError('Live driver differs from offline gate: ' + field)
        return proof
    finally:
        cue_stop.set()
        if cue_thread: cue_thread.join(timeout=3)
        transport.tunnel(data['admin_port'], open=False)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('offline-gate','prepare-only','trial'))
    parser.add_argument('--host', default='firefly@192.168.68.111')
    parser.add_argument('--control-path', default='/tmp/m19-firefly-control-%u')
    parser.add_argument('--remote-repo', default=REMOTE_REPO,
                        help='Explicit clean candidate source for offline-gate only')
    parser.add_argument('--chromium', default=shutil.which('chromium') or shutil.which('google-chrome'))
    parser.add_argument('--node', default=shutil.which('node'))
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--owner-start', choices=('СТАРТ',))
    parser.add_argument('--voice-mode', choices=('synthetic-acoustic','owner-live'), default='synthetic-acoustic')
    args = parser.parse_args()
    if args.remote_repo != REMOTE_REPO and args.mode != 'offline-gate':
        parser.error('alternate clean source is permitted only for offline-gate')
    if args.mode == 'trial' and args.owner_start != 'СТАРТ':
        parser.error('trial requires a NEW explicit owner СТАРТ; preparation never authorizes trial')
    if socket.gethostname() == 'firefly' or not args.chromium or not args.node or not os.environ.get('DISPLAY'):
        parser.error('Run on the PC with its X11 Chromium environment')
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='pc-window-', dir=args.artifacts)).resolve()
    transport = Transport(args.host, args.control_path, args.remote_repo)
    profile = browser_profile(args)
    offline = transport.call('offline-start')
    (root / 'offline-backend.json').write_text(json.dumps(offline, indent=2))
    try:
        proof = run_browser(args, transport, root, offline, profile)
        gate = transport.call('record-gate', proof=proof)
        (root / 'gate.json').write_text(json.dumps(gate, indent=2))
    finally:
        stopped = transport.call('offline-stop', root=offline['root'])
        (root / 'offline-stopped.json').write_text(json.dumps(stopped))
    if args.mode == 'offline-gate':
        print(json.dumps({'result':'OFFLINE_BROWSER_GATE_PASS','artifacts':str(root)})); return
    prepared = transport.call('prepare', timeout=120, gate_path=gate['gate_path'], voice_mode=args.voice_mode)
    (root / 'prepared.json').write_text(json.dumps(prepared, indent=2))
    if args.mode == 'prepare-only':
        transport.call('stop-prepared-mock', root=prepared['root'])
        print(json.dumps({'result':'PREPARED_NOT_ARMED','artifacts':str(root),'supervisor':prepared['root']})); return
    # Keep all connection-loss decisions on Firefly. PC heartbeat is only evidence.
    stopped = threading.Event()
    errors = []
    def heartbeat():
        while not stopped.is_set():
            try: transport.call('heartbeat', timeout=8, root=prepared['root'], token=prepared['lease_token'])
            except Exception as error: errors.append(str(error)); return
            stopped.wait(3)
    if browser_profile(args) != profile: raise RuntimeError('PC browser profile changed after gate')
    transport.call('heartbeat', root=prepared['root'], token=prepared['lease_token'])
    thread = threading.Thread(target=heartbeat, daemon=True); thread.start()
    try:
        transport.call('start-window', timeout=120, root=prepared['root'])
        wait_test_ready(transport, prepared['root'], root)
        live = {**prepared,'app_sha256':offline['app_sha256'],'worker_sha256':offline['worker_sha256']}
        run_browser(args, transport, root, live, profile, live=True, offline_proof=proof)
        if errors: raise RuntimeError('SSH heartbeat failed: ' + errors[0])
    finally:
        stopped.set(); thread.join(timeout=10)
        try:
            recovery = transport.call('recover', root=prepared['root'])
            if not recovery.get('unarmed'):
                wait_recovery(transport, prepared['root'], root)
        finally:
            collect_evidence(transport, prepared, root)
    print(json.dumps({'result':'BROWSER_TRIAL_AND_RECOVERY_PASS','artifacts':str(root)}))

if __name__ == '__main__': main()
