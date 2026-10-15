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


def verify_stress_gate(reference, transport, remote_root):
    receipt = Path(reference['path'])
    recovery.require(receipt.name == 'admin-stress-gate.json' and receipt.parent.parent == Path('/var/lib') and
                     receipt.parent.name.startswith('firefly-m19-pc-offline-') and receipt.resolve() == receipt,
                     'Unknown private stress receipt')
    recovery.require(receipt.parent == Path(remote_root), 'Stress/browser backend mismatch')
    for path in (receipt.parent, receipt):
        info = path.stat()
        recovery.require(info.st_uid == 0 and not info.st_mode & 0o077, 'Stress receipt must be root-private')
    recovery.require(recovery.rollout.sha(receipt) == reference['sha256'], 'Stress receipt changed')
    gate = json.loads(receipt.read_text())
    recovery.require(gate['offline_root'] == remote_root and
                     0 <= time.monotonic() - gate['checked_monotonic'] < 300,
                     'Stress preflight is stale or belongs to another backend')
    proof_path = Path(gate['proof_path'])
    recovery.require(proof_path == receipt.parent / 'admin-stress-offline.json' and proof_path.resolve() == proof_path and
                     proof_path.stat().st_uid == 0 and not proof_path.stat().st_mode & 0o077 and
                     recovery.rollout.sha(proof_path) == gate['proof_sha256'], 'Stress proof changed')
    client_sha = recovery.rollout.sha(REPO / 'benchmarks/web-admin-b4/DialogueSmoke.py')
    recovery.require(gate['boot_id'] == Path('/proc/sys/kernel/random/boot_id').read_text().strip() and
                     gate['client_sha256'] == client_sha and transport.get('stress_client_sha256') == client_sha and
                     transport.get('admin_stress') is True, 'Unchecked stress client/profile')
    proof = json.loads(proof_path.read_text())
    diagnostic = transport.get('diagnostic_only', False)
    repeatability = transport.get('repeatability_only', False)
    recovery.require(type(diagnostic) is bool and type(repeatability) is bool and not (diagnostic and repeatability),
                     'Invalid stress mode')
    recovery.require(gate.get('diagnostic_only') is diagnostic and proof.get('diagnostic_only') is diagnostic and
                     gate.get('repeatability_only', False) is repeatability and proof.get('repeatability_only', False) is repeatability and
                     proof.get('passed') is True and proof.get('real_rkllm') is False and
                     proof.get('profile') == ('ADMIN4096_DIAGNOSTIC' if diagnostic else 'ADMIN4096_BOUNDED_STRESS') and
                     proof.get('apply_calls') == 0 and proof.get('device_requests') == 0 and len(proof['cycles']) == (1 if diagnostic else 2),
                     'Incomplete or mislabelled offline stress proof')
    if repeatability:
        recovery.require([(x['case'], x['bytes']) for x in gate['workload']] == [('long', 1606)] and
                         transport.get('diagnostic_candidate') is not None, 'Repeatability requires the pinned long-only workload')
    import hashlib
    recovery.require(hashlib.sha256(json.dumps(gate['workload'], sort_keys=True, ensure_ascii=False).encode()).hexdigest() ==
                     gate['workload_sha256'], 'Stress workload changed')
    for cycle in proof['cycles']:
        recovery.require(cycle['restored'] is True and [(x['case'], x['input_bytes']) for x in cycle['cases']] ==
                         [(x['case'], x['bytes']) for x in gate['workload']], 'Offline cycles/workload mismatch')
    return gate


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
    if plan.get('profile') == 'ADMIN4096_BOUNDED_STRESS' or proof.get('admin_stress_gate'):
        recovery.require(proof.get('admin_stress_gate') is not None, 'Offline stress gate required')
        verify_stress_gate(proof['admin_stress_gate'], proof['transport'], proof['remoteRoot'])
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


def verify_diagnostic_candidate(reference):
    path = Path(reference['path'])
    recovery.require(path.parent == Path('/var/lib') and path.name.startswith('firefly-m19-diag-candidate-') and
                     path.suffix == '.json' and path.resolve() == path and path.stat().st_uid == 0 and
                     not path.stat().st_mode & 0o077 and recovery.rollout.sha(path) == reference['sha256'], 'Untrusted diagnostic descriptor')
    data = json.loads(path.read_text())
    recovery.require(REPO == Path('/home/firefly/FireflyVoiceAssistantLinux-m19-brightness-route') and data['source_root'] == str(REPO),
                     'Wrong diagnostic source')
    recovery.require(data['identity']['head'] == 'c7fa14906f8ae15f4c8fb488e9c9d7628eb8154f' and not data['identity']['dirty'] and
                     data['identity']['source_sha256'] == source_tree_fingerprint(REPO), 'Diagnostic source/build mismatch')
    reviewed = {
        'src/admin/RuntimeGeneration.cpp': '459dface26ff1f807cbf9f8728dabb926dbd592adaed969ec8e864d1c97e6d05',
        'src/admin/RuntimeWorker.cpp': 'fdb6960b8dcfb1560c8d5ea17705cf24142d7b24c8bf0f03f71e0d10538d6729',
        'src/admin/SetupSession.h': '5a7d7439d434529c89458b495aa02e9ee94a1eeaa58c9dd589db7848c3285b4a',
    }
    recovery.require(all(recovery.rollout.sha(REPO / name) == digest for name,digest in reviewed.items()),
                     'Unreviewed diagnostic product change')
    recovery.require(run(['git','-C',REPO,'diff','--name-only','320b80a6d5a73d9638e4bc386129248ba3d65b8e','--',
                         'src','include','config','runtime','third_party','CMakeLists.txt']) == '\n'.join(reviewed),
                     'Unexpected diagnostic product diff')
    binary = Path(data['binary'])
    recovery.require(binary == REPO / 'build/diagnostic-candidate/bin/firefly-demo' and binary.resolve() == binary and
                     recovery.rollout.sha(binary) == data['binary_sha256'] and
                     json.loads(run([binary,'--build-identity'])) == data['identity'], 'Diagnostic binary changed')
    return data


def prepare(gate_path=None, voice_mode='synthetic-acoustic', installed_candidate=False, diagnostic_candidate=None, repeatability_only=False):
    recovery.require(os.geteuid() == 0, 'Run with sudo; only preparation is safe before START')
    recovery.require(gate_path is not None, 'PC browser gate required before preparation')
    verify_browser_gate({'browser_gate':json.loads(Path(gate_path).read_text()),
                         'profile':'ADMIN4096_BOUNDED_STRESS' if installed_candidate else 'voice'})
    diagnostic = verify_diagnostic_candidate(diagnostic_candidate) if diagnostic_candidate else None
    recovery.require(type(repeatability_only) is bool and (not repeatability_only or diagnostic is not None),
                     'Repeatability requires a pinned candidate')
    recovery.require(not diagnostic or installed_candidate, 'Diagnostic candidate requires isolated Admin profile')
    voice_check = voice_preflight(voice_mode)
    # The stress profile exercises the already installed immutable ELF, never
    # the historical dirty M19 candidate. Recovery/barriers/unit templates stay
    # the same as the existing single-window harness.
    template_root = (Path('/home/firefly/FireflyVoiceAssistantLinux-m19/build/m19-resource-hw-20261013T172025Z')
                     if installed_candidate else TEMPLATE)
    template = json.loads((template_root / 'supervisor/plan.json').read_text())
    installed = Path('/opt/firefly/voice-assistant/current').resolve()
    identity = (json.loads(run([installed / 'bin/firefly-demo', '--build-identity'])) if installed_candidate
                else json.loads((TEMPLATE / 'candidate-identity.json').read_text()))
    if installed_candidate:
        recovery.require(installed.name == 'm19-320b80a6-20261014T173500Z' and
                         identity['head'] == '320b80a6d5a73d9638e4bc386129248ba3d65b8e' and not identity['dirty'],
                         'Stress requires the exact accepted installed M19 release')
        recovery.require(recovery.rollout.sha(installed / 'bin/firefly-demo') ==
                         '80175e29dc5cc96a3a4315369c45f530130c0581bec28e097a359e6110aa4af0', 'Installed ELF changed')
    recovery.require(source_tree_fingerprint(REPO) == (diagnostic or {'identity':identity})['identity']['source_sha256'], 'Candidate source changed')
    for path, digest in template['candidate_files'].items():
        recovery.require(recovery.rollout.sha(path) == digest, 'Candidate template changed: ' + path)
    root = Path(tempfile.mkdtemp(prefix='firefly-m19-window-', dir='/var/lib'))
    workspace = REPO / 'build' / root.name
    workspace.mkdir(mode=0o700)
    base = root.name
    old_root = str(Path(template['backup_store']).parent)
    old_base = template['test_unit'].removesuffix('-test.service')
    substitutions = [(str(template_root), str(workspace)), (old_root, str(root)), (old_base, base)]
    if installed_candidate:
        substitutions.append(('/opt/firefly/voice-assistant/releases/' + template['old'], str(installed)))
    plan = rewrite(copy.deepcopy(template), substitutions)
    if installed_candidate:
        recovery.require(recovery.rollout.sha(installed / 'manifest.json') ==
                         '6a474d57d69ca039821d80dced83f29691d24813f058ef7c3e3308fd4ac01291', 'Installed manifest changed')
        installed_manifest = json.loads((installed / 'manifest.json').read_text())
        approved = {str(installed / item['path']): item['sha256'] for item in installed_manifest['artifacts']}
        approved[str(installed / 'manifest.json')] = recovery.rollout.sha(installed / 'manifest.json')
        approved[str(installed / 'manifest.sha256')] = recovery.rollout.sha(installed / 'manifest.sha256')
        plan['old'] = plan['new'] = installed.name
        plan['binary_sha256'] = {installed.name: recovery.rollout.sha(installed / 'bin/firefly-demo')}
        plan['identities'] = {installed.name: identity}
        plan['profile'] = 'ADMIN4096_BOUNDED_STRESS'
        plan['repeatability_only'] = repeatability_only
        if diagnostic:
            if not repeatability_only: plan['profile'] = 'ADMIN4096_DIAGNOSTIC'
            plan['diagnostic_candidate'] = diagnostic
            plan['diagnostic_reference'] = diagnostic_candidate
        # Metadata contracts refer to the actual installed libraries/assets,
        # including the protected Amina target; no dependency is copied.
        for entry in plan['dependencies']:
            info = Path(entry['path']).stat()
            if entry['path'].startswith(str(installed) + '/'):
                recovery.require(entry['path'] in approved and recovery.rollout.sha(entry['path']) == approved[entry['path']],
                                 'Installed dependency integrity mismatch: ' + entry['path'])
                entry.update(sha256=approved[entry['path']], size=info.st_size, mtime_ns=info.st_mtime_ns, inode=info.st_ino, device=info.st_dev)
            else:
                recovery.require((info.st_size, info.st_mtime_ns, info.st_ino, info.st_dev) ==
                                 (entry['size'], entry['mtime_ns'], entry['inode'], entry['device']),
                                 'Original verified shared/system dependency changed: ' + entry['path'])
    user = pwd.getpwnam('firefly')
    for directory in ('candidate/bin', 'store', 'evidence'):
        (workspace / directory).mkdir(parents=True, mode=0o700)
    for name in ('candidate/bin/firefly-demo', 'candidate/mock-http-device.py', 'empty-home.json', 'store/configuration.json'):
        source = (Path(diagnostic['binary']) if diagnostic else installed / 'bin/firefly-demo') if installed_candidate and name == 'candidate/bin/firefly-demo' else template_root / name
        shutil.copy2(source, workspace / name)
    if installed_candidate:
        (workspace / 'stress-test-marker').write_text('ADMIN4096_BOUNDED_STRESS\n')
    release = Path('/opt/firefly/voice-assistant/releases') / plan['old']
    for name in ('share', 'lib'): (workspace / 'candidate' / name).symlink_to(release / name)
    if diagnostic:
        recovery.require(recovery.rollout.sha(workspace / 'candidate/bin/firefly-demo') == diagnostic['binary_sha256'] and
                         json.loads(run([workspace / 'candidate/bin/firefly-demo','--build-identity'])) == diagnostic['identity'],
                         'Diagnostic copy differs from approved binary')
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
    if installed_candidate:
        shutil.copyfile(REPO / 'benchmarks/web-admin-b4/DialogueSmoke.py', root / 'scripts/admin-stress-client.py')
        (root / 'scripts/admin-stress-client.py').chmod(0o600)
        proof = json.loads(Path(plan['browser_gate']['proof_path']).read_text())
        recovery.require(proof['transport'].get('diagnostic_only',False) is bool(diagnostic and not repeatability_only) and
                         proof['transport'].get('repeatability_only',False) is repeatability_only and
                         (not diagnostic or proof['transport']['diagnostic_candidate'] == diagnostic_candidate), 'Diagnostic transport/candidate mismatch')
        gate = verify_stress_gate(proof['admin_stress_gate'], proof['transport'], proof['remoteRoot'])
        copied_proof = root / 'evidence/admin-stress-offline.json'
        shutil.copyfile(gate['proof_path'], copied_proof); copied_proof.chmod(0o600)
        recovery.require(recovery.rollout.sha(root / 'scripts/admin-stress-client.py') == gate['client_sha256'],
                         'Stress client changed while copying')
        plan['stress_gate'] = {**gate, 'proof_path':str(copied_proof)}
    for path in (template_root / 'supervisor/units').iterdir():
        recovery.require(recovery.rollout.sha(path) == template['units'][path.name], 'Unit template changed')
        (root / 'units' / rewrite(path.name, substitutions)).write_text(rewrite(path.read_text(), substitutions))
    plan['units'] = {p.name: recovery.rollout.sha(p) for p in (root / 'units').iterdir()}
    plan['supervisor_files'] = {str(p.relative_to(root)): recovery.rollout.sha(p)
        for directory in ('scripts', 'packaging', 'tests', 'units') for p in (root / directory).iterdir()}
    plan['candidate_files'] = {path: recovery.rollout.sha(path) for path in plan['candidate_files']}
    if diagnostic:
        plan['candidate_files'][str(workspace / 'candidate/bin/firefly-demo')] = diagnostic['binary_sha256']
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
    parser.add_argument('--installed-candidate', action='store_true', help='Stress only: use the exact accepted installed ELF')
    args = parser.parse_args()
    # No option to reuse yesterday's supervisor, mock, store or deadline.
    root = prepare(args.browser_gate, args.voice_mode, True) if args.installed_candidate else prepare(args.browser_gate, args.voice_mode)
    if args.mode in ('start', 'trial'): start_window(root)



if __name__ == '__main__':
    main()
