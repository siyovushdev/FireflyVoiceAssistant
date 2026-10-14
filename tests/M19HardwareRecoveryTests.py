#!/usr/bin/python3
"""Offline tests. Fixture mode only updates ordinary JSON files, never devices."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
controller = Path(__file__).resolve().parents[1] / 'scripts/m19-hardware-recovery.py'
spec = importlib.util.spec_from_file_location('hardware_recovery', controller)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fake(root):
    w = module.Window.__new__(module.Window)
    w.root = root
    w.unit = 'original.service'
    w.plan = {'old': 'old', 'test_unit': 'isolated-test.service', 'timer_unit': 'isolated.timer'}
    w.operation_deadline = time.monotonic() + 30
    w.armed = lambda: {'deadline': time.monotonic() + 600}
    w.identity = lambda: None
    w.candidate = lambda: None
    w.endpoints = lambda: None
    w.verify_units = lambda: None
    w.record = lambda *args: None
    return w


class RecoveryTests(unittest.TestCase):
    def test_recovery_stops_browser_before_test_and_production_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []
            w.plan['browser_unit'] = 'isolated-browser.service'
            w.run = lambda argv, **kw: events.append((argv, kw.get('timeout')))
            def gates(old):
                events.append('production gate')
                return {'ready': True}
            w.gates = gates
            w.recovery()
            self.assertEqual(events, [
                (['systemctl', 'stop', 'isolated-browser.service'], 15),
                (['systemctl', 'stop', 'isolated-test.service'], 65),
                'production gate'])

    def test_pc_loss_prevents_cutover_before_production_stop(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp))
            w.plan.update(pc_lease_token='token', boot_id='boot', pc_heartbeat_timeout_seconds=15)
            (w.root / 'PC_HEARTBEAT.json').write_text(json.dumps({'token':'token','boot_id':'boot','monotonic':0}))
            w.run = lambda *a, **k: 'active'
            w.stop = lambda: self.fail('lost PC must not stop production')
            with self.assertRaises(RuntimeError): w.cutover()
            self.assertFalse((w.root / 'CUTOVER_STARTED').exists())

    def test_cutover_rechecks_pc_after_slow_production_gates(self):
        for expired in ('lease','gate'):
            with self.subTest(expired=expired), tempfile.TemporaryDirectory() as tmp:
                w = fake(Path(tmp)); state = {'fresh':True}
                w.run = lambda *a, **k: 'active'
                def check():
                    if not state['fresh']: raise RuntimeError('expired ' + expired)
                if expired == 'lease': w.pc_connection = check
                else: w.pc_browser_gate = check
                w.gates = lambda old: state.update(fresh=False)
                w.stop = lambda: self.fail('No stop after expiry during gates')
                with self.assertRaises(RuntimeError): w.cutover()
                self.assertFalse((w.root / 'CUTOVER_STARTED').exists())

    def test_pc_loss_watchdog_requests_independent_recovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); calls = []; records = {}
            w.plan.update(pc_lease_token='token', boot_id='boot', pc_heartbeat_timeout_seconds=15,
                          test_log=str(w.root / 'test.log'), recovery_unit='isolated-recover.service')
            w.run = lambda argv, **kw: calls.append(argv)
            w.record = lambda name, data: records.update({name:data})
            w.watch()
            self.assertEqual(calls,[['systemctl','start','--no-block','isolated-recover.service']])
            self.assertIn('heartbeat',records['TEST_ABORT']['reason'])

    def test_recovery_stops_firefly_audio_before_test(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []
            w.plan['audio_unit'] = 'isolated-audio.service'
            w.run = lambda argv, **kw: events.append(argv)
            w.gates = lambda old: {'ready':True}
            w.recovery()
            self.assertEqual(events,[['systemctl','stop','isolated-audio.service'],
                                     ['systemctl','stop','isolated-test.service']])

    def test_stop_test_before_accepting_already_ready_production(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []
            w.run = lambda argv, **kw: events.append(argv)
            w.gates = lambda old: {'ready': True}
            w.start = lambda: self.fail('Already ready must not be restarted')
            w.recovery()
            self.assertEqual(events, [['systemctl', 'stop', 'isolated-test.service']])
            self.assertTrue((w.root / 'CLOSED').exists())

    def test_recovery_retries_barrier_failure_without_stopping_production(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []; attempts = []
            (w.root / 'CUTOVER_STARTED').touch()
            w.run = lambda argv, **kw: '0' if argv[1] == 'show' else events.append(argv)
            w.gates = lambda old: (_ for _ in ()).throw(RuntimeError('not ready'))
            def barrier():
                attempts.append(1)
                if len(attempts) == 1: raise RuntimeError('foreign owner')
                events.append('barrier')
            w.barrier = barrier
            w.start = lambda: events.append('start original')
            w.wait_ready = lambda *args: {'ready': True}
            with patch.object(module.time, 'sleep'):
                w.recovery()
            self.assertEqual(events, [['systemctl', 'stop', 'isolated-test.service'],
                ['systemctl', 'stop', 'isolated-test.service'], 'barrier', 'start original'])

    def test_exhausted_original_deadline_never_starts(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); w.armed = lambda: {'deadline': time.monotonic() - 361}
            w.run = lambda *a, **k: self.fail('expired recovery must not mutate')
            with self.assertRaises(module.rollout.RecoveryTimeout): w.recovery()

    def test_early_recovery_deadline_survives_supervisor_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for now in (100, 200, 461):
                # A fresh object models a systemd restart; the persisted first
                # entry still bounds recovery even before the trial expires.
                w = fake(root); w.armed = lambda: {'deadline': 700}
                calls = []
                w.run = lambda argv, **kw: calls.append(argv)
                w.gates = lambda old: {'ready': True}
                with patch.object(module.time, 'monotonic', return_value=now):
                    if now < 460:
                        w.recovery()
                        self.assertEqual(len(calls), 1)
                    else:
                        with self.assertRaises(module.rollout.RecoveryTimeout): w.recovery()
                        self.assertEqual(calls, [])
                self.assertEqual(w.operation_deadline, 460)
                self.assertEqual(json.loads((root / 'RECOVERY_STARTED.json').read_text()), 100)

    def test_cutover_one_attempt_and_barrier_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []
            w.run = lambda argv, **kw: 'active' if argv[1] == 'is-active' else events.append(argv)
            w.gates = lambda old: None
            w.stop = lambda: events.append('stop original')
            w.barrier = lambda: events.append('barrier')
            w.cutover()
            self.assertEqual(events, ['stop original', 'barrier', ['systemctl', 'start', '--no-block', 'isolated-test.service']])
            with self.assertRaises(FileExistsError): w.cutover()
            self.assertEqual(len(events), 3)

    def test_cutover_rejects_inactive_timer_or_closed_window(self):
        for closed in (False, True):
            with self.subTest(closed=closed), tempfile.TemporaryDirectory() as tmp:
                w = fake(Path(tmp))
                if closed: (w.root / 'CLOSED').touch()
                w.run = lambda *a, **k: 'inactive'
                w.stop = lambda: self.fail('must not stop')
                with self.assertRaises(RuntimeError): w.cutover()
                self.assertFalse((w.root / 'CUTOVER_STARTED').exists())

    def test_failed_cutover_barrier_does_not_launch_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []
            w.run = lambda *a, **k: 'active'
            w.gates = lambda old: None
            w.stop = lambda: events.append('stop original')
            w.barrier = lambda: (_ for _ in ()).throw(RuntimeError('unknown owner'))
            with self.assertRaises(RuntimeError): w.cutover()
            self.assertEqual(events, ['stop original'])

    def test_permit_never_reuses_consumed_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); (w.root / 'CUTOVER_STARTED').touch(); (w.root / 'CLOSED').touch()
            w.resource_snapshot = lambda: self.fail('must not probe a closed window')
            with self.assertRaises(RuntimeError): w.permit()

    def test_observer_uses_phase_policy_and_catches_split_voice_failure(self):
        for available, line, expected in [(1048575, '', 'Test exited'), (2 * 1048576,
                'SETUP_MEMORY phase=admin_admission available_kib=6000000 cgroup_headroom_kib=9999999 required_kib=6291456\n', 'Phase admission rejected'),
                (2 * 1048576, 'WAITING_WAKE\nVOICE_RECOV', 'VOICE_RECOVERY failure reason=')]:
            with self.subTest(available=available), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); w = fake(root); calls = []
                proc = root / 'proc'; group = root / 'cgroup/system.slice/test.service'
                (proc / '42').mkdir(parents=True); group.mkdir(parents=True)
                binary = root / 'candidate'; binary.touch(); (proc / '42/exe').symlink_to(binary)
                (proc / 'meminfo').write_text(f'MemAvailable: {available} kB\n')
                (proc / '42/smaps_rollup').write_text('Pss: 1 kB\n')
                (group / 'memory.current').write_text('1024\n')
                (group / 'memory.events').write_text('oom 0\noom_kill 0\nmax 0\n')
                log = root / 'test.log'; log.write_text(line)
                w.plan.update(candidate_binary=str(binary), test_log=str(log), recovery_unit='isolated-recover.service')
                records = {}
                w.record = lambda name, data: records.update({name: data})
                def run(argv, **kw):
                    calls.append(argv)
                    if argv[1] == 'show':
                        pid = 0 if len(calls) == 2 and not line else 42
                        return f'MainPID={pid}\nControlGroup=/system.slice/test.service'
                    return ''
                w.run = run
                def paths(value):
                    value = str(value)
                    if value == '/proc' or value.startswith('/proc/'): return root / value.lstrip('/')
                    if value == '/sys/fs/cgroup': return root / 'cgroup'
                    return Path(value)
                def next_chunk(_):
                    with log.open('a') as stream: stream.write('ERY failure reason=fixture wake failure\n')
                with patch.object(module, 'Path', side_effect=paths), patch.object(module.time, 'sleep', side_effect=next_chunk): w.watch()
                self.assertEqual(calls[-1], ['systemctl', 'start', '--no-block', 'isolated-recover.service'])
                self.assertIn(expected, records['TEST_ABORT']['reason'])
                self.assertEqual(len(calls), 2 if 'Phase' in expected else 3)

    def test_degraded_original_gets_only_one_verified_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); events = []
            (w.root / 'CUTOVER_STARTED').touch()
            (w.root / 'RESTORE_STARTS.json').write_text('1')
            w.run = lambda argv, **kw: '42' if argv[1] == 'show' else None
            w.gates = lambda old: (_ for _ in ()).throw(RuntimeError('degraded'))
            w.confirm_original_process = lambda pid: events.append(('verified', pid))
            w.stop = lambda: events.append('stop original')
            w.barrier = lambda: events.append('barrier')
            w.start = lambda: events.append('start original')
            w.wait_ready = lambda *args: {'ready': True}
            w.recovery()
            self.assertEqual(events, [('verified', 42), 'stop original', 'barrier', 'start original'])
            self.assertEqual(json.loads((w.root / 'RESTORE_STARTS.json').read_text()), 2)
            with patch.object(module.time, 'sleep', side_effect=module.rollout.RecoveryTimeout('stop test loop')):
                with self.assertRaises(module.rollout.RecoveryTimeout): w.recovery()
            self.assertEqual(len(events), 4)

    def test_unknown_production_is_not_stopped(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = fake(Path(tmp)); (w.root / 'CUTOVER_STARTED').touch()
            w.run = lambda argv, **kw: '42' if argv[1] == 'show' else None
            w.gates = lambda old: (_ for _ in ()).throw(RuntimeError('degraded'))
            w.confirm_original_process = lambda pid: (_ for _ in ()).throw(RuntimeError('unknown argv'))
            w.stop = lambda: self.fail('unknown process must not be stopped')
            w.start = lambda: self.fail('unknown resources must not be reused')
            with patch.object(module.time, 'sleep', side_effect=module.rollout.RecoveryTimeout('stop test loop')):
                with self.assertRaises(module.rollout.RecoveryTimeout): w.recovery()
            self.assertFalse((w.root / 'RESTORE_STARTS.json').exists())


def fixture(root):
    """Same recovery loop under real systemd; ALL device/service ops are fake."""
    state_file = root / 'fixture-state.json'
    if not state_file.exists():
        state_file.write_text(json.dumps({'deadline': time.monotonic() + 20,
            'ready': False, 'starts': 0, 'stops': 0, 'pids': [], 'ppids': []}))
    state = json.loads(state_file.read_text())
    state['pids'].append(os.getpid()); state['ppids'].append(os.getppid())
    def save(): state_file.write_text(json.dumps(state, indent=2) + '\n')
    save()
    w = fake(root)
    (root / 'CUTOVER_STARTED').touch()
    w.armed = lambda: {'deadline': state['deadline'] - 360}
    def run(argv, **kw):
        if argv == ['systemctl', 'show', 'original.service', '-p', 'MainPID', '--value']: return '0'
        assert argv == ['systemctl', 'stop', 'isolated-test.service']
        state['stops'] += 1; save()
    w.run = run
    def gates(old):
        if not state['ready']: raise RuntimeError('fixture not ready')
        return {'ready': True}
    w.gates = gates
    w.barrier = lambda: None
    def start():
        state['starts'] += 1; state['ready'] = True; save()
        os._exit(23)  # Crash after simulated start, before successful verification.
    w.start = start
    w.wait_ready = lambda *args: gates('old')
    w.recovery()
    assert state['ready'] and state['starts'] == 1 and state['stops'] == 2
    assert len(set(state['pids'])) == 2 and state['ppids'] == [1, 1]
    (root / 'FIXTURE_PASS').write_text('systemd restarted recovery; one simulated start; no hardware/service I/O\n')


def pc_loss_fixture(root):
    """Real PID1 dispatch/restart with simulated device/service ops only."""
    import subprocess
    w = fake(root)
    unit = root.name + '-recover.service'
    w.plan.update(pc_lease_token='offline-only', boot_id='offline-only',
                  pc_heartbeat_timeout_seconds=15, test_log=str(root/'unused.log'),
                  recovery_unit=unit)
    w.record = lambda name, value: (root/(name+'.json')).write_text(json.dumps(value))
    def run(argv, **kwargs):
        assert argv == ['systemctl','start','--no-block',unit]
        subprocess.run(argv,check=True,timeout=5)
    w.run = run
    w.watch()  # Missing heartbeat causes recovery without opening any device.
    assert 'heartbeat' in (root/'TEST_ABORT.json').read_text()
    (root/'PC_LOSS_FIXTURE_PASS').write_text('PID1 watcher dispatched independent fake recovery\n')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'fixture': fixture(Path(sys.argv[2]))
    elif len(sys.argv) == 3 and sys.argv[1] == 'pc-loss-fixture': pc_loss_fixture(Path(sys.argv[2]))
    else: unittest.main(verbosity=2)
