#!/usr/bin/python3
"""One authorized deployment; independent systemd-timer recovery. No store writes."""
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import time
import urllib.request

# This is the incident controller preserved in the repository for review.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging.firefly_recovery import RecoveryTimeout, recover, wait_quiescent


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def manifest(base):
    out = {}
    for p in sorted([base, *base.rglob('*')]):
        s = p.lstat()
        assert stat.S_ISDIR(s.st_mode) or stat.S_ISREG(s.st_mode), p
        e = {'mode': stat.S_IMODE(s.st_mode), 'uid': s.st_uid, 'gid': s.st_gid,
             'mtime_ns': s.st_mtime_ns, 'type': 'dir' if p.is_dir() else 'file'}
        if p.is_file():
            e.update(sha256=sha(p), size=s.st_size)
        out[str(p.relative_to(base))] = e
    return out


def comparable(tree):
    return {p: {k: v for k, v in e.items() if k != 'mtime_ns'} for p, e in tree.items()}


class Production:
    def __init__(self, root):
        assert os.geteuid() == 0
        if not __debug__:
            raise RuntimeError('Safety assertions require Python without -O')
        info = root.stat()
        if info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o700:
            raise RuntimeError('Supervisor directory must be root-owned mode 0700')
        self.root = root
        self.plan = json.loads((root / 'plan.json').read_text())
        self.before = json.loads((root / 'production-before.json').read_text())
        self.prefix = Path('/opt/firefly/voice-assistant')
        self.unit = 'firefly-voice-assistant.service'
        self.env = {**os.environ, 'FIREFLY_OWNER_ACCEPTED': '1', 'PYTHONDONTWRITEBYTECODE': '1'}
        self.started = time.monotonic()
        if self.plan['boot_id'] != Path('/proc/sys/kernel/random/boot_id').read_text().strip():
            raise RuntimeError('Armed plan belongs to a different boot')
        self.recovery_deadline = self.plan['deadline_monotonic'] + self.plan['recovery_budget_seconds']
        self.operation_deadline = self.plan['deadline_monotonic']

    def record(self, name, value):
        target = self.root / 'evidence' / (name + '.json')
        partial = target.with_suffix('.tmp')
        partial.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        partial.replace(target)
        with (self.root / 'evidence' / 'observations.jsonl').open('a') as journal:
            journal.write(json.dumps({'utc': utc(), 'event': name, 'value': value}) + '\n')

    def run(self, argv, timeout=90):
        timeout = min(timeout, self.operation_deadline - time.monotonic())
        if timeout <= 0:
            raise RecoveryTimeout('Armed operation deadline expired')
        try:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=self.env)
        except subprocess.TimeoutExpired as error:
            self.record('command-timeout', {'argv': argv, 'error': str(error),
                        'stdout': repr(error.stdout), 'stderr': repr(error.stderr)})
            raise
        with (self.root / 'evidence' / 'commands.jsonl').open('a') as f:
            f.write(json.dumps({'utc': utc(), 'argv': argv, 'returncode': result.returncode,
                               'stdout': result.stdout, 'stderr': result.stderr}) + '\n')
        if result.returncode:
            raise RuntimeError(str(argv) + ': ' + result.stderr[-2000:])
        return result.stdout.strip()

    def get(self, route):
        timeout = min(3, self.operation_deadline - time.monotonic())
        if timeout <= 0:
            raise RecoveryTimeout('Armed HTTP verification deadline expired')
        with urllib.request.urlopen('http://127.0.0.1:8080' + route, timeout=timeout) as response:
            return json.load(response)

    def owner_probe(self):
        devices = ['/dev/dri/card1', '/dev/snd/pcmC3D0c']
        for device in devices:
            if not stat.S_ISCHR(Path(device).stat().st_mode):
                raise RuntimeError('Resource is not a character device: ' + device)
        timeout = min(5, self.operation_deadline - time.monotonic())
        if timeout <= 0:
            raise RecoveryTimeout('Armed probe deadline expired')
        try:
            result = subprocess.run(['fuser', *devices],
                                    capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.record('fuser-timeout', {'error': str(error),
                        'stdout': repr(error.stdout), 'stderr': repr(error.stderr)})
            raise
        self.record('fuser', {'returncode': result.returncode, 'stdout': result.stdout,
                             'stderr': result.stderr, 'devices': devices})
        owners = sorted({int(p) for p in result.stdout.split()})
        # Non-verbose fuser emits labels and access markers c/e/r/m on stderr
        # (e.g. RKLLM's mmap marker m). Everything else is a failed probe.
        labels = result.stderr
        for device in devices:
            labels = labels.replace(device + ':', '')
        if (result.returncode not in (0, 1)
                or any(c not in 'cerm' for c in labels if not c.isspace())
                or (result.returncode == 0) != bool(owners)):
            raise RuntimeError('Inconsistent or failed fuser probe')
        return owners

    def owners(self):
        return self.owner_probe()

    def resource_snapshot(self):
        if not Path('/sys/fs/cgroup/cgroup.controllers').is_file():
            raise RuntimeError('Unified cgroup v2 observation is required')
        output = self.run(['systemctl', 'show', self.unit, '-p', 'MainPID',
                           '-p', 'ControlPID', '-p', 'ActiveState', '-p', 'ControlGroup'])
        values = dict(line.split('=', 1) for line in output.splitlines())
        group = values['ControlGroup']
        pids = set()
        if group:
            if not group.startswith('/system.slice/') or '..' in group.split('/'):
                raise RuntimeError('Unexpected service cgroup: ' + group)
            base = Path('/sys/fs/cgroup') / group.lstrip('/')
            if base.exists():
                paths = [base / 'cgroup.procs', *base.rglob('cgroup.procs')]
                for path in paths:
                    try:
                        pids.update(int(pid) for pid in path.read_text().split())
                    except FileNotFoundError:
                        # Child cgroups may disappear as stop completes.
                        if path.parent.exists():
                            raise
        owners = self.owner_probe()
        processes = {}
        for pid in sorted(pids | set(owners)):
            proc = Path('/proc') / str(pid)
            try:
                processes[str(pid)] = {'stat': (proc / 'stat').read_text(),
                                      'cgroup': (proc / 'cgroup').read_text(),
                                      'exe': os.readlink(proc / 'exe')}
            except FileNotFoundError:
                processes[str(pid)] = {'state': 'exited-during-observation'}
        return {'main_pid': int(values['MainPID']), 'control_pid': int(values['ControlPID']),
                'active_state': values['ActiveState'], 'cgroup': group,
                'cgroup_pids': sorted(pids), 'owners': owners, 'processes': processes}

    def current(self):
        link = os.readlink(self.prefix / 'current')
        assert link in ('releases/' + self.plan['old'], 'releases/' + self.plan['new']), link
        return Path(link).name

    def stop(self):
        self.run(['systemctl', 'stop', self.unit], timeout=65)

    def free(self):
        return wait_quiescent(self.resource_snapshot, self.record,
                              min(time.monotonic() + 30, self.plan['deadline_monotonic']))

    def switch(self, target):
        # Explicit recorded rollback target avoids depending on a partially
        # written previous-release marker after an interrupted activation.
        self.run(['/usr/bin/python3', str(self.root / 'tools/scripts/stage-release.py'),
                  'activate', '--destination', str(self.prefix), '--release-id', target,
                  '--store', '/var/lib/firefly/store'], timeout=120)

    def start(self):
        self.run(['systemctl', 'start', self.unit], timeout=30)

    def gates(self, target):
        status = self.get('/api/status')
        assert status['ok'] and status['runtime_ready'] and status['stt_ready']
        assert status['microphone'] == 'connected' and status['voice'] == 'WAITING_WAKE'
        assert not status['stopping'] and not status['needs_apply'] and not status['runtime_error']
        assert not status['config_recovered'] and status['health']['overall'] == 'ready'
        for name in ('admin', 'configuration', 'runtime', 'llm', 'stt', 'wake'):
            assert status['health']['subsystems'][name]['status'] == 'ready', name
        assert status['active'] == self.before['status']['active']
        assert status['stored'] == self.before['status']['stored']
        assert not self.get('/api/setup/status')['active']
        assert self.run(['systemctl', 'is-active', self.unit]) == 'active'
        pid = int(self.run(['systemctl', 'show', self.unit, '-p', 'MainPID', '--value']))
        assert status['worker_pid'] == pid and self.owners() == [pid]
        release = self.prefix / 'releases' / target
        assert self.current() == target and Path(f'/proc/{pid}/exe').resolve() == release / 'bin/firefly-demo'
        assert sha(f'/proc/{pid}/exe') == self.plan['binary_sha256'][target]
        identity = json.loads(self.run([str(release / 'bin/firefly-demo'), '--build-identity']))
        assert identity == self.plan['identities'][target] and not identity['dirty']
        assert status['build']['head'] == identity['head']
        assert status['build']['source_sha256'] == identity['source_sha256']
        argv = Path(f'/proc/{pid}/cmdline').read_bytes().decode().split('\0')
        assert argv == self.before['argv'], 'Production flags changed'
        maps = Path(f'/proc/{pid}/maps').read_text()
        for library in ('librkllmrt.so', 'libsherpa-onnx-c-api.so', 'libonnxruntime.so'):
            lines = [line for line in maps.splitlines() if library in line]
            assert lines and all(str(release / 'lib' / library) in line for line in lines), library
        assert sha('/etc/systemd/system/firefly-voice-assistant.service') == self.before['unit_sha256']
        after = manifest(Path('/var/lib/firefly/store'))
        assert comparable(after) == comparable(self.before['store_manifest']), 'ConfigStore bytes/owner/mode changed'
        if not (self.root / 'CUTOVER_STARTED').exists():
            assert after == self.before['store_manifest'], 'Store timestamps changed during preparation'
            assert manifest(Path(self.plan['backup_store'])) == self.before['store_manifest'], 'Backup changed'
        changed_times = [p for p in after if after[p]['mtime_ns'] != self.before['store_manifest'][p]['mtime_ns']]
        result = {'utc': utc(), 'pid': pid, 'release': str(release), 'identity': identity,
                  'status': status, 'store_manifest': after, 'store_bytes_owners_modes_unchanged': True,
                  'store_timestamp_changes': changed_times, 'sole_npu_microphone_owner': pid,
                  'argv_unchanged': True, 'library_maps': maps, 'unit_unchanged': True}
        return result

    def wait_ready(self, target, seconds):
        deadline = min(self.operation_deadline, time.monotonic() + seconds)
        last = None
        while time.monotonic() < deadline:
            try:
                return self.gates(target)
            except Exception as error:
                last = str(error)
                self.record('readiness-failure', {'error': last, 'target': target})
                time.sleep(min(.5, max(0, deadline - time.monotonic())))
        raise RuntimeError('Readiness deadline: ' + str(last))

    def non_actuating_checks(self):
        history = self.get('/api/executions')
        assert not history['executions'], 'Unexpected device execution after startup'
        replies = []
        for text in ('какая погода', 'включи лампу в неизвестной комнате'):
            body = {'text': text, 'execute': False, 'production': False}
            request = urllib.request.Request('http://127.0.0.1:8080/api/test-command',
                data=json.dumps(body, ensure_ascii=False).encode(), headers={'Content-Type': 'application/json'})
            timeout = min(40, self.operation_deadline - time.monotonic())
            if timeout <= 0:
                raise RecoveryTimeout('Armed diagnostic deadline expired')
            with urllib.request.urlopen(request, timeout=timeout) as response:
                reply = json.load(response)
            replies.append({'request': body, 'response': reply, 'utc': utc()})
            self.record('non-actuating-checks', replies)
            assert reply['ok'] and reply['result'].get('unsupported') is True, reply
            assert reply.get('execution') is None, reply
            assert self.get('/api/executions') == history, 'Dry diagnostic entered execution history'
        return replies

    def rollback(self, reason):
        # Terminate only our transient cutover worker before recovery, avoiding
        # a race with an activation/start left running after agent failure.
        if not (self.root / 'CUTOVER_STARTED').exists():
            self.record('aborted-before-cutover', {'utc': utc(), 'reason': reason,
                         'service_stop_performed': False})
            return
        # A restarted supervisor cannot reset or extend the original deadline.
        if time.monotonic() >= self.recovery_deadline:
            raise RecoveryTimeout('Recovery deadline already expired; no mutation permitted')
        self.operation_deadline = self.recovery_deadline
        worker = self.plan['worker_unit']
        output = self.run(['systemctl', 'show', worker, '-p', 'LoadState', '-p', 'ActiveState',
                           '-p', 'MainPID', '-p', 'ControlPID', '-p', 'ControlGroup'])
        state = dict(line.split('=', 1) for line in output.splitlines())
        # A completed transient worker may have been garbage-collected before
        # recovery re-entry. Do not fail restoration on stopping a missing unit.
        if state['LoadState'] == 'not-found':
            if (state['ActiveState'] != 'inactive' or state['MainPID'] != '0'
                    or state['ControlPID'] != '0' or state['ControlGroup']):
                raise RuntimeError('Inconsistent missing cutover worker state')
        else:
            self.run(['systemctl', 'stop', worker], timeout=20)
        with (self.root / 'operation.lock').open('a') as lock:
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= self.recovery_deadline:
                        raise RecoveryTimeout('Recovery lock deadline expired')
                    time.sleep(.1)
            self.restore(reason)

    def restore(self, reason):
        if (self.root / 'ACCEPTED').exists():
            try:
                self.record('accepted-rechecked', self.gates(self.plan['new']))
                return
            except Exception:
                pass
        result = recover(self, self.plan['old'], self.recovery_deadline)
        result.update(reason=reason, result='ROLLED_BACK', finished_utc=utc())
        self.record('rollback-result', result)


class Fixture:
    """Same recovery algorithm; local state only, no subprocess/device access."""
    def __init__(self, current='new', running=True):
        self.release, self.running, self.events = current, running, []
    def stop(self):
        self.events.append('stop'); self.running = False
    def record(self, name, value):
        pass
    def resource_snapshot(self):
        return {'main_pid': 0, 'control_pid': 0, 'active_state': 'inactive',
                'owners': [] if not self.running else [123], 'cgroup_pids': []}
    def gates(self, target):
        return self.wait_ready(target, 1)
    def current(self):
        return self.release
    def switch(self, target):
        assert not self.running; self.release = target; self.events.append('atomic-switch-' + target)
    def start(self):
        self.running = True; self.events.append('start')
    def wait_ready(self, target, seconds):
        assert self.running and self.release == target
        self.events.append('verify-store-health-ownership')
        return {'release': target, 'ready': True, 'events': self.events}


def main():
    os.umask(0o077)
    mode, root_arg = sys.argv[1:]
    root = Path(root_arg)
    if mode == 'self-test':
        cases = {}
        for name, current, running in [('candidate_start_failure', 'new', False),
                                       ('agent_crash', 'new', True), ('watchdog_timeout', 'new', True),
                                       ('stop_before_activation', 'old', False)]:
            ops = Fixture(current, running); result = recover(ops, 'old', time.monotonic() + 5)
            assert result['ready'] and result['release'] == 'old'
            cases[name] = result
        (root / 'watchdog-self-tests.json').write_text(json.dumps(cases, indent=2) + '\n')
        return
    if mode == 'timer-proof':
        ops = Fixture(); result = recover(ops, 'old', time.monotonic() + 5)
        result.update(pid=os.getpid(), ppid=os.getppid(), utc=utc(), scope='fixture-only; no production operations')
        (root / 'timer-proof-result.json').write_text(json.dumps(result, indent=2) + '\n')
        return
    ops = Production(root)
    if mode == 'preflight':
        ops.record('preflight-gates', ops.gates(ops.plan['old']))
        return
    if mode == 'rollback':
        ops.rollback('independent watchdog or explicit worker failure')
        return
    assert mode == 'deploy'
    lock = (root / 'operation.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX)
    try:
        assert not (root / 'CUTOVER_STARTED').exists(), 'One attempt already consumed'
        ops.record('immediate-preflight', ops.gates(ops.plan['old']))
        assert ops.run(['systemctl', 'is-active', ops.plan['rollback_timer']]) == 'active'
        assert time.monotonic() < ops.plan['deadline_monotonic'] - 180
        (root / 'CUTOVER_STARTED').write_text(utc() + '\n')
        ops.stop()
        ops.free()
        ops.record('resources-released', {'utc': utc(), 'owners': ops.owners()})
        ops.switch(ops.plan['new'])
        ops.free()
        ops.start()
        ready = ops.wait_ready(ops.plan['new'], 100)
        ops.record('first-ready', ready)
        ops.non_actuating_checks()
        final = ops.wait_ready(ops.plan['new'], 15)
        final.update(result='DEPLOYED_VERIFIED', cutover_start=(root / 'CUTOVER_STARTED').read_text().strip())
        ops.record('deployed-verified', final)
        # Marker is written only after every mandatory gate; timer stays armed
        # until the agent independently reads and verifies this evidence.
        (root / 'ACCEPTED').write_text(utc() + '\n')
    except BaseException as error:
        ops.record('deployment-error', {'utc': utc(), 'error': repr(error)})
        ops.run(['systemctl', 'start', '--no-block', ops.plan['rollback_unit']])
        raise


if __name__ == '__main__':
    try:
        main()
    except RecoveryTimeout as error:
        print(str(error), file=sys.stderr)
        sys.exit(78)  # RestartPreventExitStatus=78 for the transient supervisor.
