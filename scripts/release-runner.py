#!/usr/bin/python3
"""Prepare a fresh recovery bundle; run one separately owner-authorized release."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def command(root, argv, timeout=60):
    """Preserve the response before judging a mandatory preparation command."""
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                                env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1',
                                     'FIREFLY_OWNER_ACCEPTED': '1'})
        record = dict(argv=argv, returncode=result.returncode,
                      stdout=result.stdout, stderr=result.stderr)
    except subprocess.TimeoutExpired as error:
        record = dict(argv=argv, timeout=timeout, stdout=repr(error.stdout), stderr=repr(error.stderr))
        with (root / 'preparation-commands.jsonl').open('a') as file:
            file.write(json.dumps(record) + '\n')
        raise
    with (root / 'preparation-commands.jsonl').open('a') as file:
        file.write(json.dumps(record) + '\n')
    if result.returncode:
        raise RuntimeError('Preparation command failed: ' + repr(argv))
    return result.stdout.strip()


def write(path, value):
    partial = path.with_name(path.name + '.partial')
    with partial.open('x') as file:
        file.write(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        file.flush()
        os.fsync(file.fileno())
    partial.chmod(0o600)
    partial.replace(path)


def prepare_files(root, source):
    """Copy only tracked operator tools/tests; never reuse a consumed directory."""
    root.mkdir(mode=0o700)
    (root / 'evidence').mkdir(mode=0o700)
    files = ['scripts/release-runner.py', 'scripts/deployment-launcher.py',
             'scripts/production-rollout.py', 'packaging/__init__.py',
             'packaging/firefly_recovery.py', 'tests/DeploymentLauncherTests.py',
             'tests/ReleaseRecoveryTests.py', 'tests/ReleaseRunnerTests.py',
             'tests/DeploymentLauncherSystemdTests.py']
    for name in files:
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        shutil.copyfile(source / name, target)
        target.chmod(0o600)
    for name in ['scripts/stage-release.py', 'packaging/__init__.py', 'packaging/firefly_release.py']:
        target = root / 'tools' / name
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        shutil.copyfile(source / name, target)
        target.chmod(0o600)


def mandatory_fixtures(root):
    # Fixed, reviewed list. No ephemeral NRestarts assertion or extra live gates.
    for name in ['DeploymentLauncherTests', 'ReleaseRecoveryTests', 'ReleaseRunnerTests']:
        command(root, ['/usr/bin/python3', str(root / 'tests' / (name + '.py'))])
    command(root, ['/usr/bin/python3', str(root / 'tests/DeploymentLauncherSystemdTests.py'),
                   str(root / 'mandatory-systemd-fixtures')])


def application_checks(root, source):
    # Existing full offline application checks run as the repository/service user,
    # never as privileged production mutation tools. No dependency downloads.
    command(root, ['runuser', '-u', 'firefly', '--', 'ctest', '--test-dir', str(source / 'build'),
                   '--output-on-failure', '--no-tests=error', '-j2'], timeout=1200)
    for name in ['typecheck', 'test', 'build']:
        command(root, ['runuser', '-u', 'firefly', '--', 'npm', '--prefix',
                       str(source / 'src/admin/frontend'), 'run', name], timeout=120)


def prepare(root, source, commit, release):
    # Build, full application tests and immutable staging/install use the existing
    # packaging runbook before this non-disruptive preparation entry point.
    if (root.parent != Path('/run') or root.exists() or source.resolve() != source
            or subprocess.check_output(['runuser', '-u', 'firefly', '--', 'git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() != commit
            or subprocess.check_output(['runuser', '-u', 'firefly', '--', 'git', '-C', str(source), 'status', '--porcelain']).strip()):
        raise RuntimeError('Fresh /run directory and exact clean approved source are required')
    reviewed = subprocess.check_output(['runuser', '-u', 'firefly', '--', 'git', '-C', str(source), 'show', commit + ':scripts/release-runner.py'])
    if Path(__file__).read_bytes() != reviewed:
        raise RuntimeError('Runner is not the tracked approved version')
    prepare_files(root, source)
    application_checks(root, source)
    m = load(root / 'scripts/production-rollout.py')
    package = load(root / 'tools/packaging/firefly_release.py')
    prefix, store = Path('/opt/firefly/voice-assistant'), Path('/var/lib/firefly/store')
    old = Path(os.readlink(prefix / 'current')).name
    for target in [old, release]:
        command(root, ['/usr/bin/python3', str(root / 'tools/scripts/stage-release.py'),
                       'verify', '--destination', str(prefix), '--release-id', target], timeout=600)
    identities = {target: package.build_identity(prefix / 'releases' / target / 'bin/firefly-demo')
                  for target in [old, release]}
    if (old == release or identities[release]['head'] != commit or identities[release]['dirty']
            or identities[release]['source_sha256'] != package.source_tree_fingerprint(source)
            or identities[release]['admin_assets_sha256'] != identities[old]['admin_assets_sha256']):
        raise RuntimeError('Candidate identity/source/assets mismatch')
    documents = {target: json.loads((prefix / 'releases' / target / 'manifest.json').read_bytes())
                 for target in [old, release]}
    def preserved(document):
        return {a['path']: a for a in document['artifacts']
                if a['path'].startswith(('lib/', 'share/firefly/wake/', 'share/firefly/config/', 'share/firefly/systemd/'))}
    if (preserved(documents[old]) != preserved(documents[release])
            or documents[old]['shared_models'] != documents[release]['shared_models']):
        raise RuntimeError('Voice/libraries/unit/seed/shared-model contract changed')
    command(root, ['cp', '-a', str(store), str(root / 'backup-store')])
    with urllib.request.urlopen('http://127.0.0.1:8080/api/status', timeout=3) as response:
        status = json.load(response)
    before = dict(status=status, argv=Path('/proc/' + str(status['worker_pid']) + '/cmdline').read_bytes().decode().split('\0'),
                  unit_sha256=m.sha('/etc/systemd/system/firefly-voice-assistant.service'), store_manifest=m.manifest(store))
    write(root / 'production-before.json', before)
    now = time.monotonic()
    plan = dict(old=old, new=release, backup_store=str(root / 'backup-store'), identities=identities,
                binary_sha256={target: m.sha(prefix / 'releases' / target / 'bin/firefly-demo') for target in [old, release]},
                worker_unit=root.name + '-forward.service', rollback_unit=root.name + '-rollback.service',
                rollback_timer=root.name + '-rollback.timer', verification_budget_seconds=240, recovery_budget_seconds=360,
                boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                armed_monotonic=now, deadline_monotonic=now + 240,
                controller_sha256=m.sha(root / 'scripts/production-rollout.py'),
                recovery_helper_sha256=m.sha(root / 'packaging/firefly_recovery.py'),
                launcher_sha256=m.sha(root / 'scripts/deployment-launcher.py'),
                runner_sha256=m.sha(root / 'scripts/release-runner.py'))
    # This provisional, unarmed plan cannot pass supervision_gate. The one real
    # boot-bound deadline is frozen from the actual timer before any forward launch.
    write(root / 'plan.json', plan)
    mandatory_fixtures(root)
    ops = m.Production(root)
    ops.operation_deadline = time.monotonic() + 30  # Read-only preparation, timer not armed.
    ops.record('prepared-gates', ops.gates(old))
    write(root / 'PREPARED', {'plan_sha256': m.sha(root / 'plan.json'), 'commit': commit})


def diagnostic(ops):
    # Diagnostic failures are recorded, not treated as extra authorization gates.
    try:
        value = {'output': ops.run(['systemctl', 'show',
            ops.plan['worker_unit'], ops.plan['rollback_unit'], ops.plan['rollback_timer']], timeout=5)}
    except Exception as error:
        value = {'error': repr(error)}
    try:
        ops.record('unit-diagnostics', value)
    except Exception as error:
        value['recording_error'] = repr(error)
    return value


def arm(ops, authorization):
    root = ops.root
    m = load(root / 'scripts/production-rollout.py')
    command(root, ['systemd-run', '--unit=' + ops.plan['rollback_unit'], '--on-active=240s',
        '--timer-property=AccuracySec=250ms', '--property=Type=exec', '--property=Restart=on-failure',
        '--property=RestartSec=1s', '--property=RestartPreventExitStatus=78', '--property=StartLimitIntervalSec=0',
        '--property=TimeoutStartSec=10s', '--property=TimeoutStopSec=10s', '--property=RuntimeMaxSec=360s',
        '/usr/bin/python3', str(root / 'scripts/production-rollout.py'), 'rollback', str(root)], timeout=15)
    trigger = m.property_value(ops.systemd_properties(ops.plan['rollback_timer'], 'Timer'), 'NextElapseUSecMonotonic', 't')
    plan = {**ops.plan, 'armed_monotonic': (trigger - 240000000) / 1000000,
            'deadline_monotonic': trigger / 1000000, 'authorization': authorization}
    write(root / 'plan.json', plan)
    ops = m.Production(root)
    write(root / 'PLAN_FROZEN', {'plan_sha256': ops.plan_sha256, 'utc': m.utc()})
    return ops


def run(root, authorization):
    launcher = load(root / 'scripts/deployment-launcher.py')
    m = launcher.controller
    ops = m.Production(root)
    ops.supervisor_files()
    if (Path(__file__).resolve() != root / 'scripts/release-runner.py'
            or m.sha(Path(__file__)) != ops.plan['runner_sha256']
            or json.loads((root / 'PREPARED').read_bytes())['plan_sha256'] != ops.plan_sha256):
        raise RuntimeError('Runner/prepared bundle changed')
    with (root / 'RUNNER_STARTED').open('x') as claim:
        claim.write(m.utc() + '\n')
    plan = ops.plan
    result = None
    try:
        ops.operation_deadline = time.monotonic() + 30  # Only old read-only gates before arm.
        ops.record('runner-before-arm', ops.gates(plan['old']))
        ops = arm(ops, authorization)
        plan = ops.plan
        ops.record('runner-preflight', ops.gates(plan['old']))
        ops.supervision_gate()
        launcher.start(ops)  # The existing exclusive claim/asynchronous launcher.
        result = launcher.observe(ops)
        if result['state'] == 'DEPLOYED_VERIFIED':
            independent = m.Production(root).gates(plan['new'])
            ops.record('runner-independent-acceptance', independent)
            result = {'result': 'DEPLOYED_VERIFIED', 'verification': independent}
        elif result['state'] == 'FORWARD_FAILED' and not (root / 'CUTOVER_STARTED').exists():
            result = {'result': 'ABORTED_BEFORE_CUTOVER', 'verification': ops.gates(plan['old'])}
        else:
            raise RuntimeError('Forward operation did not independently verify: ' + repr(result))
    except Exception as error:
        ops.record('runner-error', {'error': repr(error)})
        if not (root / 'FORWARD_LAUNCH_STARTED').exists():
            try:
                ops.operation_deadline = time.monotonic() + 30  # No forward submitted; read-only old verification.
                result = {'result': 'ABORTED_BEFORE_CUTOVER', 'verification': ops.gates(plan['old'])}
            except Exception as verification_error:
                result = {'result': 'BLOCKED_UNKNOWN', 'error': repr(verification_error), 'watchdog_preserved': True}
        else:
            # Do not stop/disarm recovery or start another worker. The original
            # independent timer/controller owns restoration, including agent loss.
            ops.operation_deadline = ops.recovery_deadline
            while time.monotonic() < ops.recovery_deadline:
                try:
                    if (root / 'evidence/rollback-result.json').exists():
                        result = {'result': 'ROLLED_BACK', 'verification': ops.gates(plan['old'])}
                        break
                except Exception as recovery_error:
                    ops.record('runner-recovery-observation', {'error': repr(recovery_error)})
                time.sleep(min(.5, max(0, ops.recovery_deadline - time.monotonic())))
            if result is None or 'result' not in result:
                result = {'result': 'BLOCKED_UNKNOWN', 'watchdog_preserved': True}
    result['diagnostics'] = diagnostic(ops)
    if result['result'] in ('DEPLOYED_VERIFIED', 'ROLLED_BACK', 'ABORTED_BEFORE_CUTOVER'):
        # Disarm only after fresh existing runtime/store/ownership gates passed.
        try:
            ops.run(['systemctl', 'stop', plan['rollback_timer']], timeout=10)
            result['watchdog_disarmed_after_verification'] = True
        except Exception as error:
            result['watchdog_disarmed_after_verification'] = False
            result['watchdog_error'] = repr(error)
    ops.record('runner-result', result)
    return result


def main():
    os.umask(0o077)
    if os.geteuid() != 0 or not __debug__:
        raise RuntimeError('Root and Python assertions are required')
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('--source', type=Path, required=True)
    prep.add_argument('--approved-commit', required=True)
    prep.add_argument('--new-release', required=True)
    prep.add_argument('--root', type=Path, required=True)
    execute = sub.add_parser('run')
    execute.add_argument('root', type=Path)
    execute.add_argument('--owner-authorization', required=True)
    args = parser.parse_args()
    if args.mode == 'prepare':
        try:
            prepare(args.root, args.source, args.approved_commit, args.new_release)
        except Exception as error:
            print(json.dumps({'result': 'ABORTED_BEFORE_CUTOVER', 'phase': 'prepare', 'error': repr(error)}))
            return 1
        print(json.dumps({'result': 'PREPARED', 'production_stopped': False}))
        return 0
    try:
        result = run(args.root, args.owner_authorization)
    except Exception as error:
        # No retry, ad-hoc recovery or watchdog disarm after an unhandled failure.
        uncertain = (args.root / 'FORWARD_LAUNCH_STARTED').exists() or (args.root / 'CUTOVER_STARTED').exists()
        result = {'result': 'BLOCKED_UNKNOWN' if uncertain else 'ABORTED_BEFORE_CUTOVER',
                  'error': repr(error), 'watchdog_preserved': True}
    print(json.dumps({'result': result['result']}))
    return 0 if result['result'] == 'DEPLOYED_VERIFIED' else 1


if __name__ == '__main__':
    raise SystemExit(main())
