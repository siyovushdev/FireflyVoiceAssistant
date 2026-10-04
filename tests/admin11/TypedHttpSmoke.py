"""Real RKLLM typed runtime smoke. Own ephemeral store; never touches the user's store.
Usage: python3 TypedHttpSmoke.py DEMO MODEL REVIEW_CONFIG ARTIFACT_DIRECTORY
DEMO is the absolute path to firefly-demo. REVIEW_CONFIG is an explicitly reviewed schema-v2 config (for example exported from Vue Advanced).
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

app, model, configuration, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
records = []
url = None
process = None
local_http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def request(path, body=None, method=None):
    started = time.monotonic()
    query = urllib.request.Request(url + path, data=None if body is None else json.dumps(body, ensure_ascii=False).encode(),
        method=method, headers={'Content-Type': 'application/json', 'X-Firefly-Admin': '1'})
    try:
        response = local_http.open(query, timeout=240)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        code, raw = response.code, response.read().decode('utf-8', errors='replace')
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        records.append({'path':path,'status':code,'raw_body':raw,'wall_ms':(time.monotonic()-started)*1000})
        raise AssertionError(f'Non-JSON HTTP {code}: {raw[:256]}')
    records.append({'path': path, 'status': code, 'wall_ms':(time.monotonic()-started)*1000,'response': value})
    return code, value

def checked(path, body=None, method=None):
    code, value = request(path, body, method)
    assert code == 200 and value.get('ok', True), (path, code, value.get('errors'))
    return value

def busy():
    for _ in range(150):
        if checked('/api/status')['runtime_busy']:
            return
        time.sleep(.05)
    raise AssertionError('Inference was not observed in progress')

def command(text):
    result = checked('/api/test-command', {'text': text, 'execute': True})
    print('COMMAND', text, result['result'], 'generation', result['generation_id'], flush=True)
    return result

def identity(value):
    return {key: value[key] for key in ('revision', 'hash')}

try:
    with tempfile.TemporaryDirectory(prefix='firefly-typed-http-') as temporary:
        seed = json.loads(configuration.read_text())
        seed['revision'] = 1
        seed_file = Path(temporary) / 'seed.json'
        seed_file.write_text(json.dumps(seed, ensure_ascii=False))
        store = Path(temporary) / 'store'
        store.mkdir(mode=0o700)
        with (output / 'server.log').open('w') as log, concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            process = subprocess.Popen([str(app.resolve()), '--store', str(store), '--init-config', str(seed_file),
                '--model', str(model), '--admin-port', '0'], stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    assert process.poll() is None, 'Runtime exited during startup'
                    match = re.search(r'DEMO_LISTEN (http://\S+)', (output / 'server.log').read_text())
                    if match:
                        url = match.group(1)
                        if checked('/api/status')['runtime_ready']:
                            break
                    time.sleep(.25)
                assert url and checked('/api/status')['runtime_ready']
                initial = checked('/api/status')
                assert initial['worker_pid'] == process.pid
                # One real model alternates admin instructions and command scopes.
                for entity, name in [('location', 'Ванная')]:
                    draft = checked('/api/admin/draft', {'entity': entity, 'name': name})
                    assert draft['suggestion']['description'] and draft['suggestion']['mentions']
                    # Structured validity alone missed a copied, unrelated example.
                    proposal = draft['suggestion']
                    assert proposal['type'] not in ('pump', 'tv', 'conveyor')
                    assert any('ван' in alias or 'душ' in alias for alias in proposal['aliases'])
                    assert 'насос' not in proposal['description'].lower()
                assert command('На кухне поставь свет на 40 процентов')['execution']['current']['brightness'] == 40
                television = command('Будь добр, включи детский телевизор')
                assert television['route'] == 'rkllm' and television['execution']['current']['power'] is True
                before = checked('/api/devices/state')['devices']
                history = checked('/api/executions')['executions']
                for text in ['На кухне выключи свет на 50 процентов',
                             'Открой входную дверь и включи детский телевизор',
                             'Открой входную дверь и включи свет на кухне']:
                    unsafe = command(text)
                    assert unsafe['result'] == {'unsupported': True}
                    assert checked('/api/devices/state')['devices'] == before
                    assert checked('/api/executions')['executions'] == history
                draft = checked('/api/admin/draft', {'entity': 'location', 'name': 'Цех упаковки'})
                assert draft['suggestion']['description'] and draft['suggestion']['mentions']
                marked = command('Пометь образец "готово"')
                assert marked['route'] == 'rkllm' and marked['execution']['current']['label'] == 'готово'
                # Existing DEV labels also pass through the actual v2 HTTP executor.
                root = Path(__file__).resolve().parents[2]
                cases = [line.split('\t',3) for line in (root/'tests/data/dev.tsv').read_text().splitlines() if line and not line.startswith('#')]
                assert len(cases)==48
                compatibility = seed['compatibility']
                light_codes = {target: code for code,target in compatibility['light_targets'].items()}
                totals = {'total':48,'pass':0,'false_reject':0,'wrong_or_unsafe':0}
                for case_id, category, expected, text in cases:
                    old_states=checked('/api/devices/state')['devices']
                    old_history=checked('/api/executions')['executions']
                    observed=command(text);action=observed['result']
                    if action.get('unsupported'):compact='U'
                    elif action['actionId']==compatibility['door_action_id'] and action['targetId']==compatibility['door_target_id']:compact='D'
                    elif action['actionId']==compatibility['light_action_id'] and action['targetId'] in light_codes:compact='L|'+light_codes[action['targetId']]+'|'+str(action['arguments']['brightness'])
                    else:compact='GENERIC_NONLEGACY'
                    totals['pass']+=compact==expected
                    totals['false_reject']+=expected!='U' and compact=='U'
                    totals['wrong_or_unsafe']+=compact!=expected and compact!='U'
                    records.append({'dev_case':case_id,'expected':expected,'actual':compact})
                    if expected=='U':
                        assert compact=='U', (case_id,compact)
                        assert checked('/api/devices/state')['devices']==old_states
                        assert checked('/api/executions')['executions']==old_history
                records.append({'http_dev':totals})
                assert totals['wrong_or_unsafe']==0
                print('HTTP DEV v2',totals,flush=True)
                # An excessive complete binding fails admission; no truncation/publish.
                stored = checked('/api/config')
                too_large = json.loads(json.dumps(stored['configuration']))
                sample = next(d for d in too_large['devices'] if d['id'] == marked['result']['targetId'])
                sample['spoken_aliases'] = [hashlib.sha256(str(i).encode()).hexdigest() for i in range(32)]
                code, failed = request('/api/config/validate', {'configuration': too_large})
                assert code == 422 and not failed['ok']
                assert checked('/api/status')['active'] == initial['active']
                assert command('Будь добр, выключи детский телевизор')['execution']['current']['power'] is False
                # Save a candidate, then race Apply with a real request retaining generation 1.
                candidate = stored['configuration']
                next(d for d in candidate['devices'] if d['id'] == marked['result']['targetId'])['name'] = 'Образец с меткой'
                saved = checked('/api/config', {'configuration': candidate, 'expected': identity(stored)}, 'PUT')
                in_flight = pool.submit(command, 'Пометь образец "проверено"')
                busy()
                code, conflict = request('/api/config/apply', {'expected': identity(saved)})
                assert code == 409 and conflict['errors'][0]['code'] == 'runtime_busy'
                old = in_flight.result()
                assert old['generation_id'] == 1 and old['execution']['current']['label'] == 'проверено'
                checked('/api/config/apply', {'expected': identity(saved)})
                new = command('Пометь образец "готово"')
                assert new['generation_id'] == 2
                assert new['execution']['previous']['label'] == 'проверено' and new['execution']['current']['label'] == 'готово'
                final = checked('/api/status')
                assert final['worker_pid'] == initial['worker_pid']
                # Interrupt only this owned process during synchronous real inference.
                interrupted = pool.submit(request, '/api/test-command', {'text': 'Пометь образец "пауза"', 'execute': True})
                busy()
                started = time.monotonic()
                process.send_signal(signal.SIGINT)
                process.wait(timeout=60)
                shutdown = {'pid': process.pid, 'exit': process.returncode, 'seconds': time.monotonic()-started}
                try:
                    shutdown['request'] = interrupted.result()
                except (OSError, urllib.error.URLError) as error:
                    shutdown['request_error'] = str(error)
                records.append({'shutdown': shutdown})
                assert process.returncode == 0
                text = (output / 'server.log').read_text()
                assert text.count('init_ms=') == 1 and text.count('RKLLM destroyed') == 1
                print('PASS typed HTTP: task contexts, real typed RKLLM, failed admission, concurrent Apply, request lifetime, state continuity, SIGINT; one model load', flush=True)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                    process.wait(timeout=90)
finally:
    (output / 'evidence.json').write_text(json.dumps(records, ensure_ascii=False, indent=2))
