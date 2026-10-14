#!/usr/bin/python3
"""Coordinator guards/faults; no systemd, production, NPU or microphone."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('runner', SOURCE / 'scripts/release-runner.py')
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def test_new_assets_require_exact_approved_and_rebuilt_fingerprint(self):
        old, new = 'a' * 64, 'b' * 64
        self.assertEqual(runner.approved_assets(new, old, new, new), new)
        self.assertEqual(runner.approved_assets(old, old, None, None), old)
        for pin, built in [(None, new), ('invalid', new), (old, new), (new, old)]:
            with self.subTest(pin=pin, built=built), self.assertRaises(RuntimeError):
                runner.approved_assets(new, old, pin, built)

    def test_offline_child_never_receives_inherited_owner_permission(self):
        probe = ['/usr/bin/python3', '-c',
                 'import os; print(repr(os.environ.get("FIREFLY_OWNER_ACCEPTED")))']
        for inherited in [None, '1']:
            with self.subTest(inherited=inherited), tempfile.TemporaryDirectory() as tmp, \
                 patch.dict(os.environ):
                os.environ.pop('FIREFLY_OWNER_ACCEPTED', None)
                if inherited is not None:
                    os.environ['FIREFLY_OWNER_ACCEPTED'] = inherited
                self.assertEqual(runner.command(Path(tmp), probe), 'None')
                self.assertEqual(os.environ.get('FIREFLY_OWNER_ACCEPTED'), inherited)

    def test_explicit_owner_permission_is_scoped_to_one_child(self):
        probe = ['/usr/bin/python3', '-c',
                 'import os; print(repr(os.environ.get("FIREFLY_OWNER_ACCEPTED")))']
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'FIREFLY_OWNER_ACCEPTED': '1'}):
            root = Path(tmp)
            self.assertEqual(runner.command(root, probe, owner_authorized=True), "'1'")
            self.assertEqual(runner.command(root, probe), 'None')

    def test_all_offline_checks_strip_parent_permission(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'FIREFLY_OWNER_ACCEPTED': '1'}), \
             patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')) as run:
            runner.application_checks(Path(tmp), SOURCE)
            runner.mandatory_fixtures(Path(tmp))
            self.assertEqual(run.call_count, 8)
            for call in run.call_args_list:
                self.assertNotIn('FIREFLY_OWNER_ACCEPTED', call.kwargs['env'])

    def test_preparation_build_identity_contract_backup_and_receipt(self):
        # Real private filesystem preparation; only production paths, commands and
        # binary/status interfaces are substituted. No production file is opened.
        real_path = Path
        for fault in [None, 'source', 'assets', 'voice']:
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp); target = base / 'bundle'; prefix = base / 'prefix'; store = base / 'store'
                store.mkdir(mode=0o700); (store / 'configuration.json').write_text('fixture configuration')
                (prefix / 'releases').mkdir(parents=True)
                for release in ['old', 'new']:
                    tree = prefix / 'releases' / release; tree.mkdir()
                    (tree / 'bin').mkdir(); (tree / 'bin/firefly-demo').write_text('FILE_ONLY fake binary; never executed')
                    runner.write(tree / 'manifest.json', {'artifacts': [{'path': 'lib/fixture', 'sha256': 'same'}], 'shared_models': []})
                (prefix / 'current').symlink_to('releases/old')
                if fault == 'voice':
                    runner.write(prefix / 'releases/new/manifest.json', {'artifacts': [], 'shared_models': []})
                class Bundle:
                    parent = real_path('/run')
                    name = 'file-only-unit-bundle'
                    def exists(self): return target.exists()
                    def mkdir(self, **kw): return target.mkdir(**kw)
                    def __truediv__(self, child): return target / child
                root = Bundle()
                def path(value):
                    if str(value) == '/opt/firefly/voice-assistant': return prefix
                    if str(value) == '/var/lib/firefly/store': return store
                    return real_path(value)
                original = runner.load(SOURCE / 'scripts/production-rollout.py')
                ops = Mock(); ops.gates.return_value = {'verified_old': True}
                sha = lambda p: 'unit-hash' if str(p).startswith('/etc/') else hashlib.sha256(real_path(p).read_bytes()).hexdigest()
                m = SimpleNamespace(sha=sha, manifest=original.manifest, Production=Mock(return_value=ops))
                def identity(binary):
                    return dict(head=binary.parents[1].name, dirty=False,
                                source_sha256='wrong' if fault == 'source' else 'fingerprint',
                                admin_assets_sha256='changed' if fault == 'assets' and binary.parents[1].name == 'new' else 'assets')
                package = SimpleNamespace(build_identity=identity, source_tree_fingerprint=lambda _: 'fingerprint')
                def command(root, argv, **kw):
                    if argv[0] == 'cp':
                        import shutil
                        shutil.copytree(store, target / 'backup-store'); shutil.copystat(store, target / 'backup-store')
                    return ''
                response = Mock(); response.__enter__ = Mock(return_value=response); response.__exit__ = Mock(return_value=False)
                response.read.return_value = json.dumps({'worker_pid': __import__('os').getpid()}).encode()
                def copy_bundle(root, source):
                    # This test substitutes tool loading, so copy only its runner
                    # pin. Full bundle copying is exercised by the systemd suite.
                    root.mkdir(mode=0o700)
                    (root / 'evidence').mkdir(mode=0o700)
                    (root / 'scripts').mkdir(mode=0o700)
                    (root / 'scripts/release-runner.py').write_bytes(real_path(runner.__file__).read_bytes())
                    for name in ['scripts/production-rollout.py', 'scripts/deployment-launcher.py',
                                 'packaging/firefly_recovery.py']:
                        destination = root / name
                        destination.parent.mkdir(exist_ok=True)
                        destination.write_bytes((SOURCE / name).read_bytes())
                with patch.object(runner, 'Path', side_effect=path), \
                     patch.object(runner, 'prepare_files', side_effect=copy_bundle), \
                     patch.object(runner.subprocess, 'check_output', side_effect=['new', b'', real_path(runner.__file__).read_bytes()]), \
                     patch.object(runner, 'load', side_effect=[m, package]), \
                     patch.object(runner, 'command', side_effect=command) as commands, \
                     patch.object(runner, 'mandatory_fixtures') as fixtures, \
                     patch.object(runner, 'application_checks') as app, \
                     patch.object(runner.urllib.request, 'urlopen', return_value=response):
                    if fault:
                        with self.assertRaisesRegex(RuntimeError, 'mismatch|contract changed'):
                            runner.prepare(root, SOURCE, 'new', 'new')
                        self.assertFalse((target / 'PREPARED').exists()); m.Production.assert_not_called()
                    else:
                        runner.prepare(root, SOURCE, 'new', 'new')
                        self.assertEqual(original.manifest(target / 'backup-store'), original.manifest(store))
                        receipt = json.loads((target / 'PREPARED').read_bytes())
                        self.assertEqual(receipt['plan_sha256'], sha(target / 'plan.json'))
                        fixtures.assert_called_once(); app.assert_called_once(); ops.gates.assert_called_once_with('old')
                        self.assertFalse((target / 'CUTOVER_STARTED').exists())
                    self.assertFalse(any(call.args[1][0] == 'systemctl' for call in commands.call_args_list))
                    authorized = [call for call in commands.call_args_list if call.kwargs.get('owner_authorized')]
                    self.assertEqual(len(authorized), 2)
                    self.assertEqual([call.args[1][-1] for call in authorized], ['old', 'new'])
                    self.assertTrue(all('verify' in call.args[1] for call in authorized))

    def test_application_checks_use_fixed_existing_commands_without_root_runtime(self):
        with patch.object(runner, 'command') as command:
            runner.application_checks(Path('/fixture'), Path('/source'))
            commands = [call.args[1] for call in command.call_args_list]
            self.assertEqual(commands[0], ['runuser', '-u', 'firefly', '--', 'ctest', '--test-dir',
                                          '/source/build', '--output-on-failure', '--no-tests=error', '-j2'])
            self.assertEqual([cmd[-1] for cmd in commands[1:]], ['typecheck', 'test', 'build'])
            self.assertTrue(all(cmd[:4] == ['runuser', '-u', 'firefly', '--'] for cmd in commands))

    def test_atomic_plan_publication_preserves_prior_bytes_on_partial_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'plan.json'; path.write_text('original plan')
            with patch.object(runner.os, 'fsync', side_effect=OSError('disk failure')):
                with self.assertRaisesRegex(OSError, 'disk failure'):
                    runner.write(path, {'partial': 'unpublished'})
            self.assertEqual(path.read_text(), 'original plan')

    def test_mandatory_command_preserves_response_before_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            reply = subprocess.CompletedProcess(['fixture'], 23, 'original stdout', 'original stderr')
            with patch.object(runner.subprocess, 'run', return_value=reply):
                with self.assertRaisesRegex(RuntimeError, 'Preparation command failed'):
                    runner.command(root, ['fixture'])
            saved = json.loads((root / 'preparation-commands.jsonl').read_text())
            self.assertEqual(saved['stdout'], 'original stdout')
            self.assertEqual(saved['stderr'], 'original stderr')
            self.assertEqual(saved['returncode'], 23)

    def test_mandatory_suite_is_fixed_and_failure_stops_preparation(self):
        root = Path('/fixture')
        with patch.object(runner, 'command') as command:
            runner.mandatory_fixtures(root)
            self.assertEqual([call.args[1][1] for call in command.call_args_list],
                ['/fixture/tests/DeploymentLauncherTests.py', '/fixture/tests/ReleaseRecoveryTests.py',
                 '/fixture/tests/ReleaseRunnerTests.py',
                 '/fixture/tests/DeploymentLauncherSystemdTests.py'])
        with patch.object(runner, 'command', side_effect=RuntimeError('mandatory failed')) as command:
            with self.assertRaisesRegex(RuntimeError, 'mandatory failed'):
                runner.mandatory_fixtures(root)
            self.assertEqual(command.call_count, 1)

    def test_dirty_or_untracked_runner_rejected_before_bundle_creation(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp).resolve(); root = Path('/run/firefly-unit-never-created-' + source.name)
            for replies, expected in [(['commit', b' M scripts/release-runner.py'], 'exact clean'),
                                      (['commit', b'', b'untracked runner'], 'tracked approved')]:
                with self.subTest(expected=expected), patch.object(runner.subprocess, 'check_output', side_effect=replies), \
                     patch.object(runner, 'prepare_files') as copy:
                    with self.assertRaisesRegex(RuntimeError, expected):
                        runner.prepare(root, source, 'commit', 'new')
                    copy.assert_not_called()

    def exercise(self, root, *, outcome='DEPLOYED_VERIFIED', guard_error=False,
                 arm_error=False, diagnostic_error=False, recovered=False, expired=False):
        (root / 'scripts').mkdir(); (root / 'evidence').mkdir()
        file = root / 'scripts/release-runner.py'; file.write_text('fixture runner')
        sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
        plan = dict(old='old', new='new', worker_unit='fixture-forward.service', rollback_unit='fixture-rollback.service',
                    rollback_timer='fixture-rollback.timer', deadline_monotonic=time.monotonic() + 240,
                    runner_sha256=sha(file))
        runner.write(root / 'plan.json', plan)
        runner.write(root / 'PREPARED', {'plan_sha256': sha(root / 'plan.json')})
        instances = []
        def factory(path):
            ops = Mock(); ops.root = root; ops.plan = json.loads((root / 'plan.json').read_bytes())
            ops.plan_sha256 = sha(root / 'plan.json'); ops.recovery_deadline = ops.plan['deadline_monotonic'] + 360
            if expired:
                ops.recovery_deadline = time.monotonic() - 1
            ops.gates.side_effect = lambda target: {'verified_target': target}
            if guard_error:
                ops.supervision_gate.side_effect = RuntimeError('unsafe real timer configuration')
            ops.systemd_properties.return_value = {'NextElapseUSecMonotonic': {'type': 't', 'data': int((time.monotonic() + 240) * 1000000)}}
            ops.run.return_value = 'optional unit diagnostic'
            if diagnostic_error:
                ops.run.side_effect = RuntimeError('optional diagnostic unavailable')
            instances.append(ops)
            return ops
        m = SimpleNamespace(Production=factory, sha=sha, utc=lambda: 'fixture UTC',
                            property_value=lambda values, key, signature: values[key]['data'])
        def launch(ops):
            (root / 'FORWARD_LAUNCH_STARTED').write_text('exclusive claim')
            if recovered or expired:
                (root / 'CUTOVER_STARTED').write_text('fixture cutover')
            if recovered:
                runner.write(root / 'evidence/rollback-result.json', {'result': 'ROLLED_BACK'})
        launcher = SimpleNamespace(controller=m, start=Mock(side_effect=launch), observe=Mock(return_value={'state': outcome}))
        def load(path):
            return launcher if path.name == 'deployment-launcher.py' else m
        with patch.object(runner, '__file__', str(file)), patch.object(runner, 'load', side_effect=load), \
             patch.object(runner, 'command', side_effect=RuntimeError('arm failed') if arm_error else None) as command:
            result = runner.run(root, 'fixture authorization')
            frozen = (root / 'plan.json').read_bytes()
            with self.assertRaises((RuntimeError, FileExistsError)):
                runner.run(root, 'no second launch')
            self.assertEqual((root / 'plan.json').read_bytes(), frozen)
        return result, instances, launcher, command

    def test_success_requires_independent_existing_gates_before_timer_stop(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); result, instances, launcher, command = self.exercise(root)
            self.assertEqual(result['result'], 'DEPLOYED_VERIFIED')
            self.assertEqual(instances[2].gates.call_args.args, ('new',))
            launcher.start.assert_called_once(); launcher.observe.assert_called_once()
            self.assertIn('--property=Type=exec', command.call_args.args[1])
            self.assertIn('--on-active=240s', command.call_args.args[1])
            instances[1].run.assert_any_call(['systemctl', 'stop', 'fixture-rollback.timer'], timeout=10)
            self.assertTrue((root / 'PLAN_FROZEN').exists())

    def test_supervision_failure_aborts_before_forward_submission(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, instances, launcher, _ = self.exercise(Path(tmp), guard_error=True)
            self.assertEqual(result['result'], 'ABORTED_BEFORE_CUTOVER')
            launcher.start.assert_not_called()
            self.assertEqual(result['verification']['verified_target'], 'old')

    def test_arm_failure_does_not_submit_forward_or_stop_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, _, launcher, _ = self.exercise(Path(tmp), arm_error=True)
            self.assertEqual(result['result'], 'ABORTED_BEFORE_CUTOVER')
            launcher.start.assert_not_called()

    def test_optional_diagnostics_do_not_change_verified_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, _, _, _ = self.exercise(Path(tmp), diagnostic_error=True)
            self.assertEqual(result['result'], 'DEPLOYED_VERIFIED')
            self.assertIn('error', result['diagnostics'])
            self.assertFalse(result['watchdog_disarmed_after_verification'])

    def test_failed_worker_before_cutover_verifies_original_without_recovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, _, _, _ = self.exercise(Path(tmp), outcome='FORWARD_FAILED')
            self.assertEqual(result['result'], 'ABORTED_BEFORE_CUTOVER')
            self.assertEqual(result['verification']['verified_target'], 'old')

    def test_failure_or_observation_timeout_waits_for_independent_recovery(self):
        for outcome in ['FORWARD_FAILED', 'OBSERVATION_DEADLINE', 'UNVERIFIED_RESULT']:
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as tmp:
                result, _, _, _ = self.exercise(Path(tmp), outcome=outcome, recovered=True)
                self.assertEqual(result['result'], 'ROLLED_BACK')
                self.assertEqual(result['verification']['verified_target'], 'old')

    def test_unconfirmed_recovery_does_not_disarm_watchdog(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, instances, _, _ = self.exercise(Path(tmp), outcome='OBSERVATION_DEADLINE', expired=True)
            self.assertEqual(result['result'], 'BLOCKED_UNKNOWN')
            self.assertTrue(result['watchdog_preserved'])
            self.assertFalse(any(call.args[0][:2] == ['systemctl', 'stop'] for ops in instances for call in ops.run.call_args_list))


if __name__ == '__main__':
    unittest.main(verbosity=2)
