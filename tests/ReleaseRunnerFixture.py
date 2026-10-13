"""File-only runtime boundary for testing the unchanged rollout controller.

No fixture interface is exposed by the production runner. This test adapter
replaces only production paths/runtime; real systemd supervision is unchanged.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('reviewed_controller', ROOT / 'scripts/reviewed-controller.py')
reviewed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reviewed)
reviewed.__file__ = str(ROOT / 'scripts/production-rollout.py')
Base = reviewed.Production


class FileOnlyProduction(Base):
    def __init__(self, root):
        if root != ROOT or root.parent != Path('/run') or not root.name.startswith('firefly-release-runner-fixture-'):
            raise RuntimeError('Fixture root only')
        super().__init__(root)
        self.unit = root.name + '-runtime.service'
        self.prefix = root / 'prefix'

    def run(self, argv, timeout=90):
        if ((self.root / 'scenario').read_text().strip() == 'diagnostic_error'
                and argv == ['systemctl', 'show', self.plan['worker_unit'],
                             self.plan['rollback_unit'], self.plan['rollback_timer']]):
            raise RuntimeError('Injected optional diagnostic failure')
        if any(x in arg for arg in argv for x in ['firefly-voice-assistant', '/dev/', '/opt/', '/var/lib/']):
            raise RuntimeError('Forbidden production operation in fixture')
        if argv[0] == 'systemctl' and argv[1] in ('start', 'stop'):
            if any(not arg.startswith(self.root.name) for arg in argv[2:] if not arg.startswith('--')):
                raise RuntimeError('Foreign unit')
        return super().run(argv, timeout)

    def current(self):
        return (self.root / 'current-release').read_text().strip()

    def owners(self):
        probe = subprocess.run(['fuser', str(self.root / 'resource')], capture_output=True, text=True, timeout=3)
        self.record('fixture-resource-probe', dict(returncode=probe.returncode, stdout=probe.stdout, stderr=probe.stderr))
        owners = sorted({int(x) for x in probe.stdout.split()})
        if probe.returncode not in (0, 1) or (probe.returncode == 0) != bool(owners):
            raise RuntimeError('Unconfirmed file resource ownership')
        return owners

    def resource_snapshot(self):
        state = dict(line.split('=', 1) for line in self.run(['systemctl', 'show', self.unit,
            '-p', 'MainPID', '-p', 'ControlPID', '-p', 'ActiveState', '-p', 'ControlGroup']).splitlines())
        group = Path('/sys/fs/cgroup') / state['ControlGroup'].lstrip('/')
        pids = set()
        if state['ControlGroup'] and group.exists():
            for p in [group / 'cgroup.procs', *group.rglob('cgroup.procs')]:
                if p.exists():
                    pids.update(map(int, p.read_text().split()))
        return dict(main_pid=int(state['MainPID']), control_pid=int(state['ControlPID']),
                    active_state=state['ActiveState'], owners=self.owners(), cgroup_pids=sorted(pids))

    def gates(self, target):
        pid = int(self.run(['systemctl', 'show', self.unit, '-p', 'MainPID', '--value']))
        assert self.current() == target and pid > 0 and self.owners() == [pid]
        assert self.run(['systemctl', 'is-active', self.unit]) == 'active'
        ready = json.loads((self.root / 'runtime-ready.json').read_bytes())
        assert ready == dict(pid=pid, release=target)
        assert Path(f'/proc/{pid}/exe').resolve() == Path('/usr/bin/python3').resolve()
        assert reviewed.manifest(self.root / 'store') == self.before['store_manifest']
        assert reviewed.manifest(Path(self.plan['backup_store'])) == self.before['store_manifest']
        return dict(ready=True, pid=pid, release=target, scope='FILE_ONLY; no Qwen/NPU/microphone/HTTP', store_unchanged=True)

    def switch(self, target):
        assert not self.owners()
        partial = self.root / 'current-release.tmp'
        partial.write_text(target + '\n'); partial.replace(self.root / 'current-release')
        with (self.root / 'switches.jsonl').open('a') as f:
            f.write(json.dumps(dict(pid=os.getpid(), target=target, monotonic=time.monotonic())) + '\n')
        scenario = (self.root / 'scenario').read_text().strip()
        if target == 'old' and scenario == 'rollback_crash' and not (self.root / 'recovery-crashed').exists():
            (self.root / 'recovery-crashed').write_text(str(os.getpid()))
            os._exit(23)  # PID 1 restarts the same supervisor with the original plan.
        if target == 'new' and scenario == 'worker_crash':
            os._exit(23)
        if target == 'new' and scenario == 'runner_crash':
            (self.root / 'worker-waiting').write_text('waiting for independent timer\n')
            time.sleep(300)  # Simulates stalled work; systemd recovery stops this worker.
        if target == 'new' and scenario in ('success', 'diagnostic_error'):
            time.sleep(17)  # Work longer than the submission timeout.

    def start(self):
        if self.current() == 'new' and (self.root / 'scenario').read_text().strip() in ('forward_failure', 'rollback_crash'):
            raise RuntimeError('Injected candidate startup failure')
        state = self.run(['systemctl', 'show', self.unit, '-p', 'LoadState', '--value'])
        if state == 'not-found':
            self.run(['systemd-run', '--no-block', '--unit=' + self.unit, '--property=Type=exec',
                      '/usr/bin/python3', str(self.root / 'runtime.py'), str(self.root)])
        else:
            super().start()

    def stop(self):
        # Fixture runtime is transient, unlike the permanent production unit.
        if self.run(['systemctl', 'show', self.unit, '-p', 'LoadState', '--value']) == 'not-found':
            sample = self.resource_snapshot()
            assert not sample['main_pid'] and not sample['control_pid'] and not sample['cgroup_pids'] and not sample['owners']
        else:
            super().stop()

    def non_actuating_checks(self):
        self.record('fixture-non-actuating', {'scope': 'FILE_ONLY', 'HTTP_commands': 0})
        return []


reviewed.Production = FileOnlyProduction
