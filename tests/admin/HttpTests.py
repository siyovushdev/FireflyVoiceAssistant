"""Real HTTP, IPC child, ConfigStore and CommandEngine; only inference is a seam."""
import copy
import http.client
import json
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest
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

    def test_protocol_static_and_errors(self):
        for path,mime in [('/','text/html'),('/admin.js','text/javascript'),('/admin.css','text/css')]:
            status,body,headers=self.request(path);self.assertEqual(status,200);self.assertTrue(headers['Content-Type'].startswith(mime));self.assertGreater(len(body),100)
            self.assertIn("frame-ancestors 'none'",headers['Content-Security-Policy']);self.assertEqual(headers['Cache-Control'],'no-store')
        self.assertEqual(self.request('/../../etc/passwd')[0],404)
        self.assertEqual(self.request('/%2e%2e/etc/passwd')[0],400)
        self.assertEqual(self.request('/api/missing')[0],404)
        self.assertEqual(self.request('/api/config','DELETE',b'{}')[0],405)
        self.assertEqual(self.request('/api/test-command','POST',b'{}',{'Content-Type':'application/json'})[0],403)
        self.assertEqual(self.request('/api/test-command','POST',b'{}',{'Content-Type':'application/json','X-Firefly-Admin':'1','Origin':'http://foreign.invalid'})[0],403)
        self.assertEqual(self.request('/api/test-command','POST',b'{}',{'Content-Type':'text/plain','X-Firefly-Admin':'1'})[0],415)
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
