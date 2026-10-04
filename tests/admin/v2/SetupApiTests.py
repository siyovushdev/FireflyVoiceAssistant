"""Private C++ API fixture: owner/version/atomic/no-zone/recovery, no hardware."""
import copy, json, pathlib, subprocess, sys, tempfile, time, urllib.request, urllib.error
app, worker, root = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='firefly-b3-api-') as temp:
    pathlib.Path(temp, 'store').mkdir(mode=0o700)
    log = open(pathlib.Path(temp)/'server.log', 'w+')
    process = subprocess.Popen([app, '--store', temp+'/store', '--init-config', root+'/config/empty-home.json', '--runtime-worker', worker, '--admin-port', '0'], stdout=log, stderr=log)
    try:
        url = None
        for _ in range(200):
            log.flush(); log.seek(0)
            for line in log.read().splitlines():
                if line.startswith('ADMIN_LISTEN '): url = line.split()[1]
            if url: break
            time.sleep(.05)
        assert url, 'server startup'
        def call(path, body=None, code=200):
            request = urllib.request.Request(url+path, data=None if body is None else json.dumps(body).encode(), headers={'Content-Type':'application/json', 'Origin':url,'X-Firefly-Admin':'1'})
            try:
                with urllib.request.urlopen(request, timeout=20) as response: status, data = response.status, json.load(response)
            except urllib.error.HTTPError as error: status, data = error.code, json.load(error)
            assert status == code, (path,status,data)
            return data
        for _ in range(200):
            if call('/api/status')['runtime_ready']: break
            time.sleep(.05)
        original = call('/api/config'); assert not call('/api/setup/status')['active']
        state = call('/api/setup/start', {}); token = state['session_id']
        body = {'session_id':token,'version':state['version'],'request_id':'no-zone','text':'Добавь контроллер стенда без зоны.','atomic_message':True}
        call('/api/setup/message', {**body,'session_id':'not-owner'},409)
        call('/api/setup/message', {**body,'version':-1},409)
        state = call('/api/setup/message', body)
        device = state['configuration']['devices'][0]
        assert device['room_id'] is None and device['action_ids'] == [] and state['configuration']['rooms'] == []
        assert call('/api/config')['hash'] == original['hash']
        call('/api/config', { 'configuration': state['configuration'], 'expected': {'hash':original['hash'],'revision':original['revision']} },405)
        call('/api/config/apply', {'session_id':token,'expected':{'hash':original['hash'],'revision':original['revision']}},409)
        state = call('/api/setup/draft', {'session_id':token,'version':state['version'],'configuration':original['configuration']})
        state = call('/api/setup/message', {'session_id':token,'version':state['version'],'request_id':'A','atomic_message':True,'text':'Создай Детскую, добавь туда Свет и Телевизор. Оба только включаются и выключаются.'})
        assert len(state['configuration']['devices']) == 2
        assert all(len(d['action_ids']) == 2 for d in state['configuration']['devices'])
        # Deterministic B3 candidate takes the existing B2 validation/draft path.
        physical = copy.deepcopy(state['configuration']); target = physical['devices'][0]
        physical['connections'] = [{'id':'connection_explicit','name':'Explicit HTTP','type':'http','enabled':True,'timeout_ms':2000,'http':{'base_url':'http://127.0.0.1:8765','base_path':''}}]
        physical['bindings'] = [{'target_id':target['id'],'action_id':a['id'],'connection_id':'connection_explicit','executor':'http','http':{'method':'POST','path':'/relay/1','body':{'on':a['assignments'][0]['value']},'expected_status':200}} for a in physical['actions'] if a['id'] in target['action_ids']]
        physical['observations'] = [{'target_id':target['id'],'connection_id':'connection_explicit','executor':'http','http':{'method':'GET','path':'/relay/1','expected_status':200},'fields':{'on':'power'}}]
        assert call('/api/config/validate',{'session_id':token,'configuration':physical})['valid']
        call('/api/setup/draft',{'session_id':token,'version':state['version']-1,'configuration':physical},409)
        invalid = copy.deepcopy(physical); invalid['connections'][0]['http']['base_url'] = 'http://example.com'
        call('/api/config/validate',{'session_id':token,'configuration':invalid},422)
        call('/api/setup/draft',{'session_id':token,'version':state['version'],'configuration':invalid},409)
        assert call('/api/setup/status')['configuration'] == state['configuration']
        state = call('/api/setup/draft',{'session_id':token,'version':state['version'],'configuration':physical})
        assert len(state['configuration']['bindings']) == 2 and len(state['configuration']['observations']) == 1
        assert call('/api/config')['hash'] == original['hash']
        checkpoint = state['configuration']
        call('/api/setup/message', {'session_id':token,'version':state['version'],'request_id':'malformed','atomic_message':True,'text':'сломанный запрос'},409)
        state = call('/api/setup/status'); assert state['message_incomplete'] and state['configuration'] == checkpoint
        call('/api/setup/draft', {'session_id':token,'version':state['version'],'configuration':checkpoint},409)
        call('/api/setup/apply', {'session_id':token,'version':state['version']},409)
        call('/api/setup/cancel', {'session_id':token})
        assert call('/api/config')['hash'] == original['hash']
        assert call('/api/executions')['executions'] == []
        print('PASS B3 API: owner, stale version, nullable zone, atomic A, incomplete checkpoint, strict Apply, zero execution')
    finally:
        process.terminate(); process.wait(timeout=20); log.close()
