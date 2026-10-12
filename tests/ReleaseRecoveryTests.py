#!/usr/bin/env python3
"""Offline recovery faults. No production unit, ConfigStore or device is opened."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from packaging.firefly_recovery import RecoveryTimeout, recover, wait_quiescent

spec = importlib.util.spec_from_file_location('controller', ROOT / 'scripts/production-rollout.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)


def snapshot(owners=(), children=(), main=0, active='inactive', control=0):
    return dict(owners=list(owners), cgroup_pids=list(children), main_pid=main,
                control_pid=control, active_state=active)


class Clock:
    def __init__(self):
        self.now = 0
    def __call__(self):
        return self.now
    def sleep(self, seconds):
        self.now += seconds


class Operations:
    def __init__(self, clock, release='new', delayed=0, hung=False, ready=False):
        self.clock, self.release, self.delayed = clock, release, delayed
        self.hung, self.ready = hung, ready
        self.events, self.records = [], []
        self.start_failures = 0
    def record(self, name, value):
        self.records.append((name, value))
    def current(self):
        return self.release
    def gates(self, target):
        if self.ready and self.release == target:
            return {'ready': True, 'release': target}
        raise RuntimeError('Not ready')
    def stop(self):
        self.events.append('stop')
        self.ready = False
    def resource_snapshot(self):
        busy = self.hung or self.clock() < self.delayed
        return snapshot(owners=[101] if busy else [], children=[102] if busy else [])
    def switch(self, target):
        if self.resource_snapshot()['owners']:
            raise AssertionError('Switch while occupied')
        self.events.append('switch-' + target)
        self.release = target
    def start(self):
        if self.resource_snapshot()['owners'] or self.resource_snapshot()['cgroup_pids']:
            raise AssertionError('Start while occupied')
        self.events.append('start')
        if self.start_failures:
            self.start_failures -= 1
            raise RuntimeError('Transient start failure')
        self.ready = True
    def wait_ready(self, target, seconds):
        return self.gates(target)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
    def restore(self, ops, deadline=100):
        return recover(ops, 'old', deadline, clock=self.clock, pause=self.clock.sleep)
    def test_delayed_release_waits_without_bypass(self):
        ops = Operations(self.clock, delayed=3)
        self.assertTrue(self.restore(ops)['ready'])
        self.assertGreaterEqual(self.clock(), 3.5)
        self.assertEqual(ops.events, ['stop', 'switch-old', 'start'])
        observations = [v for k, v in ops.records if k == 'resource-observation']
        self.assertEqual(observations[0]['owners'], [101])
        self.assertEqual(observations[0]['cgroup_pids'], [102])
    def test_hung_child_blocks_switch_and_start(self):
        ops = Operations(self.clock, hung=True)
        with self.assertRaises(RecoveryTimeout):
            self.restore(ops, deadline=2)
        self.assertEqual(ops.events, ['stop'])
        self.assertEqual(ops.release, 'new')
    def test_foreign_owner_never_killed_or_ignored(self):
        ops = Operations(self.clock)
        ops.resource_snapshot = lambda: snapshot(owners=[999])
        with self.assertRaises(RecoveryTimeout):
            self.restore(ops, deadline=2)
        self.assertEqual(ops.events, ['stop'])
    def test_child_without_device_fd_is_not_quiescent(self):
        ops = Operations(self.clock)
        ops.resource_snapshot = lambda: snapshot(children=[102])
        with self.assertRaises(RecoveryTimeout):
            self.restore(ops, deadline=2)
        self.assertNotIn('start', ops.events)
    def test_transient_control_failure_retries_old_only(self):
        ops = Operations(self.clock)
        ops.start_failures = 1
        self.assertTrue(self.restore(ops)['ready'])
        self.assertEqual(ops.events.count('switch-old'), 1)
        self.assertEqual(ops.events.count('start'), 2)
        self.assertTrue(any(k == 'recovery-failure' for k, v in ops.records))
    def test_repeated_recovery_is_verified_noop(self):
        ops = Operations(self.clock)
        self.restore(ops)
        events = list(ops.events)
        self.restore(ops)
        self.assertEqual(ops.events, events)
    def test_before_pointer_switch_restores_without_activation(self):
        ops = Operations(self.clock, release='old', delayed=1)
        self.restore(ops)
        self.assertEqual(ops.events, ['stop', 'start'])
    def test_expired_deadline_cannot_mutate(self):
        ops = Operations(self.clock)
        with self.assertRaises(RecoveryTimeout):
            self.restore(ops, deadline=0)
        self.assertEqual(ops.events, [])
    def test_new_owner_after_switch_blocks_start(self):
        ops = Operations(self.clock)
        ops.resource_snapshot = lambda: snapshot(owners=[999] if ops.release == 'old' else [])
        with self.assertRaises(RecoveryTimeout):
            self.restore(ops, deadline=2)
        self.assertEqual(ops.events.count('switch-old'), 1)
        self.assertNotIn('start', ops.events)
    def test_false_free_observation_cannot_authorize(self):
        states = iter([snapshot(), snapshot(owners=[88]), snapshot(), snapshot()])
        records = []
        wait_quiescent(lambda: next(states), lambda k, v: records.append(v), 10,
                       clock=self.clock, pause=self.clock.sleep)
        self.assertEqual(len(records), 4)
    def test_probe_errors_fail_closed_with_diagnostics(self):
        def probe():
            raise PermissionError('Cannot inspect owners')
        records = []
        with self.assertRaises(RecoveryTimeout):
            wait_quiescent(probe, lambda k, v: records.append(v), 1,
                           clock=self.clock, pause=self.clock.sleep)
        self.assertIn('PermissionError', records[0]['error'])
    def test_probe_finishing_after_deadline_cannot_authorize(self):
        def probe():
            self.clock.now = 2
            return snapshot()
        with self.assertRaises(RecoveryTimeout):
            wait_quiescent(probe, lambda *args: None, 1,
                           clock=self.clock, pause=self.clock.sleep)
    def test_live_or_control_pid_blocks_barrier(self):
        for state in [snapshot(main=1), snapshot(control=2), snapshot(active='deactivating')]:
            with self.subTest(state=state), self.assertRaises(RecoveryTimeout):
                wait_quiescent(lambda: state, lambda *args: None, self.clock() + 1,
                               clock=self.clock, pause=self.clock.sleep)
    def make_controller(self):
        ops = object.__new__(controller.Production)
        ops.operation_deadline = time.monotonic() + 10
        ops.records = []
        ops.record = lambda k, v: ops.records.append((k, v))
        return ops
    def test_fuser_records_offending_pid_stdout_stderr(self):
        ops = self.make_controller()
        result = subprocess.CompletedProcess([], 0, ' 123 456 ', '/dev/dri/card1: m\n/dev/snd/pcmC3D0c:')
        with patch.object(controller.stat, 'S_ISCHR', return_value=True), \
                patch.object(controller.subprocess, 'run', return_value=result):
            self.assertEqual(ops.owner_probe(), [123, 456])
        self.assertEqual(ops.records[0][1]['stdout'], ' 123 456 ')
    def test_fuser_error_with_exit_one_is_not_free(self):
        ops = self.make_controller()
        for result in [subprocess.CompletedProcess([], 1, '', 'Permission denied'),
                       subprocess.CompletedProcess([], 0, '', ''),
                       subprocess.CompletedProcess([], 1, '123', ''),
                       subprocess.CompletedProcess([], 2, '', '')]:
            with self.subTest(result=result), patch.object(controller.stat, 'S_ISCHR', return_value=True), \
                    patch.object(controller.subprocess, 'run', return_value=result), self.assertRaises(RuntimeError):
                ops.owner_probe()
    def test_fuser_timeout_keeps_partial_output(self):
        ops = self.make_controller()
        with patch.object(controller.stat, 'S_ISCHR', return_value=True), \
                patch.object(controller.subprocess, 'run', side_effect=subprocess.TimeoutExpired('fuser', 5, b'123', b'error')), \
                self.assertRaises(subprocess.TimeoutExpired):
            ops.owner_probe()
        self.assertIn('123', ops.records[0][1]['stdout'])
    def test_missing_resource_fails_closed_before_fuser(self):
        ops = self.make_controller()
        with patch.object(controller.Path, 'stat', side_effect=FileNotFoundError), \
                patch.object(controller.subprocess, 'run') as run, self.assertRaises(FileNotFoundError):
            ops.owner_probe()
        run.assert_not_called()
    def test_rollback_before_stop_marker_is_noop(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = self.make_controller()
            ops.root = Path(tmp)
            ops.run = lambda *a, **kw: self.fail('No control command allowed')
            ops.rollback('fixture')
            self.assertEqual(ops.records[-1][0], 'aborted-before-cutover')
    def test_controller_reentry_restores_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = self.make_controller()
            ops.root = Path(tmp)
            (ops.root / 'CUTOVER_STARTED').write_text('fixture')
            ops.plan = dict(old='old', new='new', worker_unit='fixture-worker.service')
            ops.recovery_deadline = time.monotonic() + 5
            fake = Operations(time.monotonic, release='old')
            commands = []
            def run(args, **kw):
                commands.append(args)
                return 'LoadState=loaded\nActiveState=inactive\nMainPID=0\nControlPID=0\nControlGroup=' if 'show' in args else ''
            ops.run = run
            for name in ['current', 'gates', 'stop', 'start', 'wait_ready', 'switch', 'resource_snapshot']:
                setattr(ops, name, getattr(fake, name))
            ops.rollback('fixture failure before pointer switch')
            ops.rollback('fixture repeated recovery')
            self.assertEqual(fake.events, ['stop', 'start'])
            stops = [c for c in commands if 'stop' in c]
            self.assertEqual(len(stops), 2)  # Only our fixture worker stop.
            self.assertTrue(all(c[-1] == 'fixture-worker.service' for c in stops))
    def test_garbage_collected_worker_does_not_block_repeat_recovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = self.make_controller()
            ops.root = Path(tmp)
            (ops.root / 'CUTOVER_STARTED').write_text('fixture')
            ops.plan = dict(old='old', new='new', worker_unit='fixture-worker.service')
            ops.recovery_deadline = time.monotonic() + 5
            fake = Operations(time.monotonic, release='old', ready=True)
            def run(args, **kw):
                self.assertIn('show', args)
                return 'LoadState=not-found\nActiveState=inactive\nMainPID=0\nControlPID=0\nControlGroup='
            ops.run = run
            ops.current, ops.gates = fake.current, fake.gates
            ops.rollback('fixture repeated recovery after transient unit GC')
            self.assertEqual(fake.events, [])
    def test_expired_controller_cannot_stop_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = self.make_controller()
            ops.root = Path(tmp)
            (ops.root / 'CUTOVER_STARTED').write_text('fixture')
            ops.recovery_deadline = 0
            ops.run = lambda *a, **kw: self.fail('Expired plan must not mutate')
            with self.assertRaises(RecoveryTimeout):
                ops.rollback('fixture')
    def test_expired_http_verification_cannot_issue_request(self):
        ops = self.make_controller()
        ops.operation_deadline = 0
        with patch.object(controller.urllib.request, 'urlopen') as request, self.assertRaises(RecoveryTimeout):
            ops.get('/api/status')
        request.assert_not_called()
    def test_control_command_timeout_keeps_partial_diagnostics(self):
        ops = self.make_controller()
        ops.env = {}
        with patch.object(controller.subprocess, 'run',
                          side_effect=subprocess.TimeoutExpired('fixture-command', 1, b'pid=123', b'timed out')), \
                self.assertRaises(subprocess.TimeoutExpired):
            ops.run(['fixture-command'])
        self.assertEqual(ops.records[-1][0], 'command-timeout')
        self.assertIn('pid=123', ops.records[-1][1]['stdout'])
    def test_real_fuser_and_delayed_fixture_process(self):
        # A real process holds an ordinary temporary file. No NPU/ALSA access.
        with tempfile.TemporaryDirectory() as tmp:
            resource = Path(tmp) / 'resource'
            resource.write_text('fixture')
            child = subprocess.Popen([sys.executable, '-c',
                'import sys,time; f=open(sys.argv[1]); print("opened",flush=True); time.sleep(60)', str(resource)],
                stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(child.stdout.readline().strip(), 'opened')
                def probe():
                    result = subprocess.run(['fuser', str(resource)], capture_output=True, text=True, timeout=5)
                    return snapshot(owners=[int(p) for p in result.stdout.split()])
                observations = []
                self.assertIn(child.pid, probe()['owners'])
                # Offline owner exits in response to TERM; the wait observes exit.
                child.terminate()
                wait_quiescent(probe, lambda k, v: observations.append(v), time.monotonic() + 5)
                child.wait(timeout=2)
                self.assertEqual(observations[-1]['owners'], [])
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
                child.stdout.close()
    def test_independent_fixture_supervisor_crash_and_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([sys.executable, __file__, 'fixture-watchdog', tmp],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            state = json.loads((Path(tmp) / 'state.json').read_text())
            self.assertTrue(state['ready'])
            self.assertEqual(state['release'], 'old')
            self.assertEqual(state['switches'], 1)
            self.assertEqual(state['starts'], 1)
            self.assertEqual(state['exits'], [23, 0, 0])


def fixture_worker(root):
    state_file = root / 'state.json'
    state = json.loads(state_file.read_text())
    state.setdefault('workers', []).append({'pid': os.getpid(), 'ppid': os.getppid()})
    state_file.write_text(json.dumps(state))
    class PersistentOps(Operations):
        def __init__(self):
            super().__init__(time.monotonic, release=state['release'], ready=state['ready'])
        def save(self):
            state.update(release=self.release, ready=self.ready)
            state_file.write_text(json.dumps(state))
        def switch(self, target):
            super().switch(target)
            state['switches'] += 1
            self.save()
            # Simulates supervisor death immediately after pointer replacement.
            os._exit(23)
        def start(self):
            super().start()
            state['starts'] += 1
            self.save()
    recover(PersistentOps(), 'old', state['deadline'])


def fixture_watchdog(root):
    root.mkdir(exist_ok=True)
    state_file = root / 'state.json'
    state_file.write_text(json.dumps(dict(release='new', ready=False, switches=0,
                                         starts=0, deadline=time.monotonic() + 5, exits=[])))
    # This subprocess supervises workers independently of the unittest caller.
    for _ in range(3):
        worker = subprocess.run([sys.executable, __file__, 'fixture-worker', str(root)], timeout=3)
        state = json.loads(state_file.read_text())
        state['exits'].append(worker.returncode)
        state_file.write_text(json.dumps(state))
    if not state['ready']:
        raise RuntimeError('Fixture restoration failed')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'fixture-worker':
        fixture_worker(Path(sys.argv[2]))
    elif len(sys.argv) == 3 and sys.argv[1] == 'fixture-watchdog':
        fixture_watchdog(Path(sys.argv[2]))
    else:
        unittest.main(verbosity=2)
