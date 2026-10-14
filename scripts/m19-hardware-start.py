#!/usr/bin/python3
"""Just-in-time launcher using the existing reviewed M19 candidate/unit templates.

prepare-only runs a fresh loopback mock and read-only production preflight.
start additionally arms independent recovery and submits ONE hardware cutover;
it requires an explicit owner START. Neither mode reuses an old mock or plan.
"""
import argparse
import copy
import datetime
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import secrets
import shlex
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('recovery', REPO / 'scripts/m19-hardware-recovery.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)
from packaging.firefly_release import source_tree_fingerprint

TEMPLATE = REPO / 'build/m19-resource-hw-20261013T172025Z'
BROWSER_RUNNER = REPO / 'tests/admin/v2/M19ChromiumTests.js'


def voice_preflight(mode):
    if mode == 'owner-live':
        return {'mode':mode, 'playback':False, 'physical_voice':'PENDING_OWNER'}
    recovery.require(mode == 'synthetic-acoustic', 'Unknown voice mode')
    # These exact assets failed the frozen PCM KWS on 2026-10-12, before any
    # speaker/microphone path. Reusing them cannot qualify autonomous acceptance.
    raise RuntimeError('VOICE_ASSET_BLOCKED: existing synthetic inline ON/OFF have '
                       'zero frozen KWS wakes (peak mean 0.05692 < 0.99); '
                       'use owner-live or prepare separately verified audio before cutover')


def verify_voice_plan(plan):
    voice_preflight(plan['voice_mode'])
    recovery.require(plan['voice_mode'] == plan['pc_browser']['transport']['voice_mode'],
                     'Voice mode differs from offline browser gate')
    recovery.require(plan['audio_files'] == {}, 'Owner voice must not use playback files')


def verify_browser_gate(plan):
    gate = plan['browser_gate']
    recovery.require(gate['boot_id'] == Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'Browser gate boot changed')
    recovery.require(0 <= time.monotonic() - gate['checked_monotonic'] < 300, 'Browser preflight is stale')
    proof_path = Path(gate['proof_path'])
    recovery.require(proof_path.stat().st_uid == 0 and not proof_path.stat().st_mode & 0o022, 'Untrusted browser proof')
    recovery.require(recovery.rollout.sha(BROWSER_RUNNER) == gate['runner_sha256'] and
                     recovery.rollout.sha(REPO / 'tests/admin/v2/M19Remote.js') == gate['adapter_sha256'] and
                     recovery.rollout.sha(REPO / 'scripts/m19-pc-bridge.py') == gate['bridge_sha256'] and
                     recovery.rollout.sha(proof_path) == gate['proof_sha256'], 'Browser transport/proof changed')
    proof = json.loads(proof_path.read_text())
    recovery.require(proof['passed'] and proof['controlled'] and not proof['live'] and
                     proof['host'] != os.uname().nodename and proof['remoteHost'] == os.uname().nodename and
                     proof['runnerSha256'] == gate['runner_sha256'] and proof['adapterSha256'] == gate['adapter_sha256'],
                     'Browser gate must be the matching PC driver with a Firefly offline backend')
    recovery.require(proof['offlineHarnessAppSha256'] == recovery.rollout.sha(REPO / 'build/firefly-admin') and
                     proof['offlineHarnessWorkerSha256'] == recovery.rollout.sha(REPO / 'build/src/admin/m19_test_worker'),
                     'Offline backend changed')
    transport = proof['transport']
    recovery.require(transport.get('voice_mode') in ('owner-live','synthetic-acoustic') and
                     proof.get('liveVoiceMode') == transport['voice_mode'], 'Voice mode missing from browser gate')
    recovery.require(transport['local_bind'] == '127.0.0.1:18089' and
                     transport['remote_bind'] == '127.0.0.1' and
                     transport['orchestrator_sha256'] == recovery.rollout.sha(REPO / 'scripts/m19-pc-browser.py') and transport['host'] == 'firefly@192.168.68.111' and
                     bool(proof['display']) and transport['chromium_version'] and
                     proof['browserArgs'] == ['--ozone-platform=x11','--no-sandbox','--disable-gpu',
                        '--disable-dev-shm-usage','--disable-background-networking','--no-first-run','--remote-debugging-port=0'],
                     'Unexpected browser/tunnel profile')
    recovery.require(proof['results'] == [{'controlled':True,'live':False,'AI':True,'apply':True,
                     'posts':2,'gets':2,'voice':'OFFLINE_FIXTURE'}] and proof['applyCalls'] == 1 and
                     not proof['external'] and not proof['exceptions'], 'Incomplete controlled browser scenario')
    return proof


def browser_gate(workspace, gate_path):
    recovery.require(gate_path is not None, 'Run the fresh PC browser gate first; Chromium on Firefly is forbidden')
    gate = json.loads(Path(gate_path).read_text())
    verify_browser_gate({'browser_gate':gate})
    return gate


def run_trial(root):
    raise RuntimeError('Browser trial must run on the PC through m19-pc-browser.py; no DISPLAY/Chromium on Firefly')


def run(argv, timeout=30):
    p = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f'{argv}: {p.stderr or p.stdout}')
    return p.stdout.strip()


def rewrite(value, substitutions):
    if isinstance(value, str):
        for old, new in substitutions: value = value.replace(old, new)
        return value
    if isinstance(value, list): return [rewrite(v, substitutions) for v in value]
    if isinstance(value, dict): return {rewrite(k, substitutions): rewrite(v, substitutions) for k, v in value.items()}
    return value


def prepare(gate_path=None, voice_mode='synthetic-acoustic'):
    recovery.require(os.geteuid() == 0, 'Run with sudo; only preparation is safe before START')
    recovery.require(gate_path is not None, 'PC browser gate required before preparation')
    verify_browser_gate({'browser_gate':json.loads(Path(gate_path).read_text())})
    voice_check = voice_preflight(voice_mode)
    template = json.loads((TEMPLATE / 'supervisor/plan.json').read_text())
    identity = json.loads((TEMPLATE / 'candidate-identity.json').read_text())
    recovery.require(source_tree_fingerprint(REPO) == identity['source_sha256'], 'Candidate source changed')
    for path, digest in template['candidate_files'].items():
        recovery.require(recovery.rollout.sha(path) == digest, 'Candidate template changed: ' + path)
    root = Path(tempfile.mkdtemp(prefix='firefly-m19-window-', dir='/var/lib'))
    workspace = REPO / 'build' / root.name
    workspace.mkdir(mode=0o700)
    base = root.name
    old_root = str(Path(template['backup_store']).parent)
    old_base = template['test_unit'].removesuffix('-test.service')
    substitutions = [(str(TEMPLATE), str(workspace)), (old_root, str(root)), (old_base, base)]
    plan = rewrite(copy.deepcopy(template), substitutions)
    user = pwd.getpwnam('firefly')
    for directory in ('candidate/bin', 'store', 'evidence'):
        (workspace / directory).mkdir(parents=True, mode=0o700)
    for name in ('candidate/bin/firefly-demo', 'candidate/mock-http-device.py', 'empty-home.json', 'store/configuration.json'):
        shutil.copy2(TEMPLATE / name, workspace / name)
    release = Path('/opt/firefly/voice-assistant/releases') / plan['old']
    for name in ('share', 'lib'): (workspace / 'candidate' / name).symlink_to(release / name)
    for path in [workspace, *workspace.rglob('*')]: os.chown(path, user.pw_uid, user.pw_gid, follow_symlinks=False)
    recovery.require(shutil.disk_usage(REPO).free >= 150 * 1024 * 1024, 'Insufficient evidence disk space')
    plan['browser_gate'] = browser_gate(workspace, gate_path)
    for directory in ('scripts', 'packaging', 'tests', 'units', 'evidence'):
        (root / directory).mkdir(mode=0o700)
    for name in ('scripts/m19-hardware-recovery.py', 'scripts/production-rollout.py',
                 'scripts/m19-hardware-start.py', 'scripts/m19-pc-bridge.py', 'packaging/__init__.py', 'packaging/firefly_recovery.py',
                 'tests/M19HardwareRecoveryTests.py'):
        shutil.copyfile(REPO / name, root / name)
        (root / name).chmod(0o600)
    for path in (TEMPLATE / 'supervisor/units').iterdir():
        recovery.require(recovery.rollout.sha(path) == template['units'][path.name], 'Unit template changed')
        (root / 'units' / rewrite(path.name, substitutions)).write_text(rewrite(path.read_text(), substitutions))
    plan['units'] = {p.name: recovery.rollout.sha(p) for p in (root / 'units').iterdir()}
    plan['supervisor_files'] = {str(p.relative_to(root)): recovery.rollout.sha(p)
        for directory in ('scripts', 'packaging', 'tests', 'units') for p in (root / directory).iterdir()}
    plan['candidate_files'] = {path: recovery.rollout.sha(path) for path in plan['candidate_files']}
    plan['boot_id'] = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    pid = int(run(['systemctl', 'show', 'firefly-voice-assistant.service', '-p', 'MainPID', '--value']))
    with urllib.request.urlopen('http://127.0.0.1:8080/api/status', timeout=3) as response: status = json.load(response)
    before = {'status': status, 'pid': pid, 'argv': Path(f'/proc/{pid}/cmdline').read_bytes().decode().split('\0'),
        'unit_sha256': recovery.rollout.sha('/etc/systemd/system/firefly-voice-assistant.service'),
        'store_manifest': recovery.rollout.manifest(Path('/var/lib/firefly/store'))}
    (root / 'production-before.json').write_text(json.dumps(before, indent=2))
    run(['cp', '-a', '/var/lib/firefly/store', root / 'store-backup'])
    mock_stdout = workspace / 'evidence/mock-stdout.log'
    mock_command = ['/usr/bin/python3', '-B', str(workspace / 'candidate/mock-http-device.py'),
                    '--port', '0', '--request-log', plan['mock_log']]
    # Started now, never while waiting for the owner. 30 minutes > 10 + 6 + margin.
    run(['systemd-run', '--unit=' + plan['mock_unit'], '--property=User=firefly', '--property=Group=firefly',
         '--property=Restart=no', '--property=RuntimeMaxSec=1800', '--property=KillMode=control-group',
         '--property=StandardOutput=append:' + str(mock_stdout), '--property=StandardError=append:' + str(mock_stdout),
         *mock_command])
    try:
        until = time.monotonic() + 10
        while time.monotonic() < until:
            text = mock_stdout.read_text() if mock_stdout.exists() else ''
            match = re.search(r'^MOCK_LISTEN (http://127\.0\.0\.1:([0-9]+)/relay/1)$', text, re.M)
            if match: break
            time.sleep(.1)
        else: raise RuntimeError('Fresh mock did not become ready')
        plan['mock_url'] = match[1].removesuffix('/relay/1')
        mock_pid = int(run(['systemctl', 'show', plan['mock_unit'], '-p', 'MainPID', '--value']))
        plan['mock_argv'] = mock_command + ['']
        probe = recovery.Window.__new__(recovery.Window)
        probe.run = run
        properties = probe.systemd_properties(plan['mock_unit'], 'Service')
        value = recovery.rollout.property_value
        plan['mock_identity'] = {'pid': mock_pid,
            'start_ticks': Path(f'/proc/{mock_pid}/stat').read_text().rsplit(')', 1)[1].split()[19],
            'started_us': value(properties, 'ExecMainStartTimestampMonotonic', 't'),
            'runtime_us': value(properties, 'RuntimeMaxUSec', 't')}
        plan.pop('browser_unit', None)
        plan['pc_lease_token'] = secrets.token_hex(32)
        plan['pc_heartbeat_timeout_seconds'] = 15
        plan['pc_browser'] = verify_browser_gate(plan)
        plan['voice_mode'] = voice_mode
        plan['voice_preflight'] = voice_check
        plan['audio_unit'] = base + '-audio.service'
        plan['audio_files'] = {}
        verify_voice_plan(plan)
        unit_text = ('[Unit]\nDescription=M19 bounded Firefly audio task (PC browser)\n'
            'PartOf=' + plan['test_unit'] + '\nRequisite=' + plan['test_unit'] + '\nAfter=' + plan['test_unit'] + '\n'
            'ConditionPathExists=!' + str(root / 'CLOSED') + '\nOnFailure=' + plan['recovery_unit'] + '\n'
            '[Service]\nType=exec\nRestart=no\nKillMode=control-group\n'
            'RuntimeMaxSec=20\nTimeoutStopSec=5\nOOMScoreAdjust=500\n'
            'ExecStart=/usr/bin/python3 -B ' + str(root / 'scripts/m19-pc-bridge.py') + ' audio-worker ' + str(root) + '\n'
            'StandardOutput=append:' + str(workspace / 'evidence/audio.log') + '\n'
            'StandardError=append:' + str(workspace / 'evidence/audio.log') + '\n')
        (root / 'units' / plan['audio_unit']).write_text(unit_text)
        plan['units'][plan['audio_unit']] = recovery.rollout.sha(root / 'units' / plan['audio_unit'])
        plan['supervisor_files']['units/' + plan['audio_unit']] = plan['units'][plan['audio_unit']]
        (root / 'plan.json').write_text(json.dumps(plan, indent=2))
        (root / 'plan.json').chmod(0o600)
        run(['systemd-analyze', 'verify', *sorted((root / 'units').iterdir())])
        run(['/usr/bin/python3', '-B', root / 'scripts/m19-hardware-recovery.py', 'check', root], timeout=60)
        result = {'supervisor': str(root), 'workspace': str(workspace), 'mock_url': plan['mock_url'],
                  'mock_unit': plan['mock_unit'], 'prepared_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
        (workspace / 'prepared.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
        return root
    except Exception:
        run(['systemctl', 'stop', plan['mock_unit']])
        raise


def start_window(root):
    plan = json.loads((root / 'plan.json').read_text())
    controller = ['/usr/bin/python3', '-B', root / 'scripts/m19-hardware-recovery.py']
    # Preflight failure occurs before linking, arming, or stopping production.
    verify_browser_gate(plan)
    verify_voice_plan(plan)
    run([*controller, 'check', root], timeout=60)
    run(['systemctl', 'link', *sorted((root / 'units').iterdir())])
    run(['systemctl', 'daemon-reload'])
    try:
        run(['systemctl', 'start', plan['timer_unit']])
        run([*controller, 'arm', root], timeout=60)
        run(['systemctl', 'start', '--no-block', plan['forward_unit']])
    except Exception:
        if (root / 'ARMED.json').exists():
            run(['systemctl', 'start', '--no-block', plan['recovery_unit']])
        else:
            run(['systemctl', 'stop', plan['timer_unit']])
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('prepare-only', 'start'))
    parser.add_argument('--browser-gate', type=Path, required=True)
    parser.add_argument('--voice-mode', choices=('synthetic-acoustic','owner-live'), default='synthetic-acoustic')
    args = parser.parse_args()
    # No option to reuse yesterday's supervisor, mock, store or deadline.
    root = prepare(args.browser_gate, args.voice_mode)
    if args.mode in ('start', 'trial'): start_window(root)



if __name__ == '__main__':
    main()
