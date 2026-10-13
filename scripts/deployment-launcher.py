#!/usr/bin/python3
"""Submit one forward worker asynchronously; observe it without owning recovery."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

spec = importlib.util.spec_from_file_location(
    'rollout_controller', Path(__file__).with_name('production-rollout.py'))
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)


def worker_command(unit, argv):
    return ['systemd-run', '--no-block', '--unit=' + unit,
            '--property=Type=oneshot', '--property=RemainAfterExit=yes',
            '--property=TimeoutStartSec=240s', '--property=TimeoutStopSec=10s',
            '--property=KillMode=control-group', *argv]


def submit_worker(ops, unit, argv):
    command = worker_command(unit, argv)
    try:
        ops.run(command, timeout=15)  # Submission only, never operation completion.
        result = {'state': 'SUBMITTED', 'argv': command}
    except (subprocess.TimeoutExpired, controller.RecoveryTimeout, RuntimeError) as error:
        # The job may exist after the client dies. Do not retry or stop either unit.
        result = {'state': 'SUBMISSION_UNCERTAIN', 'argv': command, 'error': repr(error)}
    ops.record('forward-submission', result)
    return result


def observe_worker(ops, unit, deadline, *, clock=time.monotonic, pause=time.sleep):
    last = None
    terminal = None
    while clock() < deadline:
        try:
            service = ops.systemd_properties(unit, 'Service')
            state = ops.systemd_properties(unit, 'Unit')
            value = controller.property_value
            sample = {'active': value(state, 'ActiveState', 's'),
                      'sub': value(state, 'SubState', 's'),
                      'job': value(state, 'Job', '(uo)'),
                      'pid': value(service, 'MainPID', 'u'),
                      'control_pid': value(service, 'ControlPID', 'u'),
                      'result': value(service, 'Result', 's'),
                      'exit_code': value(service, 'ExecMainCode', 'i'),
                      'exit_status': value(service, 'ExecMainStatus', 'i'),
                      'exited_monotonic_us': value(service, 'ExecMainExitTimestampMonotonic', 't')}
            if sample != last:
                ops.record('forward-observation', sample)
                last = sample
            if clock() >= deadline:
                break
            finished = (sample['job'] == [0, '/'] and sample['pid'] == 0
                        and sample['control_pid'] == 0 and sample['exit_code'] in (1, 2, 3)
                        and sample['exited_monotonic_us'] > 0)
            outcome = None
            if (finished and sample['active'] == 'failed' and sample['sub'] == 'failed'
                    and sample['result'] not in ('', 'success')):
                outcome = 'FORWARD_FAILED'
            elif (finished and sample['active'] == 'active' and sample['sub'] == 'exited'
                    and sample['result'] == 'success' and sample['exit_code'] == 1
                    and sample['exit_status'] == 0):
                outcome = 'WORKER_SUCCEEDED'
            # Service and Unit are separate reads. Require complete, identical
            # terminal evidence on two consecutive polls within the original deadline.
            if outcome and terminal == (outcome, sample):
                return {'state': outcome, 'unit': unit, 'observation': sample}
            terminal = (outcome, sample) if outcome else None
        except Exception as error:
            terminal = None
            sample = {'error': repr(error)}
            if sample != last:
                ops.record('forward-observation', sample)
                last = sample
        pause(min(.25, max(0, deadline - clock())))
    return {'state': 'OBSERVATION_DEADLINE', 'unit': unit, 'observation': last}


def verify_launcher(ops):
    ops.supervisor_files()
    launcher = ops.root / 'scripts/deployment-launcher.py'
    info = launcher.lstat()
    if (launcher.resolve() != Path(__file__).resolve() or info.st_uid != 0
            or not launcher.is_file() or launcher.is_symlink() or info.st_mode & 0o022
            or controller.sha(launcher) != ops.plan['launcher_sha256']):
        raise RuntimeError('Launcher must be the reviewed root-private pinned copy')


def start(ops):
    verify_launcher(ops)
    if (ops.root / 'CUTOVER_STARTED').exists():
        raise RuntimeError('Cutover already started; no resubmission')
    ops.gates(ops.plan['old'])
    ops.supervision_gate()  # Existing guard unchanged; worker checks it again.
    claim = {'plan_sha256': ops.plan_sha256, 'unit': ops.plan['worker_unit'],
             'utc': controller.utc()}
    # Keep the claim even on an uncertain submission. A second start is forbidden.
    with (ops.root / 'FORWARD_LAUNCH_STARTED').open('x') as file:
        file.write(json.dumps(claim) + '\n')
    return submit_worker(ops, ops.plan['worker_unit'],
                         ['/usr/bin/python3', str(ops.root / 'scripts/production-rollout.py'),
                          'deploy', str(ops.root)])


def observe(ops):
    verify_launcher(ops)
    claim = json.loads((ops.root / 'FORWARD_LAUNCH_STARTED').read_text())
    if claim['plan_sha256'] != ops.plan_sha256 or claim['unit'] != ops.plan['worker_unit']:
        raise RuntimeError('Observation must use the original submitted plan')
    result = observe_worker(ops, ops.plan['worker_unit'], ops.plan['deadline_monotonic'])
    if result['state'] == 'WORKER_SUCCEEDED':
        try:
            assert (ops.root / 'ACCEPTED').is_file()
            evidence = json.loads((ops.root / 'evidence/deployed-verified.json').read_text())
            assert evidence['result'] == 'DEPLOYED_VERIFIED'
            result['verification'] = ops.gates(ops.plan['new'])
            result['state'] = 'DEPLOYED_VERIFIED'
        except Exception as error:
            result.update(state='UNVERIFIED_RESULT', error=repr(error))
    # The independently armed timer/controller own rollback. Never disarm them here.
    ops.record('forward-result', result)
    return result


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['start', 'observe', 'run'])
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    ops = controller.Production(args.root)
    if args.mode in ('start', 'run'):
        result = start(ops)
    if args.mode in ('observe', 'run'):
        result = observe(ops)
    print(json.dumps({'state': result['state'], 'watchdog_preserved': True}))
    return 0 if result['state'] in ('SUBMITTED', 'DEPLOYED_VERIFIED') else 1


if __name__ == '__main__':
    raise SystemExit(main())
