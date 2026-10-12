"""Real HTTP, IPC child, ConfigStore and CommandEngine; only inference is a seam."""
import copy
import http.client
import hashlib
import importlib.util
import json
import os
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from unittest.mock import patch
from pathlib import Path

APP, WORKER, ROOT = map(Path, sys.argv[1:4])
sys.argv = [sys.argv[0]]

class HttpTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='firefly-http-')
        self.log = tempfile.TemporaryFile(mode='w+')
        self.process = subprocess.Popen([str(APP), '--store', self.directory.name,
            '--init-config', str(ROOT/'config/current-m4.json'), '--runtime-worker', str(WORKER),
            '--admin-port', '0'], stdout=subprocess.PIPE, stderr=self.log, text=True)
        self.assertTrue(select.select([self.process.stdout], [], [], 10)[0], 'listener did not start')
        line = self.process.stdout.readline().strip()
        self.assertTrue(line.startswith('ADMIN_LISTEN http://127.0.0.1:'), line)
        self.port = int(line.rsplit(':',1)[1])
        for _ in range(100):
            if self.api('/api/status')[1]['runtime_ready']: break
            time.sleep(.02)
        else: self.fail('worker did not initialize')

    def tearDown(self):
        self.process.terminate()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill();self.process.wait();self.fail('shutdown hung')
        self.process.stdout.close();self.log.close();self.directory.cleanup()

    def request(self,path,method='GET',body=None,headers=None):
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        all_headers={'Content-Type':'application/json','X-Firefly-Admin':'1'} if headers is None else headers
        connection.request(method,path,body=body,headers=all_headers)
        response=connection.getresponse();result=(response.status,response.read(),dict(response.getheaders()));connection.close();return result

    def api(self,path,method='GET',body=None):
        status,data,_=self.request(path,method,None if body is None else json.dumps(body,ensure_ascii=False).encode())
        return status,json.loads(data)

    def test_bathroom_lifecycle(self):
        initial=self.api('/api/config')[1];expected={k:initial[k] for k in ('revision','hash')}
        draft=copy.deepcopy(initial['configuration'])
        draft['rooms'].append({'id':'room_bathroom','name':'Ванная','speech':{'mentions':['ванная','ванна','ванной','ванне'],'locations':['в ванной','в ванне']}})
        draft['devices'].append({'id':'device_bathroom_light','name':'Свет в ванной','kind':'light','room_id':'room_bathroom','enabled':True,'action_ids':['action_set_brightness'],'spoken_aliases':[]})
        status,valid=self.api('/api/config/validate','POST',{'configuration':draft});self.assertEqual(status,200);self.assertTrue(valid['apply_ready'])
        self.assertEqual(self.api('/api/config')[1]['hash'],initial['hash'])
        status,saved=self.api('/api/config','PUT',{'configuration':draft,'expected':expected});self.assertEqual(status,200)
        self.assertTrue(self.api('/api/status')[1]['needs_apply'])
        self.assertEqual(self.api('/api/config','PUT',{'configuration':draft,'expected':expected})[0],409)
        expected={k:saved[k] for k in ('revision','hash')}
        status,applied=self.api('/api/config/apply','POST',{'expected':expected});self.assertEqual(status,200)
        self.assertEqual(applied['active']['hash'],saved['hash']);self.assertIn('device_bathroom_light',applied['active']['context'])
        for text,value in [('В ванной поставь свет на 40 процентов',40),('В ванне поставь свет на 30 процентов',30)]:
            status,result=self.api('/api/test-command','POST',{'text':text});self.assertEqual(status,200)
            self.assertEqual(result['result'],{'actionId':'action_set_brightness','targetId':'device_bathroom_light','arguments':{'brightness':value}})
            self.assertEqual(result['route'],'deterministic');self.assertEqual(result['safety'],'PASS');self.assertEqual(result['domain'],'PASS')
        rejected=copy.deepcopy(saved['configuration']);next(d for d in rejected['devices'] if d['id']=='device_bathroom_light')['name']='REJECT_CONTEXT'
        self.assertEqual(self.api('/api/config/validate','POST',{'configuration':rejected})[0],422)
        status,bad_stored=self.api('/api/config','PUT',{'configuration':rejected,'expected':expected});self.assertEqual(status,200)
        self.assertEqual(self.api('/api/config/apply','POST',{'expected':{k:bad_stored[k] for k in ('revision','hash')}})[0],422)
        self.assertEqual(self.api('/api/config/active')[1]['active']['hash'],saved['hash'])
        self.assertEqual(self.api('/api/test-command','POST',{'text':'В ванной поставь свет на 40 процентов'})[1]['result']['arguments']['brightness'],40)
        self.assertEqual(len(self.api('/api/config/revisions')[1]['revisions']),2)

    def test_editor_without_inference_worker(self):
        with tempfile.TemporaryDirectory(prefix='firefly-editor-') as directory:
            process=subprocess.Popen([str(APP),'--store',directory,'--init-config',str(ROOT/'config/current-m4.json'),
                '--runtime-worker','/nonexistent/firefly-test-worker','--admin-port','0'],stdout=subprocess.PIPE,stderr=self.log,text=True)
            previous=self.port
            try:
                self.assertTrue(select.select([process.stdout],[],[],10)[0])
                line=process.stdout.readline().strip();self.assertTrue(line.startswith('ADMIN_LISTEN '));self.port=int(line.rsplit(':',1)[1])
                config=self.api('/api/config')[1];self.assertFalse(self.api('/api/status')[1]['runtime_ready'])
                status,valid=self.api('/api/config/validate','POST',{'configuration':config['configuration']})
                self.assertEqual(status,200);self.assertFalse(valid['apply_ready']);self.assertEqual(valid['runtime_preflight'],'unavailable')
                config['configuration']['system']['name']='Offline edit'
                self.assertEqual(self.api('/api/config','PUT',{'configuration':config['configuration'],'expected':{k:config[k] for k in ('revision','hash')}})[0],200)
                self.assertEqual(self.api('/api/test-command','POST',{'text':'Открой входную дверь'})[0],503)
                self.assertEqual(self.request('/')[0],200)
            finally:
                self.port=previous;process.terminate();process.wait(timeout=10);process.stdout.close()

    def test_rollout_non_actuating_checks(self):
        def module(name, path):
            spec = importlib.util.spec_from_file_location(name, path)
            value = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(value)
            return value

        rollout = module('rollout_http_regression', ROOT/'scripts/production-rollout.py')
        device = module('rollout_http_mock', ROOT/'scripts/mock-http-device.py').MockDevice()
        thread = threading.Thread(target=device.serve_forever, daemon=True)
        thread.start()
        try:
            # Real HTTP bindings make an accidental execution observable. The
            # existing fake worker uses production RuntimeWorker with mock inference.
            home = json.loads((ROOT/'config/empty-home.json').read_text())
            home['rooms'] = [{'id': 'room_office', 'name': 'Кабинет', 'speech': {
                'mentions': ['кабинет', 'кабинете'], 'locations': ['в кабинете']}}]
            home['devices'] = [{'id': 'device_lamp', 'name': 'Лампа', 'kind': 'generic',
                'room_id': 'room_office', 'enabled': True, 'spoken_aliases': ['лампа', 'лампу'],
                'action_ids': ['action_on', 'action_off'], 'state': [{'id': 'power', 'name': 'Питание',
                    'type': 'boolean', 'required': True, 'unit': '', 'description': '', 'initial': False}]}]
            home['actions'] = [{'id': 'action_'+name, 'name': title, 'operation': 'mock.assign.v1',
                'enabled': True, 'parameters': [], 'speech_aliases': [alias], 'description': '',
                'assignments': [{'field': 'power', 'value': value}]}
                for name, title, alias, value in [('on', 'Включить', 'включи', True), ('off', 'Выключить', 'выключи', False)]]
            home['connections'] = [{'id': 'connection_test', 'name': 'Isolated mock', 'type': 'http',
                'enabled': True, 'timeout_ms': 150, 'http': {
                    'base_url': f'http://127.0.0.1:{device.server_port}', 'base_path': ''}}]
            home['bindings'] = [{'target_id': 'device_lamp', 'action_id': 'action_'+name,
                'connection_id': 'connection_test', 'executor': 'http', 'http': {
                    'method': 'POST', 'path': '/relay/1', 'body': {'on': value}, 'expected_status': 200}}
                for name, value in [('on', True), ('off', False)]]
            home['observations'] = [{'target_id': 'device_lamp', 'connection_id': 'connection_test',
                'executor': 'http', 'http': {'method': 'GET', 'path': '/relay/1', 'expected_status': 200},
                'fields': {'on': 'power'}}]
            identity = lambda value: {k: value[k] for k in ('revision', 'hash')}
            stored = self.api('/api/config')[1]
            status, saved = self.api('/api/config', 'PUT', {'configuration': home, 'expected': identity(stored)})
            self.assertEqual(status, 200, saved)
            self.assertEqual(self.api('/api/config/apply', 'POST', {'expected': identity(saved)})[0], 200)
            status, preview = self.api('/api/test-command', 'POST', {
                'text': 'включи лампу в кабинете', 'execute': False, 'production': False})
            self.assertEqual(status, 200, preview)
            self.assertEqual(preview['result']['targetId'], 'device_lamp')
            self.assertEqual(preview['result']['actionId'], 'action_on')
            self.assertIsNone(preview['execution'])
            history = self.api('/api/executions')[1]
            state = self.api('/api/devices/state')[1]
            def store_sha256():
                return {str(p.relative_to(self.directory.name)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in Path(self.directory.name).rglob('*') if p.is_file()}
            store_before = store_sha256()
            unauthenticated = []
            texts = ('какая погода', 'включи лампу в неизвестной комнате')
            for text in texts:
                body = {'text': text, 'execute': False, 'production': False}
                status, raw, _ = self.request('/api/test-command', 'POST', json.dumps(body).encode(),
                    {'Content-Type': 'application/json'})
                self.assertEqual(status, 403)
                response = json.loads(raw)
                self.assertEqual(response['errors'][0]['message'], 'X-Firefly-Admin: 1 is required.')
                unauthenticated.append({'request': body, 'status': status, 'response': response})

            # Redirect only the controller's fixed loopback origin in the test.
            # Keep its real request headers/body, real HTTP guard and response.
            real_urlopen = urllib.request.urlopen
            requests = []
            def isolated_urlopen(request, timeout):
                url = request if isinstance(request, str) else request.full_url
                self.assertIn(url, ('http://127.0.0.1:8080/api/test-command',
                                   'http://127.0.0.1:8080/api/executions'))
                local = url.replace('http://127.0.0.1:8080', f'http://127.0.0.1:{self.port}', 1)
                if isinstance(request, str):
                    return real_urlopen(local, timeout=timeout)
                self.assertEqual(request.get_method(), 'POST')
                self.assertEqual(request.get_header('X-firefly-admin'), '1')
                self.assertEqual(request.get_header('Content-type'), 'application/json')
                body = json.loads(request.data)
                self.assertIs(body['execute'], False)
                self.assertIs(body['production'], False)
                response = real_urlopen(urllib.request.Request(local, data=request.data,
                    headers=dict(request.header_items()), method=request.get_method()), timeout=timeout)
                requests.append({'request': body, 'headers': dict(request.header_items()), 'status': response.status})
                return response

            controller = rollout.Production.__new__(rollout.Production)
            controller.root = Path(self.directory.name)/'controller'
            (controller.root/'evidence').mkdir(parents=True)
            controller.operation_deadline = time.monotonic()+30
            with patch.object(rollout.urllib.request, 'urlopen', side_effect=isolated_urlopen):
                replies = controller.non_actuating_checks()
            self.assertEqual([r['request']['text'] for r in requests], list(texts))
            self.assertEqual([r['status'] for r in requests], [200, 200])
            for reply in replies:
                self.assertTrue(reply['response']['ok'])
                self.assertEqual(reply['response']['result'], {'unsupported': True})
                self.assertIsNone(reply['response']['execution'])
            self.assertEqual(self.api('/api/executions')[1], history)
            self.assertEqual(self.api('/api/devices/state')[1], state)
            store_after = {k: v for k, v in store_sha256().items() if not k.startswith('controller/')}
            self.assertEqual(store_after, store_before)
            self.assertEqual(device.snapshot(), [])
            evidence = {'inference': 'MOCK (admin_fake_worker); no NPU/microphone',
                'known_device_non_actuating_preview': preview,
                'unauthenticated': unauthenticated, 'authenticated_requests': requests, 'replies': replies,
                'history_before': history, 'history_after': self.api('/api/executions')[1],
                'device_http_requests': device.snapshot(), 'store_sha256_before': store_before,
                'store_sha256_after': store_after, 'device_state_unchanged': True}
            if os.environ.get('FIREFLY_ROLLOUT_HTTP_EVIDENCE'):
                Path(os.environ['FIREFLY_ROLLOUT_HTTP_EVIDENCE']).write_text(
                    json.dumps(evidence, ensure_ascii=False, indent=2)+'\n')
        finally:
            device.shutdown(); device.server_close(); thread.join(timeout=2)

    def test_protocol_static_and_errors(self):
        html = self.request('/')[1].decode()
        self.assertIn('type="module"', html)
        assets = re.findall(r'(?:src|href)="(/assets/[^\"]+)"', html)
        self.assertTrue(assets)
        for alias in ['/index.html', '/v2', '/v2/', '/v2/index.html']:
            self.assertEqual(self.request(alias)[1].decode(), html)
        for legacy in ['/admin.js', '/admin.css', '/admin-model.js', '/typed-editor.js', '/simple-editor.js', '/connection-editor.js']:
            self.assertEqual(self.request(legacy)[0], 404)
        for path,mime in [('/', 'text/html')] + [(asset, 'text/css' if asset.endswith('.css') else 'image/svg+xml' if asset.endswith('.svg') else 'text/javascript') for asset in assets]:
            status,body,headers=self.request(path);self.assertEqual(status,200);self.assertTrue(headers['Content-Type'].startswith(mime));self.assertGreater(len(body),100)
            self.assertIn("frame-ancestors 'none'",headers['Content-Security-Policy']);self.assertEqual(headers['Cache-Control'],'no-store')
        self.assertEqual(self.request('/../../etc/passwd')[0],404)
        self.assertEqual(self.request('/%2e%2e/etc/passwd')[0],400)
        self.assertEqual(self.request('/api/missing')[0],404)
        self.assertEqual(self.request('/api/config','DELETE',b'{}')[0],405)
        self.assertEqual(self.request('/api/test-command','POST',b'{}',{'Content-Type':'application/json'})[0],403)
        self.assertEqual(self.request('/api/test-command','POST',b'{}',{'Content-Type':'application/json','X-Firefly-Admin':'1','Origin':'http://foreign.invalid'})[0],403)
        self.assertEqual(self.request('/api/test-command','POST',b'{}',{'Content-Type':'text/plain','X-Firefly-Admin':'1'})[0],415)
        # Simulated state injection is absent even with valid Admin headers.
        status, body, _ = self.request('/api/devices/observe', 'POST',
            b'{"target_id":"device_kitchen_light","properties":{"brightness":40}}')
        self.assertEqual(status, 404)
        self.assertEqual(json.loads(body)['errors'][0]['code'], 'not_found')
        for body in [b'{',b'[]',b'{}',b'{"text":12}',b'{"text":"a","text":"b"}',b'{"text":"x"} garbage']:
            self.assertEqual(self.request('/api/test-command','POST',body)[0],400)
        with socket.create_connection(('127.0.0.1',self.port),timeout=5) as sock:
            sock.sendall(b'POST /api/test-command HTTP/1.1\r\nHost: localhost\r\nContent-Length: 524289\r\n\r\n')
            self.assertIn(b' 413 ',sock.recv(2048))
        for headers,code in [(b'Content-Length: 2\r\nContent-Length: 2',400),(b'Transfer-Encoding: chunked',400),(b'Content-Length: -1',400),(b'Expect: 100-continue',400)]:
            with socket.create_connection(('127.0.0.1',self.port),timeout=5) as sock:
                sock.sendall(b'POST /api/test-command HTTP/1.1\r\nHost: localhost\r\nX-Firefly-Admin: 1\r\nContent-Type: application/json\r\n'+headers+b'\r\n\r\n{}')
                self.assertIn(f' {code} '.encode(),sock.recv(2048))
        for _ in range(25):self.assertEqual(self.api('/api/status')[0],200)

if __name__=='__main__':unittest.main()
