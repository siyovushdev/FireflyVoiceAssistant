#!/usr/bin/env python3
"""Real DNS + Mosquitto, direct and actual HttpServer execution, never hardware."""
import argparse
import concurrent.futures
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def children(pid):
    found = set()
    for task in Path('/proc', str(pid), 'task').iterdir():
        try:
            found.update(int(x) for x in (task / 'children').read_text().split())
        except FileNotFoundError:
            pass
    return found


def resources(pid):
    return [len(list(Path('/proc', str(pid), name).iterdir())) for name in ('fd', 'task')]


class Runtime:
    def __init__(self, args, directory, config, http):
        self.http, self.directory = http, directory
        directory.mkdir()
        configuration = directory / 'config.json'
        configuration.write_text(json.dumps(config))
        self.log = (directory / 'server.log').open('w')
        if http:
            for name in ('store', 'credentials'):
                (directory / name).mkdir(mode=0o700)
            command = [args.http, str(directory / 'store'), str(directory / 'credentials'),
                       str(configuration), str(directory / 'audit.jsonl')]
        else:
            command = [args.direct, str(configuration)]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True)
        assert select.select([self.process.stdout], [], [], 4)[0], 'runtime startup deadline'
        first = self.process.stdout.readline()
        if http:
            assert first.startswith('ADMIN_LISTEN '), first
            self.url = first.split()[-1]
            self.hash = self.request('GET', '/api/config/active')[1]['active']['hash']
        else:
            assert json.loads(first)['ready']

    def request(self, method, path, body=None):
        request = urllib.request.Request(self.url + path, method=method,
            data=None if body is None else json.dumps(body).encode(),
            headers={'X-Firefly-Admin': '1', 'Content-Type': 'application/json'})
        try:
            response = urllib.request.urlopen(request, timeout=4)
        except urllib.error.HTTPError as error:
            response = error
        return response.status, json.loads(response.read())

    def direct(self, command):
        self.process.stdin.write(json.dumps(command) + '\n')
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 4)[0], 'direct execution deadline'
        return json.loads(self.process.stdout.readline())

    def execute(self, fast=False):
        target = 'device_lamp' if fast else 'device_fan'
        start = time.monotonic()
        if self.http:
            status, reply = self.request('POST', '/api/devices/command', {
                'text': 'включи сигнальную лампу' if fast else 'включи вентилятор',
                'expected_hash': self.hash,
                'expected_action': {'target_id': target, 'action_id': 'action_on', 'arguments': {}}})
            result = reply.get('execution') or {}
            state = self.request('GET', '/api/devices/state')[1]['devices']
            state = state[target] if isinstance(state, dict) else next(x for x in state if x.get('targetId', x.get('id')) == target)
            result['knowledge'] = state['properties']['power']['knowledge']
        else:
            result = self.direct({'target': target})
            status = None
        return {'http': self.http, 'fast': fast, 'status': status, 'result': result,
                'ms': (time.monotonic() - start) * 1000}

    def hashes(self):
        return {str(p.relative_to(self.directory / 'store')): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (self.directory / 'store').rglob('*') if p.is_file()}

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=3)
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()


def configuration(root, port, host):
    config = json.loads((root / 'tests/admin/v2/system.json').read_text())
    config['connections'] = []
    config['bindings'] = []
    config['observations'] = []
    for name, address, target in [('slow', host, 'device_fan'), ('fast', '127.0.0.1', 'device_lamp')]:
        config['connections'].append({'id': name, 'name': name, 'enabled': True, 'type': 'mqtt',
            'timeout_ms': 100, 'network': {'host': address, 'port': port}, 'mqtt': {'tls': False}})
        config['bindings'].append({'target_id': target, 'action_id': 'action_on', 'executor': 'mqtt',
            'connection_id': name, 'mqtt': {'topic': 'cmd/' + name, 'payload': 'ON', 'qos': 1, 'retain': False}})
    return config


def inside(args):
    # Checked before any mount: --inside alone cannot mutate the host resolver.
    assert os.getuid() == 0 and os.readlink('/proc/self/ns/net') != args.host_net
    assert os.readlink('/proc/self/ns/mnt') != args.host_mount
    subprocess.run(['mount', '--make-rprivate', '/'], check=True)
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    # Adopt only children orphaned by this fixture's cancellation cases and reap
    # the PDEATHSIG-killed worker. No global or foreign process operations.
    assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0  # PR_SET_CHILD_SUBREAPER
    root = Path(args.root)
    spec = importlib.util.spec_from_file_location('probe_dns', root / 'tests/admin/MqttProbeDnsIsolation.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='firefly-execution-dns-') as temporary:
        work = Path(temporary)
        resolver = work / 'resolv.conf'
        resolver.write_text('nameserver 127.0.0.1\noptions timeout:2 attempts:1\n')
        subprocess.run(['mount', '--bind', str(resolver), '/etc/resolv.conf'], check=True)
        dns = module.Dns()
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        settings = work / 'broker.conf'
        settings.write_text(f'listener {port} 127.0.0.1\nuser root\nallow_anonymous true\npersistence false\nlog_type all\n')
        log = (work / 'broker.log').open('w')
        broker = subprocess.Popen([args.mosquitto, '-c', str(settings)], stdout=log, stderr=log)
        results, cancellations = [], []
        try:
            deadline = time.monotonic() + 4
            while True:
                assert broker.poll() is None
                try:
                    with socket.create_connection(('127.0.0.1', port), .1):
                        break
                except OSError:
                    assert time.monotonic() < deadline
                    time.sleep(.02)
            for http in (False, True):
                for label, host in [('delayed', 'probe-delay.review.invalid'), ('never', 'probe-never.review.invalid')]:
                    runtime = Runtime(args, work / f'{http}-{label}', configuration(root, port, host), http)
                    try:
                        initial = runtime.hashes()
                        time.sleep(.1)
                        baseline = resources(runtime.process.pid)
                        for _ in range(3):
                            failed = runtime.execute()
                            assert failed['result'].get('success') is False, failed
                            assert failed['result'].get('error') == 'mqtt_timeout', failed
                            assert 95 <= failed['ms'] < 450, failed
                            assert failed['result']['knowledge'] == 'unknown', failed
                            assert not children(runtime.process.pid), 'worker must be reaped before response'
                            results.append(failed)
                            success = runtime.execute(fast=True)
                            assert success['result'].get('success') is True and success['ms'] < 450, success
                            assert success['result']['knowledge'] == 'predicted', success
                            assert not children(runtime.process.pid)
                            results.append(success)
                        time.sleep(.1)
                        assert resources(runtime.process.pid) == baseline
                        assert runtime.hashes() == initial
                    finally:
                        runtime.close()
                # Cancel while DNS is truly in progress. No detached resolver or
                # publish client may survive parent death and later send a command.
                runtime = Runtime(args, work / f'cancel-{http}', configuration(root, port, 'probe-delay.review.invalid'), http)
                try:
                    dns.arrived.clear()
                    with concurrent.futures.ThreadPoolExecutor(1) as pool:
                        pending = pool.submit(runtime.execute)
                        assert dns.arrived.wait(2)
                        pid = next(iter(children(runtime.process.pid)))
                        runtime.process.kill()
                        runtime.process.wait(timeout=3)
                        end = time.monotonic() + 2
                        while True:
                            reaped, status = os.waitpid(pid, os.WNOHANG)
                            if reaped:
                                assert os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL
                                break
                            assert time.monotonic() < end
                            time.sleep(.01)
                        try:
                            pending.result()
                        except (Exception, AssertionError):
                            pass  # Killed parent cannot provide an execution response.
                        cancellations.append({'http': http, 'worker_reaped_signal': 9})
                finally:
                    runtime.close()
            # A stopped child between CONNECT and publish response must not
            # hold the shared observation mutex across IPC and delay another
            # target beyond its own budget. Only the slow branch is injected;
            # the fast branch execs the untouched real worker before reading IPC.
            stage = work / 'ipc-stall-helper'
            stage.mkdir()
            direct = stage / 'direct'
            shutil.copy2(args.direct, direct)
            marker = stage / 'stall-ready'
            original = Path(args.direct).parent / 'firefly-mqtt-publish-worker'
            wrapper = stage / original.name
            wrapper.write_text("#!/usr/bin/python3\n" +
                "import socket,struct,json,time,pathlib,os\n" +
                "s=socket.socket(fileno=0);s.settimeout(3)\n" +
                "while True:\n" +
                " b=s.recv(32772,socket.MSG_PEEK)\n" +
                " if len(b)>=4 and len(b)>=4+int.from_bytes(b[:4],'big'):break\n" +
                " time.sleep(.001)\n" +
                "j=json.loads(b[4:4+int.from_bytes(b[:4],'big')])\n" +
                "if j['topic']=='cmd/fast':\n" +
                " os.set_inheritable(0,True);os.set_inheritable(3,True)\n" +
                " os.execv(" + repr(str(original)) + ",[" + repr(str(original)) + "])\n" +
                "b=json.dumps({'stage':'connected'}).encode();s.sendall(struct.pack('!I',len(b))+b)\n" +
                "pathlib.Path(" + repr(str(marker)) + ").touch()\n" +
                "time.sleep(3)\n")
            wrapper.chmod(0o755)
            private_args = argparse.Namespace(**vars(args))
            private_args.direct = str(direct)
            config = configuration(root, port, '127.0.0.1')
            config['connections'][0]['timeout_ms'] = 1000
            runtime = Runtime(private_args, work / 'ipc-stall', config, False)
            try:
                assert runtime.direct({'op': 'launch', 'target': 'device_fan', 'id': 'slow'})['launched']
                end = time.monotonic() + 1
                while not marker.exists():
                    assert time.monotonic() < end
                    time.sleep(.005)
                fast = runtime.execute(fast=True)
                assert fast['result']['success'] is True and fast['ms'] < 450, fast
                slow = runtime.direct({'op': 'collect', 'id': 'slow'})
                assert slow['success'] is False and slow['error'] == 'mqtt_publish_uncertain', slow
                assert not children(runtime.process.pid)
                ipc_stall = {'fast_actual_publish_ms': fast['ms'], 'slow': slow, 'no_mutex_held_across_IPC': True}
            finally:
                runtime.close()
            guards = []
            # Guard only disposable helper copies. Positive controls above use
            # the untouched CMake-built 0755 executable.
            for variant in ('missing', 'corrupt', 'writable', 'setuid', 'setgid', 'symlink'):
                stage = work / ('helper-' + variant)
                stage.mkdir()
                direct = stage / 'direct'
                shutil.copy2(args.direct, direct)
                helper = stage / 'firefly-mqtt-publish-worker'
                original = Path(args.direct).parent / helper.name
                if variant == 'corrupt':
                    helper.write_bytes(b'not an executable\n')
                    helper.chmod(0o755)
                elif variant == 'symlink':
                    helper.symlink_to(original)
                elif variant != 'missing':
                    shutil.copy2(original, helper)
                    helper.chmod({'writable': 0o775, 'setuid': 0o4755, 'setgid': 0o2755}[variant])
                private_args = argparse.Namespace(**vars(args))
                private_args.direct = str(direct)
                runtime = Runtime(private_args, work / ('guard-' + variant), configuration(root, port, '127.0.0.1'), False)
                try:
                    rejected = runtime.execute()
                    assert rejected['result']['success'] is False and rejected['result']['error'] == 'mqtt_publish_worker_unavailable', rejected
                    assert not children(runtime.process.pid)
                    guards.append(variant)
                finally:
                    runtime.close()
            # Wait past all actual delayed DNS replies, then inspect real broker
            # capture. Positive controls prove that this execution path is live.
            time.sleep(1.5)
            trace = (work / 'broker.log').read_text()
            publications = [line for line in trace.splitlines() if 'Received PUBLISH' in line]
            assert len(publications) == 13 and all("'cmd/fast'" in line and '(d0, q1, r0,' in line for line in publications), publications
            assert any(q['host'] == 'probe-delay.review.invalid' for q in dns.queries)
            assert any(q['host'] == 'probe-never.review.invalid' for q in dns.queries)
            evidence = {'result': 'PASS', 'dns_delay_ms': 1250, 'timeout_ms': 100, 'executions': results,
                        'cancelled': cancellations, 'worker_guards': guards, 'ipc_stall': ipc_stall, 'slow_publish': 0, 'positive_publish': 13,
                        'no_worker_or_slot_leak': True, 'store_unchanged': True}
            output = os.environ.get('FIREFLY_MQTT_EXECUTION_EVIDENCE')
            if output:
                Path(output).write_text(json.dumps(evidence, indent=2))
                Path(output + '.broker.log').write_text(trace)
            print(json.dumps(evidence))
        finally:
            broker.terminate()
            broker.wait(timeout=3)
            log.close()
            dns.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('http', 'direct', 'root', 'mosquitto'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--inside', action='store_true')
    parser.add_argument('--host-net', default='')
    parser.add_argument('--host-mount', default='')
    args = parser.parse_args()
    if args.inside:
        inside(args)
    else:
        subprocess.run(['unshare', '-Ur', '-m', '-n', '--', sys.executable, str(Path(__file__).resolve()),
            *sys.argv[1:], '--inside', '--host-net', os.readlink('/proc/self/ns/net'),
            '--host-mount', os.readlink('/proc/self/ns/mnt')], check=True, timeout=40)


if __name__ == '__main__':
    main()
