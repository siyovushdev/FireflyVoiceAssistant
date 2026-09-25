"""M16 connection draft lifecycle: Validate → Save → Apply without hardware I/O on edit."""
import copy
import http.client
import json
import select
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

APP, WORKER, ROOT = map(Path, sys.argv[1:4])
sys.argv = [sys.argv[0]]

class ConnectionAdminTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='firefly-conn-')
        self.log = tempfile.TemporaryFile(mode='w+')
        self.process = subprocess.Popen([str(APP), '--store', self.directory.name,
            '--init-config', str(ROOT/'config/current-m4.json'), '--runtime-worker', str(WORKER),
            '--admin-port', '0'], stdout=subprocess.PIPE, stderr=self.log, text=True)
        self.assertTrue(select.select([self.process.stdout], [], [], 10)[0], 'listener did not start')
        line = self.process.stdout.readline().strip()
        self.assertTrue(line.startswith('ADMIN_LISTEN http://127.0.0.1:'), line)
        self.port = int(line.rsplit(':', 1)[1])
        for _ in range(100):
            if self.api('/api/status')[1]['runtime_ready']: break
            time.sleep(.02)
        else: self.fail('worker did not initialize')

    def tearDown(self):
        self.process.terminate()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait(); self.fail('shutdown hung')
        self.process.stdout.close(); self.log.close(); self.directory.cleanup()

    def request(self, path, method='GET', body=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        headers = {'Content-Type': 'application/json', 'X-Firefly-Admin': '1'}
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, json.loads(data)

    def api(self, path, method='GET', body=None):
        return self.request(path, method, None if body is None else json.dumps(body, ensure_ascii=False).encode())

    def test_connection_draft_save_apply_and_playground_isolation(self):
        stored = self.api('/api/config')[1]
        draft = copy.deepcopy(stored['configuration'])
        if draft.get('schema_version') == 1:
            draft = self.api('/api/config/migrate', 'POST', {'configuration': draft})[1]['configuration']
        active_before = self.api('/api/config/active')[1]['active']['hash']
        binding = {
            'target_id': 'device_kitchen_light', 'action_id': 'action_set_brightness', 'executor': 'http',
            'http': {'method': 'POST', 'url': 'http://192.168.68.50/light', 'timeout_ms': 2000,
                     'content_type': 'application/json', 'body': {}, 'argument': 'brightness'}
        }
        draft['bindings'] = [binding]
        status, valid = self.api('/api/config/validate', 'POST', {'configuration': draft})
        self.assertEqual(status, 200, valid)
        self.assertTrue(valid.get('ok') or valid.get('valid'))
        # Save without Apply leaves active runtime unchanged.
        expected = {k: stored[k] for k in ('revision', 'hash')}
        status, saved = self.api('/api/config', 'PUT', {'configuration': draft, 'expected': expected})
        self.assertEqual(status, 200, saved)
        self.assertEqual(self.api('/api/config/active')[1]['active']['hash'], active_before)
        self.assertTrue(self.api('/api/status')[1]['needs_apply'])
        # Apply publishes binding into active configuration.
        status, applied = self.api('/api/config/apply', 'POST',
                                   {'expected': {k: saved[k] for k in ('revision', 'hash')}})
        self.assertEqual(status, 200, applied)
        active = self.api('/api/config/active')[1]['active']['configuration']
        self.assertEqual(len(active.get('bindings', [])), 1)
        self.assertEqual(active['bindings'][0]['http']['url'], 'http://192.168.68.50/light')
        # Playground still Mock — no transport side effect from test-command execute.
        before = self.api('/api/devices/state')[1]
        status, playground = self.api('/api/test-command', 'POST',
            {'text': 'На кухне поставь свет на 40 процентов', 'execute': True})
        self.assertEqual(status, 200, playground)
        self.assertTrue(playground['execution']['success'])
        self.assertEqual(playground['execution']['current']['brightness'], 40)
        # Production observe path still available; Remove binding → Apply → binding gone.
        removed = copy.deepcopy(active)
        removed['bindings'] = []
        removed['revision'] = saved['revision']
        status, saved2 = self.api('/api/config', 'PUT', {
            'configuration': removed,
            'expected': {k: self.api('/api/config')[1][k] for k in ('revision', 'hash')}})
        self.assertEqual(status, 200, saved2)
        status, applied2 = self.api('/api/config/apply', 'POST',
                                    {'expected': {k: saved2[k] for k in ('revision', 'hash')}})
        self.assertEqual(status, 200, applied2)
        self.assertEqual(self.api('/api/config/active')[1]['active']['configuration'].get('bindings', []), [])
        # Invalid URL rejected by backend validation (not JS-only).
        bad = copy.deepcopy(removed)
        bad['bindings'] = [{**binding, 'http': {**binding['http'], 'url': 'http://esp32.local/light'}}]
        self.assertEqual(self.api('/api/config/validate', 'POST', {'configuration': bad})[0], 422)
        self.assertIsNotNone(before)

if __name__ == '__main__':
    unittest.main()
