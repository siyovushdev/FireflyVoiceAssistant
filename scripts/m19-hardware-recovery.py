#!/usr/bin/python3
"""Single private hardware window; never deploys a release or rewrites a store.

check is read-only. All other modes require a separately authorized, fresh plan.
Reuses the reviewed rollout resource probes and production health gates.
"""
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import stat
import sys
import time

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('rollout', Path(__file__).with_name('production-rollout.py'))
rollout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rollout)
from packaging.firefly_recovery import quiescent


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def mock_remaining_seconds(started_us, runtime_us, now):
    require(type(started_us) is int and started_us > 0 and type(runtime_us) is int
            and runtime_us > 0, 'Unknown mock lifetime')
    remaining = (started_us + runtime_us) / 1_000_000 - now
    # Entire 10-minute trial + 6-minute recovery + 2-minute preflight margin.
    require(remaining >= 1080, 'Mock cannot cover the complete hardware window')
    return remaining


class Window(rollout.Production):
    def __init__(self, root):
        require(os.geteuid() == 0 and __debug__, 'Root and enabled assertions required')
        require(root.is_absolute() and root.resolve() == root, 'Noncanonical plan')
        for path in [root, *root.parents]:
            info = path.stat()
            require(info.st_uid == 0 and not info.st_mode & 0o022, 'Untrusted plan parent')
        require(stat.S_IMODE(root.stat().st_mode) == 0o700, 'Plan must be mode 0700')
        self.root = root
        self.plan = json.loads((root / 'plan.json').read_text())
        self.before = json.loads((root / 'production-before.json').read_text())
        self.prefix = Path('/opt/firefly/voice-assistant')
        self.unit = 'firefly-voice-assistant.service'
        self.env = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'PYTHONDONTWRITEBYTECODE': '1'}
        self.operation_deadline = time.monotonic() + 60
        require(self.plan['boot_id'] == Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'Boot changed')
        for name, digest in self.plan['supervisor_files'].items():
            path = root / name
            require(path.stat().st_uid == 0 and not path.stat().st_mode & 0o022, 'Untrusted supervisor file')
            require(rollout.sha(path) == digest, 'Supervisor bytes changed: ' + name)

    def identity(self):
        require(self.current() == self.plan['old'], 'Release pointer changed; manual recovery required')
        require(rollout.sha('/etc/systemd/system/firefly-voice-assistant.service') == self.before['unit_sha256'], 'Production unit changed')
        require(rollout.comparable(rollout.manifest(Path('/var/lib/firefly/store'))) ==
                rollout.comparable(self.before['store_manifest']), 'Production store changed')
        require(rollout.sha(self.prefix / 'releases' / self.plan['old'] / 'bin/firefly-demo') ==
                self.plan['binary_sha256'][self.plan['old']], 'Recovery binary changed')

    def candidate(self):
        for name, digest in self.plan['candidate_files'].items():
            require(rollout.sha(name) == digest, 'Candidate changed: ' + name)
        for entry in self.plan['dependencies']:
            info = Path(entry['path']).stat()
            require((info.st_size, info.st_mtime_ns, info.st_ino, info.st_dev) ==
                    (entry['size'], entry['mtime_ns'], entry['inode'], entry['device']),
                    'Verified dependency changed: ' + entry['path'])

    def endpoints(self):
        # Bind-only availability check; never send a request to another listener.
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', self.plan['admin_port']))
        require(self.run(['systemctl', 'is-active', self.plan['mock_unit']]) == 'active', 'Prepared mock expired')
        output = self.run(['systemctl', 'show', self.plan['mock_unit'], '-p', 'MainPID', '--value'])
        argv = (Path('/proc') / output / 'cmdline').read_bytes().decode().split('\0')
        require(argv == self.plan['mock_argv'], 'Mock identity changed')
        identity = self.plan['mock_identity']
        require(int(output) == identity['pid'], 'Mock PID changed')
        ticks = (Path('/proc') / output / 'stat').read_text().rsplit(')', 1)[1].split()[19]
        require(ticks == identity['start_ticks'], 'Mock process identity changed')
        properties = self.systemd_properties(self.plan['mock_unit'], 'Service')
        started = rollout.property_value(properties, 'ExecMainStartTimestampMonotonic', 't')
        runtime = rollout.property_value(properties, 'RuntimeMaxUSec', 't')
        require(started == identity['started_us'] and runtime == identity['runtime_us'], 'Mock service lifetime changed')
        mock_remaining_seconds(started, runtime, time.monotonic())
        require(Path(self.plan['mock_log']).read_bytes() == b'', 'Mock log is no longer fresh')
        require(os.readlink(Path(self.plan['candidate_binary']).parent.parent / 'share') ==
                str(self.prefix / 'releases' / self.plan['old'] / 'share'), 'Wake data link changed')

    def snapshot(self, unit):
        original = self.unit
        try:
            self.unit = unit
            return self.resource_snapshot()
        finally:
            self.unit = original

    def barrier(self):
        def probe():
            test = self.snapshot(self.plan['test_unit'])
            prod = self.snapshot('firefly-voice-assistant.service')
            self.record('both-services', {'test': test, 'production': prod})
            if not quiescent(prod):
                test['error'] = 'Production not quiescent'
            return test
        return rollout.wait_quiescent(probe, self.record, min(self.operation_deadline, time.monotonic() + 30))

    def pc_browser_gate(self):
        if self.plan.get('profile') == 'ADMIN4096_BOUNDED_STRESS':
            gate = self.plan['stress_gate']
            require(gate['boot_id'] == self.plan['boot_id'] and
                    0 <= time.monotonic() - gate['checked_monotonic'] < 300,
                    'Stress gate expired before cutover')
            browser_proof = json.loads(Path(self.plan['browser_gate']['proof_path']).read_text())
            require(gate['offline_root'] == browser_proof['remoteRoot'], 'Stress/browser backend mismatch')
            proof_path = Path(gate['proof_path'])
            require(proof_path == self.root / 'evidence/admin-stress-offline.json' and
                    proof_path.resolve() == proof_path and proof_path.stat().st_uid == 0 and
                    not proof_path.stat().st_mode & 0o077 and rollout.sha(proof_path) == gate['proof_sha256'],
                    'Frozen offline stress proof changed')
            require(rollout.sha(self.root / 'scripts/admin-stress-client.py') == gate['client_sha256'] ==
                    self.plan['supervisor_files']['scripts/admin-stress-client.py'], 'Stress client not offline-qualified')
            proof = json.loads(proof_path.read_text())
            require(proof['passed'] is True and proof['real_rkllm'] is False and len(proof['cycles']) == 2 and
                    all(c['restored'] is True and [(x['case'],x['input_bytes']) for x in c['cases']] ==
                        [(x['case'],x['bytes']) for x in gate['workload']] for c in proof['cycles']),
                    'Incomplete frozen stress proof')
        if not self.plan.get('pc_lease_token'):
            return
        gate = self.plan['browser_gate']
        require(gate['boot_id'] == self.plan['boot_id'] and
                0 <= time.monotonic() - gate['checked_monotonic'] < 300, 'PC browser gate expired before cutover')
        require(rollout.sha(gate['proof_path']) == gate['proof_sha256'], 'PC browser proof changed')

    def pc_connection(self):
        if not self.plan.get('pc_lease_token'):
            return  # Preserved historical plans/tests do not use PC transport.
        try:
            lease = json.loads((self.root / 'PC_HEARTBEAT.json').read_text())
        except (OSError, ValueError) as error:
            raise RuntimeError('PC/SSH heartbeat missing or invalid') from error
        require(lease['token'] == self.plan['pc_lease_token'] and lease['boot_id'] == self.plan['boot_id'],
                'PC lease identity changed')
        require(0 <= time.monotonic() - lease['monotonic'] < self.plan['pc_heartbeat_timeout_seconds'],
                'PC/SSH heartbeat lost')

    def armed(self):
        arm = json.loads((self.root / 'ARMED.json').read_text())
        require(arm['boot_id'] == self.plan['boot_id'], 'Arm boot changed')
        require(arm['plan_sha256'] == rollout.sha(self.root / 'plan.json'), 'Armed plan changed')
        require(arm['deadline'] > arm['armed'] and arm['deadline'] - arm['armed'] <= 601, 'Invalid window')
        return arm

    def verify_units(self):
        for name, digest in self.plan['units'].items():
            output = self.run(['systemctl', 'show', name, '-p', 'FragmentPath', '-p', 'DropInPaths', '-p', 'LoadState'])
            props = dict(line.split('=', 1) for line in output.splitlines())
            require(props['LoadState'] == 'loaded' and not props['DropInPaths'], 'Unit not loaded cleanly: ' + name)
            require(Path(props['FragmentPath']).resolve() == self.root / 'units' / name, 'Wrong unit fragment')
            require(rollout.sha(props['FragmentPath']) == digest, 'Unit bytes changed')

    def arm(self):
        require(not (self.root / 'ARMED.json').exists(), 'One arm only')
        require(not (self.root / 'CLOSED').exists(), 'Window consumed')
        self.identity()
        self.candidate()
        self.endpoints()
        self.pc_connection()
        self.pc_browser_gate()
        self.gates(self.plan['old'])
        self.verify_units()
        value = rollout.property_value
        timer = self.systemd_properties(self.plan['timer_unit'], 'Timer')
        unit = self.systemd_properties(self.plan['timer_unit'], 'Unit')
        service = self.systemd_properties(self.plan['recovery_unit'], 'Unit')
        require(value(unit, 'ActiveState', 's') == 'active' and value(unit, 'SubState', 's') == 'waiting', 'Timer not waiting')
        require(value(service, 'ActiveState', 's') == 'inactive', 'Recovery already running')
        require(value(timer, 'Unit', 's') == self.plan['recovery_unit'], 'Wrong timer target')
        deadline = value(timer, 'NextElapseUSecMonotonic', 't') / 1_000_000
        now = time.monotonic()
        require(540 < deadline - now <= 601, 'Insufficient fresh window')
        arm = {'boot_id': self.plan['boot_id'], 'armed': now, 'deadline': deadline,
               'plan_sha256': rollout.sha(self.root / 'plan.json')}
        with (self.root / 'ARMED.json').open('x') as target:
            json.dump(arm, target)
        self.record('armed', arm)

    def cutover(self):
        arm = self.armed()
        require(time.monotonic() < arm['deadline'] - 480, 'Cutover window expired')
        require(not (self.root / 'CLOSED').exists(), 'Window consumed')
        self.verify_units()
        require(self.run(['systemctl', 'is-active', self.plan['timer_unit']]) == 'active', 'Timer inactive')
        self.identity()
        self.candidate()
        self.endpoints()
        self.pc_connection()
        self.pc_browser_gate()
        self.gates(self.plan['old'])
        self.pc_connection()
        self.pc_browser_gate()
        with (self.root / 'CUTOVER_STARTED').open('x') as marker:
            marker.write(rollout.utc())
        self.operation_deadline = arm['deadline']
        self.stop()  # Only the original production service, once, after owner START.
        self.barrier()
        self.run(['systemctl', 'start', '--no-block', self.plan['test_unit']], timeout=5)

    def permit(self):
        arm = self.armed()
        require(not (self.root / 'CLOSED').exists(), 'Window closed')
        require((self.root / 'CUTOVER_STARTED').exists(), 'Cutover not admitted')
        require(time.monotonic() < arm['deadline'] - 120, 'Test startup too late')
        self.identity()
        self.candidate()
        # ExecCondition runs inside the starting test cgroup: check production
        # and all device owners, not the test's own condition process.
        rollout.wait_quiescent(self.resource_snapshot, self.record, time.monotonic() + 15)
        self.pc_connection()
        self.pc_browser_gate()
        with (self.root / 'TEST_STARTED').open('x') as marker:
            marker.write(rollout.utc())

    def confirm_original_process(self, pid):
        proc = Path('/proc') / str(pid)
        expected = self.prefix / 'releases' / self.plan['old'] / 'bin/firefly-demo'
        require((proc / 'exe').resolve() == expected, 'Unknown production executable')
        require(rollout.sha(proc / 'exe') == self.plan['binary_sha256'][self.plan['old']], 'Unknown production binary')
        require((proc / 'cmdline').read_bytes().decode().split('\0') == self.before['argv'], 'Unknown production argv')

    def recovery(self):
        arm = self.armed()
        # An early failure gets six minutes from the FIRST recovery entry,
        # including any systemd restart, not six minutes after the trial timer.
        started_file = self.root / 'RECOVERY_STARTED.json'
        now = time.monotonic()
        if not started_file.exists():
            partial = started_file.with_suffix('.tmp')
            partial.write_text(json.dumps(now))
            partial.replace(started_file)  # Persist before any service operation.
        started = json.loads(started_file.read_text())
        require(type(started) in (int, float) and 0 <= started <= now, 'Invalid recovery start')
        self.operation_deadline = min(arm['deadline'] + 360, started + 360)
        (self.root / 'CLOSED').touch(exist_ok=True)
        while time.monotonic() < self.operation_deadline:
            try:
                # Browser/audio are part of this window too: stop their entire
                # unit before restoring production, even if the launcher died.
                for helper in ('audio_unit', 'browser_unit'):
                    if self.plan.get(helper):
                        self.run(['systemctl', 'stop', self.plan[helper]], timeout=15)
                # Always cancel this test's pending start and stop its whole cgroup.
                self.run(['systemctl', 'stop', self.plan['test_unit']], timeout=65)
                self.identity()
                try:
                    result = self.gates(self.plan['old'])
                except Exception:
                    require((self.root / 'CUTOVER_STARTED').exists(), 'No authorized cutover')
                    attempts_file = self.root / 'RESTORE_STARTS.json'
                    attempts = json.loads(attempts_file.read_text()) if attempts_file.exists() else 0
                    require(type(attempts) is int and 0 <= attempts < 2, 'Two restoration starts exhausted')
                    # A failed init may leave the original process active but
                    # degraded. Retry only that verified original service, once.
                    pid = int(self.run(['systemctl', 'show', self.unit, '-p', 'MainPID', '--value']))
                    if pid:
                        self.confirm_original_process(pid)
                        self.record('retry-original-process', {'pid': pid, 'previous_starts': attempts})
                        self.stop()
                    self.barrier()
                    # Persist BEFORE starting so a supervisor crash cannot add
                    # unbounded restore attempts. Never rerun the candidate.
                    partial = attempts_file.with_suffix('.tmp')
                    partial.write_text(json.dumps(attempts + 1))
                    partial.replace(attempts_file)
                    self.start()
                    result = self.wait_ready(self.plan['old'], 120)
                self.record('RECOVERY_CHECK_PASS', result)
                return
            except Exception as error:
                self.record('recovery-failure', {'error': repr(error)})
                time.sleep(min(2, max(0, self.operation_deadline - time.monotonic())))
        raise rollout.RecoveryTimeout('Recovery deadline exhausted; fail closed, no foreign PID killed')

    def watch(self):
        """Read-only sampling; aborts the single attempt through recovery on failure."""
        arm = self.armed()
        self.operation_deadline = arm['deadline']
        log = Path(self.plan['test_log'])
        offset = 0
        began = time.monotonic()
        ready_seen = False
        pending = ''
        try:
            while time.monotonic() < arm['deadline']:
                require(not (self.root / 'CLOSED').exists(), 'Window closing')
                self.pc_connection()
                output = self.run(['systemctl', 'show', self.plan['test_unit'], '-p', 'MainPID', '-p', 'ControlGroup'], timeout=3)
                properties = dict(line.split('=', 1) for line in output.splitlines())
                pid = int(properties['MainPID'])
                require(pid > 0, 'Test exited')
                proc = Path('/proc') / str(pid)
                require((proc / 'exe').resolve() == Path(self.plan['candidate_binary']), 'Unexpected test executable')
                mem = Path('/proc/meminfo').read_text()
                available = int(re.search(r'^MemAvailable:\s+(\d+)', mem, re.M)[1])
                group = Path('/sys/fs/cgroup') / properties['ControlGroup'].lstrip('/')
                events = dict(line.split() for line in (group / 'memory.events').read_text().splitlines())
                self.record('memory-sample', {'monotonic': time.monotonic(), 'available_kib': available,
                    'pid': pid, 'pss': (proc / 'smaps_rollup').read_text(),
                    'cgroup_current': (group / 'memory.current').read_text(), 'events': events})
                # MemAvailable during load/reclaim is diagnostic, not a new
                # admission policy. The product enforces its unchanged 6/1 GiB
                # at the logged checkpoints below; actual OOM events abort now.
                require(all(int(events.get(k, 0)) == 0 for k in ('oom', 'oom_kill', 'max')), 'Cgroup allocation/OOM event')
                with log.open() as stream:
                    stream.seek(offset)
                    chunk = stream.read(1048576)
                    offset = stream.tell()
                pending += chunk
                lines = pending.split('\n')
                pending = lines.pop()  # A marker can straddle two reads.
                ready_seen = ready_seen or any('WAITING_WAKE' in line for line in lines)
                require(ready_seen or time.monotonic() - began < 120, 'Initial voice READY deadline')
                for line in lines:
                    require(not any(code in line for code in (
                        'admin_load_failed', 'admin_inference_failed', 'runtime_load_failed',
                        'destroy_failed', 'release_timeout', 'restore_failed', 'memory_status_unavailable',
                        'admin_memory_reserve_unavailable', 'rkllm_owner_busy', 'audio_drain_timeout',
                        'admin_resource_timeout', 'VOICE_RECOVERY failure reason=')), 'Failure logged: ' + line)
                    if 'SETUP_MEMORY phase=' in line and 'required_kib=' in line:
                        fields = dict(part.split('=', 1) for part in line.split() if '=' in part)
                        required = int(fields['required_kib'])
                        require(min(int(fields['available_kib']), int(fields['cgroup_headroom_kib'])) >= required,
                                'Phase admission rejected: ' + line)
                time.sleep(1)
        except Exception as error:
            self.record('TEST_ABORT', {'reason': repr(error)})
        # No retries of the candidate. Independent recovery owns test shutdown.
        self.operation_deadline = time.monotonic() + 5
        self.run(['systemctl', 'start', '--no-block', self.plan['recovery_unit']], timeout=5)


def main():
    require(len(sys.argv) == 3 and sys.argv[1] in ('check', 'arm', 'cutover', 'permit', 'recover', 'watch'), 'Usage: MODE ROOT')
    mode, directory = sys.argv[1:]
    window = Window(Path(directory))
    if mode == 'check':
        window.identity()
        window.candidate()
        window.endpoints()
        window.record('preparation-check', window.gates(window.plan['old']))
        print('PREPARATION_CHECK_PASS (no service mutation)')
        return
    # Permit runs as ExecCondition while cutover may still be submitting the job.
    if mode == 'permit':
        window.permit()
        return
    if mode == 'watch':
        window.watch()
        return
    with (window.root / 'operation.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        {'arm': window.arm, 'cutover': window.cutover, 'recover': window.recovery}[mode]()


if __name__ == '__main__':
    try:
        main()
    except rollout.RecoveryTimeout as error:
        print(str(error), file=sys.stderr)
        sys.exit(255 if len(sys.argv) > 1 and sys.argv[1] == 'permit' else 78)
    except BlockingIOError as error:
        # An in-flight cutover owns the lock; systemd may retry recovery.
        print(str(error), file=sys.stderr)
        sys.exit(1)
    except Exception as error:
        print(str(error), file=sys.stderr)
        mode = sys.argv[1] if len(sys.argv) > 1 else ''
        # Unknown/unarmed recovery state must not cause endless restart. A
        # rejected ExecCondition must fail the test unit and trigger OnFailure.
        sys.exit(78 if mode == 'recover' else 255 if mode == 'permit' else 1)
