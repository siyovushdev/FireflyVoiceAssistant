#!/usr/bin/env python3
"""Launcher faults only; no systemd, production API, NPU or microphone."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('launcher', ROOT / 'scripts/deployment-launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class Clock:
    def __init__(self):
        self.now = 0
    def __call__(self):
        return self.now
    def sleep(self, seconds):
        self.now += seconds


def properties(active='activating', sub='start', code=0, status=0, result='success', job=1):
    def v(kind, data):
        return {'type': kind, 'data': data}
    finished = active in ('failed', 'active')
    return {'Unit': {'ActiveState': v('s', active), 'SubState': v('s', sub),
                     'Job': v('(uo)', [job, '/'])},
            'Service': {'MainPID': v('u', 0 if finished else 123), 'ControlPID': v('u', 0),
                        'Result': v('s', result), 'ExecMainCode': v('i', code),
                        'ExecMainStatus': v('i', status),
                        'ExecMainExitTimestampMonotonic': v('t', 1 if finished else 0)}}


class LauncherTests(unittest.TestCase):
    def test_submission_is_asynchronous_and_retains_terminal_state(self):
        ops = Mock()
        result = launcher.submit_worker(ops, 'fixture.service', ['/usr/bin/python3', 'worker.py'])
        cmd = ops.run.call_args.args[0]
        self.assertIn('--no-block', cmd)
        self.assertIn('--property=RemainAfterExit=yes', cmd)
        self.assertIn('--property=Type=oneshot', cmd)
        self.assertIn('--property=TimeoutStartSec=240s', cmd)
        self.assertNotIn('--wait', cmd)
        self.assertNotIn('--collect', cmd)
        self.assertEqual(ops.run.call_args.kwargs, {'timeout': 15})
        self.assertEqual(result['state'], 'SUBMITTED')
        self.assertEqual(ops.run.call_count, 1)

    def test_submission_failure_is_uncertain_not_worker_completion(self):
        for error in [subprocess.TimeoutExpired('systemd-run', 15),
                      RuntimeError('D-Bus connection lost'), launcher.controller.RecoveryTimeout('deadline')]:
            with self.subTest(error=error):
                ops = Mock(); ops.run.side_effect = error
                result = launcher.submit_worker(ops, 'fixture.service', ['worker'])
                self.assertEqual(result['state'], 'SUBMISSION_UNCERTAIN')
                self.assertEqual(ops.run.call_count, 1)  # No retry, stop, or timer control.

    def observe(self, selector, deadline=30):
        clock = Clock(); ops = Mock()
        ops.systemd_properties.side_effect = lambda unit, interface: selector(clock.now)[interface]
        result = launcher.observe_worker(ops, 'fixture.service', deadline, clock=clock, pause=clock.sleep)
        ops.run.assert_not_called()
        return result, clock, ops

    def test_operation_longer_than_submission_timeout(self):
        result, clock, _ = self.observe(lambda t: properties() if t < 20 else
            properties('active', 'exited', 1, 0, job=0))
        self.assertEqual(result['state'], 'WORKER_SUCCEEDED')
        self.assertEqual(clock.now, 20)

    def test_long_failed_worker_reports_actual_exit_status(self):
        result, clock, _ = self.observe(lambda t: properties() if t < 20 else
            properties('failed', 'failed', 1, 42, 'exit-code', job=0))
        self.assertEqual(result['state'], 'FORWARD_FAILED')
        self.assertEqual(result['observation']['exit_status'], 42)
        self.assertEqual(clock.now, 20)

    def test_observation_deadline_does_not_stop_worker_or_watchdog(self):
        result, clock, ops = self.observe(lambda _: properties(), deadline=3)
        self.assertEqual(result['state'], 'OBSERVATION_DEADLINE')
        self.assertEqual(clock.now, 3)
        ops.run.assert_not_called()

    def test_transient_probe_error_does_not_mean_completed(self):
        def selector(t):
            if t < 1:
                raise RuntimeError('unit temporarily unavailable')
            return properties('active', 'exited', 1, 0, job=0)
        result, clock, _ = self.observe(selector)
        self.assertEqual(result['state'], 'WORKER_SUCCEEDED')
        self.assertEqual(clock.now, 1)

    def test_missing_malformed_and_incomplete_success_fail_closed(self):
        for key, value in [('MainPID', {'type': 's', 'data': '0'}),
                           ('ExecMainExitTimestampMonotonic', {'type': 't', 'data': 0}),
                           ('ExecMainStatus', {'type': 'i', 'data': 42}),
                           ('ControlPID', {'type': 'u', 'data': 22})]:
            with self.subTest(key=key):
                p = properties('active', 'exited', 1, 0, job=0); p['Service'][key] = value
                result, _, _ = self.observe(lambda _: p, deadline=1)
                self.assertEqual(result['state'], 'OBSERVATION_DEADLINE')
        result, _, _ = self.observe(lambda _: {}, deadline=1)
        self.assertEqual(result['state'], 'OBSERVATION_DEADLINE')

    def test_interrupt_preserves_independent_recovery(self):
        ops = Mock(); ops.systemd_properties.side_effect = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            launcher.observe_worker(ops, 'fixture.service', 100, clock=lambda: 0)
        ops.run.assert_not_called()

    def test_single_submission_and_pinned_launcher(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'scripts').mkdir()
            file = root / 'scripts/deployment-launcher.py'; file.write_text('fixture')
            ops = Mock(); ops.root = root; ops.plan_sha256 = 'original'
            ops.plan = dict(old='old', worker_unit='fixture.service', launcher_sha256=launcher.controller.sha(file))
            with patch.object(launcher, '__file__', str(file)), \
                 patch.object(Path, 'lstat', return_value=Mock(st_uid=0, st_mode=0o100600)), \
                 patch.object(Path, 'is_symlink', return_value=False), \
                 patch.object(Path, 'is_file', return_value=True):
                self.assertEqual(launcher.start(ops)['state'], 'SUBMITTED')
                self.assertEqual(ops.run.call_count, 1)
                with self.assertRaises(FileExistsError):
                    launcher.start(ops)
                self.assertEqual(ops.run.call_count, 1)
                claim = json.loads((root / 'FORWARD_LAUNCH_STARTED').read_text())
                self.assertEqual(claim['plan_sha256'], 'original')

    def test_worker_exit_zero_requires_acceptance_and_actual_production_gates(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = Mock(); ops.root = Path(tmp); ops.plan_sha256 = 'original'
            ops.plan = dict(worker_unit='fixture.service', deadline_monotonic=100, new='new')
            (ops.root / 'FORWARD_LAUNCH_STARTED').write_text(json.dumps(dict(plan_sha256='original', unit='fixture.service')))
            with patch.object(launcher, 'verify_launcher'), \
                 patch.object(launcher, 'observe_worker', side_effect=lambda *args: {'state': 'WORKER_SUCCEEDED'}):
                self.assertEqual(launcher.observe(ops)['state'], 'UNVERIFIED_RESULT')
                (ops.root / 'ACCEPTED').touch(); (ops.root / 'evidence').mkdir()
                (ops.root / 'evidence/deployed-verified.json').write_text('{"result":"DEPLOYED_VERIFIED"}')
                ops.gates.side_effect = RuntimeError('ownership/health/store gate failed')
                self.assertEqual(launcher.observe(ops)['state'], 'UNVERIFIED_RESULT')
                ops.gates.side_effect = None; ops.gates.return_value = {'verified': True}
                self.assertEqual(launcher.observe(ops)['state'], 'DEPLOYED_VERIFIED')
                ops.run.assert_not_called()

    def test_run_observes_even_after_uncertain_submission(self):
        ops = Mock()
        with patch.object(sys, 'argv', ['launcher', 'run', '/fixture']), \
             patch.object(launcher.controller, 'Production', return_value=ops), \
             patch.object(launcher, 'start', return_value={'state': 'SUBMISSION_UNCERTAIN'}) as start, \
             patch.object(launcher, 'observe', return_value={'state': 'DEPLOYED_VERIFIED'}) as observe, \
             patch('builtins.print'):
            self.assertEqual(launcher.main(), 0)
            start.assert_called_once_with(ops)
            observe.assert_called_once_with(ops)

    def test_observation_cannot_change_original_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = Mock(); ops.root = Path(tmp); ops.plan_sha256 = 'changed'
            ops.plan = dict(worker_unit='fixture.service')
            (ops.root / 'FORWARD_LAUNCH_STARTED').write_text(json.dumps(dict(plan_sha256='original', unit='fixture.service')))
            with patch.object(launcher, 'verify_launcher'):
                with self.assertRaisesRegex(RuntimeError, 'original submitted plan'):
                    launcher.observe(ops)
            ops.run.assert_not_called()
            ops.systemd_properties.assert_not_called()

    def test_slow_probe_after_deadline_cannot_authorize_success(self):
        clock = Clock(); ops = Mock()
        def probe(unit, interface):
            clock.now = 31
            return properties('active', 'exited', 1, 0, job=0)[interface]
        ops.systemd_properties.side_effect = probe
        result = launcher.observe_worker(ops, 'fixture.service', 30, clock=clock, pause=clock.sleep)
        self.assertEqual(result['state'], 'OBSERVATION_DEADLINE')
        ops.run.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
