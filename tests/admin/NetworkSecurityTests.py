"""Real production HTTP paths; private stores/fake inference only, no installed service."""
import hashlib
import json
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ADMIN, WORKER, ROOT = map(Path, sys.argv[1:4])
DEMO, DEMO_FIXTURE = map(Path, sys.argv[4:6]) if len(sys.argv) == 6 else (None, None)
sys.argv = sys.argv[:4]
import HttpTests as fixture


class NetworkSecurityTests(unittest.TestCase):
    setUp = fixture.HttpTests.setUp
    tearDown = fixture.HttpTests.tearDown
    request = fixture.HttpTests.request
    api = fixture.HttpTests.api

    def raw(self, host_lines, method='GET', path='/', body=b'', extra=b''):
        data = (f'{method} {path} HTTP/1.1\r\n'.encode() + host_lines + extra +
                f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)
        with socket.create_connection(('127.0.0.1', self.port), timeout=5) as sock:
            sock.sendall(data)
            chunks = []
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        header, payload = b''.join(chunks).split(b'\r\n\r\n', 1)
        return int(header.split(b' ')[1]), payload

    def test_valid_authorities_and_tunnel_origin(self):
        for host in ['127.0.0.1', f'127.0.0.1:{self.port}', 'localhost',
                     'localhost:8080', 'LOCALHOST:18080', '127.0.0.1:18080',
                     'localhost:1', 'localhost:65535']:
            for path in ['/', '/api/status']:
                with self.subTest(host=host, path=path):
                    self.assertEqual(self.request(path, headers={'Host': host})[0], 200)
        config = self.api('/api/config')[1]['configuration']
        for host, origin in [('LOCALHOST:18080', 'http://localhost:18080'),
                             ('127.0.0.1:18080', 'http://127.0.0.1:18080'),
                             ('localhost', 'http://localhost:80'),
                             ('localhost:80', 'http://LOCALHOST')]:
            status, body, _ = self.request('/api/config/validate', 'POST',
                json.dumps({'configuration': config}).encode(),
                {'Host': host, 'Origin': origin, 'X-Firefly-Admin': '1', 'Content-Type': 'application/json'})
            self.assertEqual(status, 200, body)
            self.assertTrue(json.loads(body)['valid'])
        self.assertEqual(self.api('/api/devices/observe', 'POST',
            {'target_id': 'device_kitchen_light', 'properties': {'brightness': 40}})[0], 404)

    def test_foreign_hosts_and_rebinding_cannot_read_or_mutate(self):
        before = self.api('/api/config')[1]
        before_state = self.api('/api/devices/state')[1]
        def files():
            return {str(p.relative_to(self.directory.name)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in Path(self.directory.name).rglob('*') if p.is_file()}
        before_files = files()
        expected = {k: before[k] for k in ('revision', 'hash')}
        for host in ['attacker.example', 'attacker.example:8080', 'localhost.evil',
                     '127.0.0.1.evil', '0.0.0.0', '192.168.1.20', 'localhost.', '127.0.0.2']:
            for path in ['/', '/api/status', '/api/config', '/api/devices/state']:
                status, body, _ = self.request(path, headers={'Host': host})
                self.assertEqual(status, 403, (host, path, body))
                self.assertEqual(json.loads(body)['errors'][0]['message'], 'Host is not allowed.')
                self.assertNotIn(host.encode(), body)
            # A real valid POST that would Apply if routed; matching hostile Origin is insufficient.
            status, body, _ = self.request('/api/config/apply', 'POST', json.dumps({'expected': expected}),
                {'Host': host, 'Origin': 'http://' + host, 'X-Firefly-Admin': '1', 'Content-Type': 'application/json'})
            self.assertEqual(status, 403, body)
            self.assertEqual(json.loads(body)['errors'][0]['message'], 'Host is not allowed.')
        self.assertEqual(self.api('/api/config')[1], before)
        self.assertEqual(self.api('/api/devices/state')[1], before_state)
        self.assertEqual(files(), before_files)
        self.assertEqual(self.api('/api/config/active')[1]['active']['hash'], before['hash'])

    def test_malformed_and_duplicate_host(self):
        for host in ['localhost:abc', 'localhost:99999', 'localhost:0', 'localhost:',
                     '127.0.0.1:abc', 'localhost@evil', 'user@127.0.0.1',
                     'localhost,127.0.0.1', 'localhost/path', 'localhost?x', 'localhost#x',
                     'local host', 'local\thost', 'localhost\x00', 'localhost\\evil',
                     'localhost:+80', 'localhost:-1', 'localhost:80:90', '[::1]',
                     'http://localhost', 'localhost:80junk', '.localhost', 'local..host']:
            with self.subTest(host=host):
                code, _ = self.raw(b'Host: ' + host.encode() + b'\r\n')
                self.assertEqual(code, 400)
        for lines in [b'', b'Host:\r\n', b'Host: localhost\r\nHost: localhost\r\n',
                      b'Host: localhost\r\nhOsT: attacker.example\r\n']:
            self.assertEqual(self.raw(lines)[0], 400)

    def test_existing_mutation_guards(self):
        base = {'Host': 'localhost:18080', 'Origin': 'http://localhost:18080',
                'X-Firefly-Admin': '1', 'Content-Type': 'application/json'}
        for origin in ['null', 'https://localhost:18080', 'http://attacker.example:18080',
                       'http://127.0.0.1:18080', 'http://localhost:18081',
                       'http://localhost:18080/', 'http://localhost:18080?x',
                       'http://user@localhost:18080', 'http://localhost:abc']:
            self.assertEqual(self.request('/api/config/apply', 'POST', b'{}',
                {**base, 'Origin': origin})[0], 403)
        for change, code in [({'X-Firefly-Admin': ''}, 403), ({'Sec-Fetch-Site': 'cross-site'}, 403),
                             ({'Content-Type': 'text/plain'}, 415)]:
            self.assertEqual(self.request('/api/config/apply', 'POST', b'{}', {**base, **change})[0], code)
        # Local curl without Origin still reaches routing/validation.
        self.assertEqual(self.api('/api/config/apply', 'POST', {})[0], 400)


class BindPolicyTests(unittest.TestCase):
    def test_rejected_before_store_or_worker_initialization(self):
        apps = [ADMIN] + ([DEMO] if DEMO else [])
        for app in apps:
            for bind in ['0.0.0.0', '192.168.1.20', '10.0.0.5', '127.0.0.2', '::1', 'localhost']:
                with self.subTest(app=app, bind=bind), tempfile.TemporaryDirectory(prefix='firefly-bind-') as directory:
                    temp = Path(directory)
                    store = temp / 'store'
                    store.mkdir(mode=0o700)
                    marker = temp / 'worker-started'
                    worker = temp / 'worker'
                    worker.write_text('#!/bin/sh\ntouch "' + str(marker) + '"\n')
                    worker.chmod(0o700)
                    args = [str(app), '--store', str(store), '--init-config', str(ROOT / 'config/empty-home.json'),
                            '--admin-bind', bind, '--admin-port', '0', '--model', 'must-not-load']
                    if app == ADMIN:
                        args += ['--runtime-worker', str(worker)]
                    result = subprocess.run(args, capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 1, result)
                    self.assertIn('V1 Admin bind must be exactly 127.0.0.1', result.stderr)
                    self.assertEqual(list(store.iterdir()), [])
                    self.assertFalse(marker.exists())
                    self.assertNotIn('STARTUP process started', result.stderr)
                    self.assertNotIn('_LISTEN', result.stdout)

    def test_default_and_explicit_loopback(self):
        # Actual Main; actual DemoMain linked to fake RKLLM, no STT/voice/NPU.
        for app in [ADMIN] + ([DEMO_FIXTURE] if DEMO_FIXTURE else []):
            for options in [[], ['--admin-bind', '127.0.0.1']]:
                with self.subTest(app=app, options=options), tempfile.TemporaryDirectory(prefix='firefly-bind-valid-') as directory:
                    with tempfile.TemporaryFile(mode='w+') as log:
                        proc = subprocess.Popen([str(app), '--store', directory, '--init-config',
                            str(ROOT / 'config/empty-home.json'), '--admin-port', '0', *options],
                            stdout=subprocess.PIPE, stderr=log, text=True)
                        try:
                            self.assertTrue(select.select([proc.stdout], [], [], 10)[0])
                            line = proc.stdout.readline().strip()
                            self.assertRegex(line, r'^(ADMIN|DEMO)_LISTEN http://127\.0\.0\.1:\d+$')
                            port = int(line.rsplit(':', 1)[1])
                            with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
                                sock.sendall(b'GET /api/status HTTP/1.1\r\nHost: localhost:18080\r\n\r\n')
                                self.assertIn(b' 200 ', sock.recv(2048))
                        finally:
                            proc.terminate()
                            proc.wait(timeout=10)
                            proc.stdout.close()
                        self.assertEqual(proc.returncode, 0)


if __name__ == '__main__':
    unittest.main()
