"""Actual unified main/HTTP/UI/runtime with the existing stub vendor C API only."""
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error

app, root = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    log = open(Path(directory) / 'runtime.log', 'w+')
    p = subprocess.Popen([app, '--store', directory, '--init-config', root+'/config/current-m4.json',
                          '--model', 'stub.rkllm', '--admin-port', '0'], stdout=subprocess.PIPE, stderr=log, text=True)
    try:
        line = p.stdout.readline()
        assert line.startswith('DEMO_LISTEN '), line
        url = line.split()[1]
        def api(path, body=None, method=None):
            request = urllib.request.Request(url+path, data=None if body is None else json.dumps(body).encode(),
                method=method, headers={'Content-Type':'application/json','X-Firefly-Admin':'1'})
            try:
                with urllib.request.urlopen(request, timeout=10) as response:
                    return response.status, json.load(response)
            except urllib.error.HTTPError as error:
                return error.code, json.load(error)
        deadline = time.monotonic()+10
        while time.monotonic()<deadline:
            status = api('/api/status')[1]
            if status['runtime_ready']: break
            time.sleep(.02)
        assert status['runtime_ready'] and status['worker_pid'] == p.pid
        assert subprocess.run(['ps','-o','pid=','--ppid',str(p.pid)],capture_output=True,text=True).stdout.strip() == ''
        assert status['executor']=='bound' and status['voice']=='disabled' and status['device_count']==5
        with urllib.request.urlopen(url) as response:
            html=response.read().decode()
            # Vue renders these flows in the B2/B3 Chromium suites. The unified
            # server delivers the same production entry, not vanilla inline DOM.
            assert 'type="module"' in html and 'admin.js' not in html
            assets=re.findall(r'(?:src|href)="(/assets/[^\"]+)"', html)
            assert assets
            for asset in assets:
                with urllib.request.urlopen(url+asset) as resource:
                    assert resource.status==200 and resource.headers['Cache-Control']=='no-store'
        before=api('/api/devices/state')[1]['devices']
        preview=api('/api/test-command', {'text':'На кухне поставь свет на 40 процентов'})[1]
        assert preview['execution'] is None and api('/api/devices/state')[1]['devices']==before
        execute=api('/api/test-command', {'text':'На кухне поставь свет на 40 процентов','execute':True})[1]
        assert execute['execution']['current']=={'brightness':40}
        for text in ['Какая сегодня погода','Открой дверь','На кухне выключи свет на 50 процентов']:
            before=api('/api/devices/state')[1]['devices']
            rejected=api('/api/test-command', {'text':text,'execute':True})[1]
            assert rejected['result']=={'unsupported':True} and rejected['execution'] is None
            assert api('/api/devices/state')[1]['devices']==before
        door=api('/api/test-command', {'text':'Открой входную дверь','execute':True})[1]
        assert door['execution']['previous']=={'state':'CLOSED'} and door['execution']['current']=={'state':'OPEN'}
        stored=api('/api/config')[1]
        candidate=stored['configuration']
        candidate['rooms'].append({'id':'room_bathroom','name':'Ванная','speech':{'mentions':['ванная','ванна','ванной','ванне'],'locations':['в ванной','в ванне']}})
        candidate['devices'].append({'id':'device_bathroom_light','name':'Свет в ванной','kind':'light','room_id':'room_bathroom','enabled':True,'action_ids':['action_set_brightness'],'spoken_aliases':[]})
        assert api('/api/config/validate',{'configuration':candidate})[0]==200
        identity=lambda v:{'revision':v['revision'],'hash':v['hash']}
        saved=api('/api/config',{'configuration':candidate,'expected':identity(stored)},'PUT')[1]
        assert api('/api/config/apply',{'expected':identity(saved)})[0]==200
        assert api('/api/status')[1]['active']['generation_id']==2
        result=api('/api/test-command',{'text':'В ванной поставь свет на 40 процентов','execute':True})[1]
        assert result['result']['targetId']=='device_bathroom_light' and result['execution']['current']=={'brightness':40}
        assert 'device_bathroom_light' in api('/api/config/active')[1]['active']['context']
        narrow=json.loads(json.dumps(candidate)); next(a for a in narrow['actions'] if a['operation']=='light.set_brightness.v1')['parameters'][0]['maximum']=30
        assert api('/api/config/validate',{'configuration':narrow})[0]==422
        savedNarrow=api('/api/config',{'configuration':narrow,'expected':identity(saved)},'PUT')[1]
        assert api('/api/config/apply',{'expected':identity(savedNarrow)})[0]==422
        assert api('/api/status')[1]['active']['generation_id']==2
        assert api('/api/executions')[1]['executions'][-1]['source']=='admin-test'
        assert api('/api/test-command',{'text':'x','execute':1})[0]==400
        assert api('/api/devices/state',{},'POST')[0]==405
        assert api('/api/not-found')[0]==404
        print('PASS unified real HTTP: one PID/no children, preview/execute, door, unsafe unchanged, Bathroom Apply, state conflict, UI, history, strict requests')
    finally:
        p.send_signal(signal.SIGINT)
        assert p.wait(timeout=15)==0, log.read()
        p.stdout.close(); log.close()
