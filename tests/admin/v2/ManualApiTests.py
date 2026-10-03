"""B2 endpoint contracts against actual C++ server, private store and local TCP/HTTP fixture."""
import copy
import http.client
import json
import select
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

APP, WORKER, ROOT = (Path(p).resolve() for p in sys.argv[1:4])
sys.argv = [sys.argv[0]]

class Equipment(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self):
        self.connections = 0
        self.requests = []
        self.power = False
        super().__init__(('127.0.0.1', 0), Handler)

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.server.connections += 1
        self.request.settimeout(3)
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = self.request.recv(4096)
            if not chunk: return # probe sent zero HTTP bytes
            data += chunk
        headers, body = data.split(b'\r\n\r\n', 1)
        length = next((int(line.split(b':', 1)[1]) for line in headers.split(b'\r\n') if line.lower().startswith(b'content-length:')), 0)
        while len(body) < length: body += self.request.recv(4096)
        method = headers.split(b' ')[0].decode()
        self.server.requests.append((method, body.decode()))
        if method == 'POST': self.server.power = json.loads(body)['on']
        response = json.dumps({'on': self.server.power}).encode()
        self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ' + str(len(response)).encode() + b'\r\nConnection: close\r\n\r\n' + response)

class ManualApiTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='firefly-b2-api-')
        self.equipment = Equipment()
        self.thread = threading.Thread(target=self.equipment.serve_forever, daemon=True); self.thread.start()
        self.connection = {'id': 'connection_test', 'name': 'Контроллер', 'type': 'http', 'enabled': True, 'timeout_ms': 1000, 'http': {'base_url': f'http://127.0.0.1:{self.equipment.server_address[1]}', 'base_path': ''}}
        config = json.loads((ROOT/'tests/admin/v2/system.json').read_text())
        config['devices'][-1]['spoken_aliases'] = ['свет ноль']  # exact-index candidate with contradictory ORIGINAL light intent
        config['connections'] = [self.connection]
        config['bindings'] = [{'target_id': 'device_fan', 'action_id': 'action_' + action, 'executor': 'http', 'connection_id': self.connection['id'], 'http': {'method': 'POST', 'path': '/relay/1', 'body': {'on': action == 'on'}, 'expected_status': 200}} for action in ('on', 'off')]
        config['observations'] = [{'target_id': 'device_fan', 'connection_id': self.connection['id'], 'executor': 'http', 'http': {'method': 'GET', 'path': '/relay/1'}, 'fields': {'on': 'power'}}]
        fixture = Path(self.directory.name)/'fixture.json'; fixture.write_text(json.dumps(config, ensure_ascii=False))
        (Path(self.directory.name)/'store').mkdir(mode=0o700)
        self.log = tempfile.TemporaryFile(mode='w+')
        self.process = subprocess.Popen([str(APP), '--store', self.directory.name+'/store', '--init-config', str(fixture), '--runtime-worker', str(WORKER), '--admin-port', '0'], stdout=subprocess.PIPE, stderr=self.log, text=True)
        self.assertTrue(select.select([self.process.stdout], [], [], 10)[0])
        self.port = int(self.process.stdout.readline().strip().rsplit(':', 1)[1])
        for _ in range(100):
            if self.api('/api/status')[1]['runtime_ready']: break
            time.sleep(.03)
        else: self.fail('runtime not ready')
        self.active = self.api('/api/config/active')[1]['active']

    def tearDown(self):
        self.process.terminate(); self.process.wait(timeout=10); self.process.stdout.close(); self.log.close()
        self.equipment.shutdown(); self.equipment.server_close(); self.thread.join(); self.directory.cleanup()

    def api(self, path, body=None, method=None, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        conn.request(method or ('POST' if body is not None else 'GET'), path, body=None if body is None else json.dumps(body, ensure_ascii=False).encode(), headers=headers if headers is not None else {'X-Firefly-Admin': '1', 'Content-Type': 'application/json'})
        response = conn.getresponse(); raw = response.read(); conn.close()
        return response.status, json.loads(raw)

    def command(self, **updates):
        payload = {'text': 'включи вентилятор в цехе', 'expected_hash': self.active['hash'], 'expected_action': {'target_id': 'device_fan', 'action_id': 'action_on', 'arguments': {}}}
        payload.update(updates)
        return self.api('/api/devices/command', payload)

    def test_scoped_probe_and_full_validation(self):
        before = {p: self.api(p)[1] for p in ['/api/config', '/api/config/active', '/api/devices/state', '/api/executions']}
        bad = copy.deepcopy(self.connection); bad['id'] = 'connection_unfinished'; bad['http']['base_url'] = ''
        config = copy.deepcopy(before['/api/config']['configuration']); config['connections'].append(bad)
        self.assertEqual(self.api('/api/connections/probe', {'connection': self.connection})[0], 200)
        time.sleep(.04); self.assertEqual(self.equipment.connections, 1); self.assertEqual(self.equipment.requests, [])
        for connection in [bad, {**self.connection, 'name': '   '}, {**self.connection, 'enabled': False}, {**self.connection, 'secret': 'no'}, {**self.connection, 'id': '../bad'}, {**self.connection, 'timeout_ms': 1}]:
            self.assertEqual(self.api('/api/connections/probe', {'connection': connection})[0], 422)
        self.assertEqual(self.api('/api/config/validate', {'configuration': config})[0], 422)
        expected = {k: before['/api/config'][k] for k in ('revision', 'hash')}
        self.assertEqual(self.api('/api/config', {'configuration': config, 'expected': expected}, 'PUT')[0], 422)
        self.assertNotEqual(self.api('/api/config/apply', {'configuration': config, 'expected': expected})[0], 200)
        self.assertEqual(self.equipment.connections, 1)
        for p, value in before.items(): self.assertEqual(self.api(p)[1], value)
        self.assertEqual(self.api('/api/connections/probe', {'connection': self.connection}, headers={})[0], 403)
        self.assertEqual(self.api('/api/connections/probe', {'connection': self.connection}, headers={'X-Firefly-Admin': '1', 'Origin': 'https://foreign.invalid'})[0], 403)

    def test_guarded_on_off_poll_and_all_rejections(self):
        for on in (True, False):
            action = 'on' if on else 'off'; text = ('включи' if on else 'выключи') + ' вентилятор в цехе'
            status, reply = self.command(text=text, expected_action={'target_id': 'device_fan', 'action_id': 'action_'+action, 'arguments': {}})
            self.assertEqual(status, 200, reply); self.assertEqual(reply['language_route'], 'INDEX_EXACT'); self.assertEqual(reply['safety'], 'PASS'); self.assertTrue(reply['execution']['success'])
            state = next(d for d in self.api('/api/devices/state')[1]['devices'] if d['targetId'] == 'device_fan')['properties']['power']
            self.assertEqual(state['knowledge'], 'predicted'); self.assertIs(state['value'], on)
            self.assertEqual(self.api('/api/devices/poll', {'target_id': 'device_fan', 'expected_hash': self.active['hash']})[0], 200)
            state = next(d for d in self.api('/api/devices/state')[1]['devices'] if d['targetId'] == 'device_fan')['properties']['power']
            self.assertEqual(state['knowledge'], 'confirmed'); self.assertIs(state['value'], on)
        self.assertEqual([method for method, _ in self.equipment.requests], ['POST', 'GET', 'POST', 'GET'])
        self.assertIs(json.loads(self.equipment.requests[0][1])['on'], True); self.assertIs(json.loads(self.equipment.requests[2][1])['on'], False)
        for updates in [ {'expected_hash': '0'*64}, {'expected_action': {'target_id': 'device_pump', 'action_id': 'action_on', 'arguments': {}}}, {'expected_action': {'target_id': 'device_fan', 'action_id': 'action_off', 'arguments': {}}}, {'expected_action': {'target_id': 'device_fan', 'action_id': 'action_on', 'arguments': {'unexpected': True}}}, {'text': 'не включай вентилятор в цехе'}, {'text': 'сделай что-нибудь'}, {'text': ''} ]:
            status, reply = self.command(**updates); self.assertEqual(status, 422, reply)
        status, rejected = self.command(text='включи свет ноль', expected_action={'target_id': 'device_bench', 'action_id': 'action_on', 'arguments': {}})
        self.assertEqual(status, 422, rejected)
        self.assertEqual(rejected['language_route'], 'INDEX_EXACT', rejected)
        self.assertEqual(rejected['safety'], 'REJECT', rejected)
        self.assertEqual(len(self.equipment.requests), 4)
        self.assertEqual(self.api('/api/devices/command', {'text': 'x'}, headers={})[0], 403)

    def test_setup_ownership_and_command_block(self):
        status, session = self.api('/api/setup/start', {})
        self.assertEqual(status, 200, session)
        token = session['session_id']
        self.assertEqual(self.api('/api/connections/probe', {'connection': self.connection})[0], 409)
        self.assertEqual(self.api('/api/connections/probe', {'connection': self.connection, 'session_id': token})[0], 200)
        self.assertEqual(self.command()[0], 409)
        self.assertEqual(self.equipment.requests, [])
        self.assertEqual(self.api('/api/setup/cancel', {'session_id': token})[0], 200)

if __name__ == '__main__': unittest.main()
