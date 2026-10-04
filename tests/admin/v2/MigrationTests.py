"""Real C++ static delivery and schema-v2 store compatibility across releases.

Inference is an IPC test seam. Optional B3 binary exercises release rollback;
the store is never restored or migrated by this harness.
"""
import copy
import http.client
import json
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time


APP, WORKER, ROOT = (Path(value).resolve() for value in sys.argv[1:4])
PREVIOUS = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None
PREVIOUS_WORKER = Path(sys.argv[5]).resolve() if len(sys.argv) > 5 else WORKER


class Server:
    def __init__(self, app, store, fixture, worker=WORKER):
        self.log = tempfile.TemporaryFile(mode='w+')
        self.process = subprocess.Popen([
            str(app), '--store', str(store), '--init-config', str(fixture),
            '--runtime-worker', str(worker), '--admin-port', '0',
        ], stdout=subprocess.PIPE, stderr=self.log, text=True)
        assert select.select([self.process.stdout], [], [], 10)[0], 'listener missing'
        line = self.process.stdout.readline().strip()
        assert line.startswith('ADMIN_LISTEN http://127.0.0.1:'), line
        self.port = int(line.rsplit(':', 1)[1])
        for _ in range(200):
            if self.api('/api/status')['runtime_ready']:
                break
            time.sleep(.03)
        else:
            self.close()
            raise AssertionError('runtime not ready')

    def request(self, path, method='GET', body=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        connection.request(method, path, None if body is None else json.dumps(body).encode(),
                           {'X-Firefly-Admin': '1', 'Content-Type': 'application/json'})
        response = connection.getresponse()
        result = response.status, response.read(), dict(response.getheaders())
        connection.close()
        return result

    def api(self, path, method='GET', body=None):
        status, raw, _ = self.request(path, method, body)
        assert status == 200, (path, status, raw)
        return json.loads(raw)

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('shutdown hung')
        finally:
            self.process.stdout.close()
            self.log.close()


def identity(snapshot):
    return {key: snapshot[key] for key in ('revision', 'hash')}


with tempfile.TemporaryDirectory(prefix='firefly-b4-migration-') as directory:
    root = Path(directory)
    store = root / 'store'
    store.mkdir(mode=0o700)
    config = json.loads((ROOT / 'tests/admin/v2/system.json').read_text())
    config['connections'] = [{'id': 'connection_existing', 'name': 'B3 controller',
                              'type': 'http', 'enabled': True, 'timeout_ms': 1000,
                              'http': {'base_url': 'http://127.0.0.1:9', 'base_path': ''}}]
    config['bindings'] = [{'target_id': 'device_fan', 'action_id': 'action_' + action,
                           'executor': 'http', 'connection_id': 'connection_existing',
                           'http': {'method': 'POST', 'path': '/relay/1',
                                    'body': {'on': action == 'on'}, 'expected_status': 200}}
                          for action in ('on', 'off')]
    config['observations'] = [{'target_id': 'device_fan', 'connection_id': 'connection_existing',
                              'executor': 'http', 'http': {'method': 'GET', 'path': '/relay/1'},
                              'fields': {'on': 'power'}}]
    fixture = root / 'fixture.json'
    fixture.write_text(json.dumps(config, ensure_ascii=False))
    # An older release creates the existing applied store when provided.
    initial = Server(PREVIOUS or APP, store, fixture, PREVIOUS_WORKER if PREVIOUS else WORKER)
    try:
        before = initial.api('/api/config')
        original_active = initial.api('/api/config/active')['active']
    finally:
        initial.close()
    server = Server(APP, store, fixture)
    try:
        loaded = server.api('/api/config')
        assert identity(loaded) == identity(before), 'upgrade changed stored identity'
        assert identity(server.api('/api/config/active')['active']) == identity(original_active)
        for key in ('connections', 'bindings', 'observations'):
            assert loaded['configuration'][key] == before['configuration'][key]
        status, html, headers = server.request('/')
        assert status == 200 and b'type="module"' in html
        assert headers['Cache-Control'] == 'no-store'
        assert "script-src 'self'" in headers['Content-Security-Policy']
        for alias in ('/index.html', '/v2', '/v2/', '/v2/index.html'):
            assert server.request(alias)[1] == html
        assets = re.findall(rb'(?:src|href)="(/assets/[^\"]+)"', html)
        assert assets and all(server.request(asset.decode())[0] == 200 for asset in assets)
        for missing in ('/admin.js', '/admin.css', '/admin-model.js', '/typed-editor.js',
                        '/simple-editor.js', '/connection-editor.js', '/assets/missing.js',
                        '/firefly-assets.json', '/api/missing', '/unknown-server-path'):
            assert server.request(missing)[0] == 404, missing
        assert server.request('/', 'POST', {})[0] == 405
        assert identity(server.api('/api/config')) == identity(before), 'opening caused save'
        assert not server.api('/api/executions')['executions'], 'hidden operation'
        candidate = copy.deepcopy(loaded['configuration'])
        candidate['system']['name'] = 'B4 applied configuration'
        assert server.api('/api/config/validate', 'POST', {'configuration': candidate})['apply_ready']
        saved = server.api('/api/config', 'PUT', {'configuration': candidate, 'expected': identity(loaded)})
        assert identity(server.api('/api/config/active')['active']) == identity(original_active), 'Save applied'
        server.api('/api/config/apply', 'POST', {'expected': identity(saved)})
        assert identity(server.api('/api/config/active')['active']) == identity(saved)
        export = server.request('/api/config/export')[1]
        revisions = server.api('/api/config/revisions')['revisions']
        snapshot = server.api('/api/config')['configuration']
        assert not server.api('/api/executions')['executions'], 'Apply executed hardware'
    finally:
        server.close()
    store_bytes = {str(path.relative_to(store)): path.read_bytes() for path in store.rglob('*') if path.is_file()}
    for app in ([PREVIOUS, APP] if PREVIOUS else [APP]):
        server = Server(app, store, fixture, PREVIOUS_WORKER if app == PREVIOUS else WORKER)
        try:
            assert identity(server.api('/api/config')) == identity(saved), 'restart changed stored identity'
            assert identity(server.api('/api/config/active')['active']) == identity(saved)
            assert server.api('/api/config')['configuration'] == snapshot, 'configuration changed'
            assert server.request('/api/config/export')[1] == export
            assert server.api('/api/config/revisions')['revisions'] == revisions
            assert server.request('/')[0] == 200
            assert not server.api('/api/executions')['executions']
        finally:
            server.close()
        assert {str(path.relative_to(store)): path.read_bytes() for path in store.rglob('*') if path.is_file()} == store_bytes, 'rollback mutated store files'
    print('PASS B4 migration: root bundle, alias, missing assets, existing identity/connectivity, explicit Apply, restart' +
          (', B3 → B4 → B3 → B4 rollback with unchanged config bytes' if PREVIOUS else ''))
