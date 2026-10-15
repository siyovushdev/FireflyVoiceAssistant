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
    profile = {'host':args.host, 'local_bind':'127.0.0.1:18089', 'remote_bind':'127.0.0.1',
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
    if getattr(args, 'admin_stress', False):
        profile['admin_stress'] = True
        profile['stress_client_sha256'] = hashlib.sha256((REPO/'benchmarks/web-admin-b4/DialogueSmoke.py').read_bytes()).hexdigest()
        profile['diagnostic_only'] = bool(getattr(args, 'diagnostic_candidate', None))
        if profile['diagnostic_only']:
            profile['diagnostic_candidate'] = {'path':args.diagnostic_candidate, 'sha256':args.diagnostic_candidate_sha256}
    return profile

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

def diagnostic_outcome(report, returncode):
    """Classify this single request, never a later cancel/restore failure."""
    if report.get('profile') != 'ADMIN4096_DIAGNOSTIC' or report.get('real_rkllm') is not True:
        return None
    cycles = report.get('cycles', [])
    if len(cycles) != 1 or len(cycles[0].get('cases', [])) != 1: return None
    case = cycles[0]['cases'][0]
    if case.get('case') != 'long' or case.get('input_bytes') != 1606: return None
    if not returncode and report.get('passed') is True and cycles[0].get('restored') is True and not report.get('http_error'):
        # A valid clarification is not the requested proposal. Check after the
        # client has restored Runtime, without another inference or Apply.
        try:
            state = case['state']; config = state['configuration']
            if state['status'] != 'editing' or state['message_incomplete'] is not False or state.get('questions'): return None
            if [len(config[key]) for key in ('rooms','devices','actions')] != [1,1,2] or config['bindings']: return None
            room = config['rooms'][0]; device = config['devices'][0]
            if room['name'] != 'Кабинет' or device['name'] != 'Лампа' or device['room_id'] != room['id']: return None
            if not isinstance(report.get('initial_system'),dict) or config['system'] != report['initial_system']: return None
            if device['kind'] != 'generic' or len(device['state']) != 1: return None
            if device['state'][0]['id'] != 'power' or device['state'][0]['type'] != 'boolean': return None
            actions = config['actions']
            if set(device['action_ids']) != {a['id'] for a in actions}: return None
            for action in actions:
                if not action['enabled'] or action['parameters'] or len(action['assignments']) != 1: return None
                assignment = action['assignments'][0]
                if assignment['field'] != 'power' or type(assignment['value']) is not bool: return None
            if {a['assignments'][0]['value'] for a in actions} != {False,True}: return None
        except (KeyError, TypeError, IndexError): return None
        return 'AI_REJECTION_NOT_REPRODUCED'
    error = report.get('http_error', {})
    if (not returncode or report.get('passed') is not False or error.get('status') != 409 or
            error.get('path') != '/api/setup/message' or error.get('method') != 'POST' or
            not error.get('capture_complete') or case.get('state') is not None or
            case.get('private_status') != 'planning_failed' or case.get('message_incomplete') is not True): return None
    traces = case.get('trace', [])
    if not traces or any(t.get('stage') != 'decode' or t.get('error_code') != 'invalid_ai_response' for t in traces): return None
    try:
        if all(t.get('error') and not t.get('rejected_raw_truncated', True) and
               len(bytes.fromhex(t.get('rejected_raw_hex', ''))) == t.get('rejected_raw_bytes', -1) for t in traces):
            return 'AI_REJECTION_CAPTURED'
    except (TypeError, ValueError): pass
    return None

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
    parser.add_argument('--admin-stress', action='store_true', help='Two private Admin4096 cycles using the installed immutable ELF; no voice/Apply')
    parser.add_argument('--diagnostic-candidate', help='Root-private pinned descriptor; one long request only')
    parser.add_argument('--diagnostic-candidate-sha256')
    args = parser.parse_args()
    if args.diagnostic_candidate:
        if not args.admin_stress or args.remote_repo != '/home/firefly/FireflyVoiceAssistantLinux-m19-format' or not args.diagnostic_candidate_sha256:
            parser.error('diagnostic requires admin-stress, exact diagnostic source and descriptor SHA256')
    if args.admin_stress and (args.voice_mode != 'owner-live' or args.remote_repo != ('/home/firefly/FireflyVoiceAssistantLinux-m19-format' if args.diagnostic_candidate else '/home/firefly/FireflyVoiceAssistantLinux-m19-rollout')):
        parser.error('stress requires owner-live (no playback) and the exact clean rollout source')
    if args.remote_repo != REMOTE_REPO and args.mode != 'offline-gate' and not args.admin_stress:
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
        if args.admin_stress:
            stress_proof = transport.call('offline-stress', timeout=110, root=offline['root'], diagnostic_only=bool(args.diagnostic_candidate))
            if not stress_proof.get('passed') or stress_proof.get('real_rkllm'):
                raise RuntimeError('Offline stress transport gate failed or mislabelled inference')
            (root / 'offline-stress.json').write_text(json.dumps(stress_proof, indent=2))
        proof = run_browser(args, transport, root, offline, profile)
        if args.admin_stress: proof['admin_stress_gate'] = stress_proof['gate']
        gate = transport.call('record-gate', proof=proof)
        (root / 'gate.json').write_text(json.dumps(gate, indent=2))
    finally:
        stopped = transport.call('offline-stop', root=offline['root'])
        (root / 'offline-stopped.json').write_text(json.dumps(stopped))
    if args.mode == 'offline-gate':
        print(json.dumps({'result':'OFFLINE_BROWSER_GATE_PASS','artifacts':str(root)})); return
    prepared = transport.call('prepare', timeout=120, gate_path=gate['gate_path'], voice_mode=args.voice_mode,
                              profile='admin-diagnostic' if args.diagnostic_candidate else 'admin-stress' if args.admin_stress else 'voice',
                              diagnostic_candidate=profile.get('diagnostic_candidate'))
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
        if args.admin_stress:
            # Same exclusive window, original deadline, heartbeat and recovery.
            # Direct Admin API is deliberate for load testing, not a browser-E2E claim.
            remote_root = Path(prepared['root'])
            # Firefly monotonic and the armed deadline come from Firefly,
            # never from the PC clock.
            arm = transport.call('test-status', root=prepared['root'])['arm']
            command = ('sudo -n /usr/bin/python3 -B ' + shlex.quote(str(remote_root / 'scripts/admin-stress-client.py')) +
                ' --stress-url ' + shlex.quote('http://127.0.0.1:' + str(prepared['admin_port'])) +
                ' --store ' + shlex.quote(str(Path(prepared['plan']['test_log']).parent.parent / 'store')) +
                ' --mock-log ' + shlex.quote(prepared['plan']['mock_log']) +
                ' --output ' + shlex.quote(str(remote_root / 'evidence/admin-stress.json')) +
                ' --deadline-monotonic ' + shlex.quote(str(arm['deadline'] - 30)))
            if args.diagnostic_candidate: command += ' --diagnostic-only'
            completed = subprocess.run(['/usr/bin/ssh', *transport.options, transport.host, command],
                capture_output=True, text=True, timeout=600)
            (root / 'stress-client.stdout').write_text(completed.stdout)
            (root / 'stress-client.stderr').write_text(completed.stderr)
            diagnostic_result = None
            if args.diagnostic_candidate:
                report = transport.call('diagnostic-result', root=prepared['root'])
                (root / 'diagnostic-result.json').write_text(json.dumps(report,indent=2))
                diagnostic_result = diagnostic_outcome(report, completed.returncode)
                if not diagnostic_result: raise RuntimeError('Diagnostic outcome incomplete or failure outside long-request decode')
            if completed.returncode and not diagnostic_result: raise RuntimeError('Admin stress client failed: ' + completed.stderr[-1500:])
            actual_rows = transport.call('rows', root=prepared['root'])
            (root / 'stress-mock-rows.json').write_text(json.dumps(actual_rows, indent=2))
            if actual_rows: raise RuntimeError('Unexpected HTTP mock traffic during Admin stress')
        else:
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
    print(json.dumps({'result':diagnostic_result if args.diagnostic_candidate else 'ADMIN_STRESS_AND_RECOVERY_PASS' if args.admin_stress else 'BROWSER_TRIAL_AND_RECOVERY_PASS','artifacts':str(root)}))

if __name__ == '__main__': main()
