#!/usr/bin/python3
"""Launch ordering/expiry regression checks; no service/device operations."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('launch', Path(__file__).resolve().parents[1] / 'scripts/m19-hardware-start.py')
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class StartTests(unittest.TestCase):
    def test_mock_budget_covers_trial_recovery_and_margin(self):
        remaining = launch.recovery.mock_remaining_seconds(1_000_000, 1800_000_000, 11)
        self.assertEqual(remaining, 1790)
        for now in (722, 1802):
            with self.assertRaises(RuntimeError):
                launch.recovery.mock_remaining_seconds(1_000_000, 1800_000_000, now)
        with self.assertRaises(RuntimeError): launch.recovery.mock_remaining_seconds(0, 0, 1)

    def test_old_mock_and_paths_are_never_reused(self):
        plan = {'mock_unit': 'old-mock.service', 'argv': ['/old/store'], 'model': '/opt/model'}
        updated = launch.rewrite(plan, [('/old', '/fresh'), ('old-mock', 'fresh-mock')])
        self.assertEqual(updated, {'mock_unit': 'fresh-mock.service', 'argv': ['/fresh/store'], 'model': '/opt/model'})
        self.assertEqual(plan['mock_unit'], 'old-mock.service')

    def test_prepare_only_never_arms_or_starts_hardware(self):
        with patch.object(launch, 'prepare', return_value=Path('/fresh')) as prepare, \
             patch.object(launch, 'start_window') as start, \
             patch.object(sys, 'argv', ['launch', 'prepare-only', '--browser-gate', '/gate.json']):
            launch.main(); launch.main()
        self.assertEqual(prepare.call_count, 2)
        start.assert_not_called()

    def test_authorized_start_always_prepares_first(self):
        events = []
        with patch.object(launch, 'prepare', side_effect=lambda gate, mode: events.append('fresh') or Path('/fresh')), \
             patch.object(launch, 'start_window', side_effect=lambda root: events.append(str(root))), \
             patch.object(sys, 'argv', ['launch', 'start', '--browser-gate', '/gate.json']): launch.main()
        self.assertEqual(events, ['fresh', '/fresh'])

    def test_missing_browser_gate_never_stops_production(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'plan.json').write_text('{}')
            with patch.object(launch, 'run') as run:
                with self.assertRaises(KeyError): launch.start_window(root)
                run.assert_not_called()

    def test_firefly_requires_external_pc_gate_and_cannot_run_browser(self):
        with patch.object(launch, 'run') as run:
            with self.assertRaises(RuntimeError): launch.browser_gate(Path('/evidence'), None)
            with self.assertRaises(RuntimeError): launch.run_trial(Path('/root'))
            run.assert_not_called()

    def test_stale_or_rebooted_browser_gate_fails_before_any_service_call(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        for gate in ({'boot_id':'different'}, {'boot_id':boot,'checked_monotonic':0}):
            with patch.object(launch, 'run') as run:
                with self.assertRaises(RuntimeError): launch.verify_browser_gate({'browser_gate':gate})
                run.assert_not_called()

    def test_preflight_and_arm_failures_never_submit_forward(self):
        for failure in ('check', 'arm', None):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); (root / 'units').mkdir()
                (root / 'plan.json').write_text(json.dumps({'timer_unit': 'fresh.timer',
                    'forward_unit': 'fresh-forward.service', 'recovery_unit': 'fresh-recover.service'}))
                calls = []
                def run(argv, **kwargs):
                    argv = [str(v) for v in argv]; calls.append(argv)
                    if failure in argv: raise RuntimeError('injected ' + failure)
                    return ''
                with patch.object(launch, 'run', side_effect=run), patch.object(launch, 'verify_browser_gate'), patch.object(launch, 'verify_voice_plan'):
                    if failure:
                        with self.assertRaises(RuntimeError): launch.start_window(root)
                        self.assertFalse(any('fresh-forward.service' in argv for argv in calls))
                        if failure == 'check': self.assertEqual(len(calls), 1)
                        else: self.assertEqual(calls[-1], ['systemctl', 'stop', 'fresh.timer'])
                    else:
                        launch.start_window(root)
                        timer = next(i for i, argv in enumerate(calls) if argv == ['systemctl', 'start', 'fresh.timer'])
                        arm = next(i for i, argv in enumerate(calls) if 'arm' in argv)
                        forward = next(i for i, argv in enumerate(calls) if 'fresh-forward.service' in argv)
                        self.assertLess(timer, arm); self.assertLess(arm, forward)

    def test_known_failed_synthetic_audio_blocks_before_workspace_or_service(self):
        with tempfile.TemporaryDirectory() as tmp:
            gate=Path(tmp)/'gate.json'; gate.write_text('{}')
            with patch.object(launch.os,'geteuid',return_value=0), \
                 patch.object(launch,'verify_browser_gate'), patch.object(launch,'run') as run, \
                 patch.object(launch.tempfile,'mkdtemp') as mkdir:
                with self.assertRaisesRegex(RuntimeError,'VOICE_ASSET_BLOCKED'):
                    launch.prepare(gate, 'synthetic-acoustic')
                run.assert_not_called(); mkdir.assert_not_called()

    def test_owner_voice_binding_rejects_mode_change_and_playback_files(self):
        plan={'voice_mode':'owner-live','pc_browser':{'transport':{'voice_mode':'owner-live'}},'audio_files':{}}
        launch.verify_voice_plan(plan)
        plan['audio_files']={'on':{'path':'/wav'}}
        with self.assertRaisesRegex(RuntimeError,'must not use playback'): launch.verify_voice_plan(plan)
        plan['audio_files']={}; plan['pc_browser']['transport']['voice_mode']='synthetic-acoustic'
        with self.assertRaisesRegex(RuntimeError,'differs'): launch.verify_voice_plan(plan)


if __name__ == '__main__': unittest.main(verbosity=2)
