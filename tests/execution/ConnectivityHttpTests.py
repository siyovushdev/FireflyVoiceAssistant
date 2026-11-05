"""Admin HTTP -> immutable generation -> production command -> loopback device.
Only RKLLM's C API is stubbed by unified_http_test_app; all runtime layers are real.
"""
import copy
import importlib.util
import json
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import urllib.error
from pathlib import Path

APP, ROOT = sys.argv[1:]
spec = importlib.util.spec_from_file_location('mock_http', ROOT + '/scripts/mock-http-device.py')
mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock)
identity = lambda x: {k: x[k] for k in ('revision', 'hash')}

def assert_property_unchanged(before, after, context):
    # Relative age advances between real HTTP requests, especially on ARM64.
    # Keep every value, knowledge flag and absolute evidence timestamp exact;
    # a failed poll must not reset age or fabricate a new observation either.
    key = 'observation_age_ms'
    old_age, new_age = before[key], after[key]
    assert ((old_age is None and new_age is None) or
            (type(old_age) is int and type(new_age) is int and new_age >= old_age)), context
    assert {k: v for k, v in before.items() if k != key} == {
        k: v for k, v in after.items() if k != key}, (context, before, after)


with tempfile.TemporaryDirectory(prefix='firefly-connectivity-') as directory:
    device = mock.MockDevice()
    thread = threading.Thread(target=device.serve_forever, daemon=True)
    thread.start()
    log = open(Path(directory) / 'runtime.log', 'w+')
    process = None

    def api(path, body=None, method=None, expected=200):
        request = urllib.request.Request(url + path, data=None if body is None else json.dumps(body, ensure_ascii=False).encode(),
            method=method, headers={'Content-Type': 'application/json', 'X-Firefly-Admin': '1'})
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                status, result = response.status, json.load(response)
        except urllib.error.HTTPError as error:
            status, result = error.code, json.load(error)
        assert status == expected, (path, status, result)
        return result

    def start():
        global process, url
        process = subprocess.Popen([APP, '--store', directory, '--init-config', ROOT + '/config/empty-home.json',
            '--model', 'stub.rkllm', '--admin-port', '0'], stdout=subprocess.PIPE, stderr=log, text=True)
        assert select.select([process.stdout], [], [], 15)[0], 'startup timeout'
        line = process.stdout.readline()
        assert line.startswith('DEMO_LISTEN'), line
        url = line.split()[1]
        for _ in range(200):
            if api('/api/status')['runtime_ready']: return
            time.sleep(.03)
        raise AssertionError('runtime unavailable')

    def stop():
        if process and process.poll() is None:
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=15) == 0
            process.stdout.close()

    def save_apply(candidate):
        stored = api('/api/config')
        api('/api/config/validate', {'configuration': candidate})
        saved = api('/api/config', {'configuration': candidate, 'expected': identity(stored)}, 'PUT')
        api('/api/config/apply', {'expected': identity(saved)})
        return saved

    def command(on, text=None):
        return api('/api/test-command', {'text': text or ('Включи' if on else 'Выключи') + ' свет в детской', 'execute': True, 'production': True})

    def state():
        return next(d for d in api('/api/devices/state')['devices'] if d['targetId'] == 'device_light')['properties']['power']

    def poll(expected=200, hash=None):
        return api('/api/devices/poll', {'target_id': 'device_light', 'expected_hash': hash or api('/api/config/active')['active']['hash']}, expected=expected)

    def rejected(candidate):
        count = len(device.snapshot())
        result = api('/api/config/validate', {'configuration': candidate}, expected=422)
        assert len(device.snapshot()) == count, result

    try:
        start()
        empty = api('/api/config')
        home = copy.deepcopy(empty['configuration'])
        home['rooms'] = [{'id': 'room_children', 'name': 'Детская', 'speech': {'mentions': ['детская', 'детской'], 'locations': ['в детской']}}]
        home['devices'] = [{'id': 'device_light', 'name': 'Свет', 'kind': 'generic', 'room_id': 'room_children', 'enabled': True,
            'spoken_aliases': ['свет'], 'action_ids': ['action_on', 'action_off'],
            'state': [{'id': 'power', 'name': 'Питание', 'type': 'boolean', 'required': True, 'unit': '', 'description': '', 'initial': False}]}]
        home['actions'] = [{'id': 'action_' + name, 'name': title, 'operation': 'mock.assign.v1', 'enabled': True, 'parameters': [],
            'speech_aliases': aliases, 'description': '', 'assignments': [{'field': 'power', 'value': value}]}
            for name, title, value, aliases in [('on', 'Включить', True, ['включи', 'включить']), ('off', 'Выключить', False, ['выключи', 'выключить'])]]
        save_apply(home)
        assert state()['knowledge'] == 'unknown'
        result = command(True)
        assert result['execution']['error'] == 'binding_missing', result
        assert len(device.snapshot()) == 0
        draft = copy.deepcopy(home)
        draft['connections'] = [{'id': 'connection_test', 'name': 'ESP32 Test', 'type': 'http', 'enabled': True, 'timeout_ms': 150,
            'http': {'base_url': f'http://127.0.0.1:{device.server_port}', 'base_path': ''}}]
        draft['bindings'] = [{'target_id': 'device_light', 'action_id': 'action_' + name, 'connection_id': 'connection_test', 'executor': 'http',
            'http': {'method': 'POST', 'path': '/relay/1', 'body': {'on': value}, 'expected_status': 200}}
            for name, value in [('on', True), ('off', False)]]
        draft['observations'] = [{'target_id': 'device_light', 'connection_id': 'connection_test', 'executor': 'http',
            'http': {'method': 'GET', 'path': '/relay/1', 'expected_status': 200}, 'fields': {'on': 'power'}}]
        old_hash = api('/api/config/active')['active']['hash']
        for endpoint, body in [('/api/connections/test', {'configuration': draft, 'connection_id': 'connection_test'}),
                               ('/api/devices/poll', {'target_id': 'device_light', 'expected_hash': old_hash})]:
            request = urllib.request.Request(url + endpoint, data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
            try:
                urllib.request.urlopen(request, timeout=5)
                raise AssertionError('missing Admin guard')
            except urllib.error.HTTPError as error:
                assert error.code == 403
        assert len(device.snapshot()) == 0
        api('/api/connections/test', {'configuration': draft, 'connection_id': 'connection_test'})
        api('/api/config/validate', {'configuration': draft})
        diff = api('/api/config/diff', {'configuration': draft, 'base': 'active'})
        assert {'connections', 'bindings', 'observations'} <= {x['entity'] for x in diff['changes']}
        assert len(device.snapshot()) == 0
        assert api('/api/config/active')['active']['hash'] == old_hash  # Cancel/discard candidate
        saved = save_apply(draft)
        assert len(device.snapshot()) == 0
        assert api('/api/status')['active']['hash'] == saved['hash']
        for restart in (False, True):
            if restart:
                stop(); start()
                assert state()['knowledge'] == 'unknown'
            for on in (True, False):
                count = len(device.snapshot())
                result = command(on)
                assert result['execution']['success'], result
                assert result['language_route'] == 'INDEX_EXACT', result
                assert len(device.snapshot()) == count + 1
                assert device.snapshot()[-1] == ('POST', '/relay/1', json.dumps({'on': on}, separators=(',', ':')))
                assert state()['knowledge'] == 'predicted' and state()['value'] == on
                poll()
                assert len(device.snapshot()) == count + 2
                assert device.snapshot()[-1] == ('GET', '/relay/1', '')
                assert state()['knowledge'] == 'confirmed' and state()['value'] == on
        device.on = True
        poll(); assert state()['value'] is True, 'independent observation overrides prediction'
        # Neither a stale poll nor rejected/unsupported/ambiguous speech can send HTTP.
        count = len(device.snapshot())
        assert poll(422, old_hash)['errors'][0]['code'] == 'revision_conflict'
        for text in ['Не включай свет в детской', 'Включи и выключи свет в детской', 'Открой свет в детской', 'Какая погода', 'Включи свет в неизвестной комнате', 'Включи свет в детской http://127.0.0.1/evil']:
            result = command(True, text)
            assert not result.get('execution'), (text, result)
        assert len(device.snapshot()) == count
        # Observe failure never confirms fabricated values or overwrites prior evidence.
        command(True)
        for mode, code in [('malformed', 'malformed_observation'), ('wrong_type', 'invalid_observation'), ('missing', 'malformed_observation'),
                           ('duplicate', 'malformed_observation'), ('oversize', 'response_limit'), ('oversize_headers', 'response_limit'), ('truncated', 'malformed_response'), ('500', 'http_error')]:
            device.mode = mode
            before = state()
            failure = poll(422)
            assert failure['errors'][0]['code'].startswith(code), failure
            assert_property_unchanged(before, state(), mode)
        for mode in ['chunked', 'fragmented']:
            device.mode = mode
            poll()
            assert state()['knowledge'] == 'confirmed'
        # A command failure has one attempt, no redirect/retry, no prediction update.
        for mode, code in [('400', 'http_error'), ('500', 'http_error'), ('302', 'http_error'), ('201', 'http_error'),
                           ('oversize', 'response_limit'), ('oversize_headers', 'response_limit'), ('truncated', 'malformed_response'), ('timeout', 'timeout')]:
            device.mode = mode
            before, count = state(), len(device.snapshot())
            result = command(False)
            assert result['execution']['error'].startswith(code), result
            assert len(device.snapshot()) == count + 1
            assert_property_unchanged(before, state(), mode)
        device.mode = ''
        # References, schemas and SSRF/injection controls are validated before Apply.
        for edit in [lambda x: x['connections'].clear(), lambda x: x['devices'].clear(),
                     lambda x: x['connections'].append(copy.deepcopy(x['connections'][0])),
                     lambda x: x['bindings'][0].update(action_id='action_unknown'),
                     lambda x: x['bindings'][0].update(connection_id='connection_missing'),
                     lambda x: x['bindings'][0]['http'].update(path='//127.0.0.2/relay'),
                     lambda x: x['bindings'][0]['http'].update(path='/${voice}'),
                     lambda x: x['bindings'][0]['http'].update(path='/x\r\nHost: evil'),
                     lambda x: x['bindings'][0]['http'].update(headers={'Host': 'evil'}),
                     lambda x: x['bindings'][0]['http'].update(headers={'Authorization': 'secret'}),
                     lambda x: x['bindings'][0]['http'].update(argument='voice'),
                     lambda x: x['bindings'][0]['http'].update(body={'on': '{{voice}}'}),
                     lambda x: x['bindings'][0]['http'].update(body={'data': 'x' * 17000}),
                     lambda x: x['observations'][0]['http'].update(method='POST'),
                     lambda x: x['observations'][0].update(fields={'on': 'missing'})]:
            bad = copy.deepcopy(draft); edit(bad); rejected(bad)
        for base in ['file:///etc/passwd', 'unix:///var/run/x', 'https://127.0.0.1', 'http://user:secret@127.0.0.1',
                     'http://127.0.0.1:80junk', 'http://127.0.0.1#x', 'http://localhost', 'http://127.0.0.1/path']:
            bad = copy.deepcopy(draft); bad['connections'][0]['http']['base_url'] = base; rejected(bad)
        for timeout in [0, 49, 10001]:
            bad = copy.deepcopy(draft); bad['connections'][0]['timeout_ms'] = timeout; rejected(bad)
        # One reusable connection, multiple devices; no duplicated addresses.
        reused = copy.deepcopy(draft)
        second = copy.deepcopy(reused['devices'][0]); second.update(id='device_night', name='Ночник', spoken_aliases=['ночник'])
        reused['devices'].append(second)
        for b in draft['bindings']:
            extra = copy.deepcopy(b); extra['target_id'] = 'device_night'; reused['bindings'].append(extra)
        save_apply(reused)
        assert command(True, 'Включи ночник в детской')['execution']['success']
        assert len(api('/api/config')['configuration']['connections']) == 1
        # Disabled/unavailable Connection fail closed, state unchanged, no retry.
        disabled = copy.deepcopy(reused); disabled['connections'][0]['enabled'] = False
        save_apply(disabled); count = len(device.snapshot())
        assert command(True)['execution']['error'] == 'connection_disabled'
        assert poll(422)['errors'][0]['code'] == 'connection_disabled'
        assert len(device.snapshot()) == count
        unavailable = copy.deepcopy(draft)
        with socket.socket() as closed:
            closed.bind(('127.0.0.1', 0)); port = closed.getsockname()[1]
        unavailable['connections'][0]['http']['base_url'] = f'http://127.0.0.1:{port}'
        save_apply(unavailable)
        assert state()['knowledge'] == 'unknown', 'new physical endpoint cannot inherit evidence'
        assert command(True)['execution']['error'] == 'connection_failure'
        assert poll(422)['errors'][0]['code'] == 'connection_failure'
        # Stale writers/Apply must not change active generation.
        active = api('/api/config/active')['active']['hash']
        api('/api/config', {'configuration': draft, 'expected': identity(saved)}, 'PUT', expected=409)
        api('/api/config/apply', {'expected': identity(saved)}, expected=409)
        assert api('/api/config/active')['active']['hash'] == active
        # Rename preserves ID; removing references permits deletion atomically.
        restored = copy.deepcopy(draft); restored['connections'][0]['name'] = 'Renamed'
        save_apply(restored); assert command(False)['execution']['success']
        poll(); before_rename = state()
        restored['connections'][0]['name'] = 'Display rename only'
        save_apply(restored); assert state() == before_rename
        removed = copy.deepcopy(home); removed['devices'] = []; removed['actions'] = []
        save_apply(removed)
        assert not api('/api/config')['configuration'].get('bindings')
        assert not api('/api/devices/state')['devices']
        print('PASS connectivity: empty home, Admin Preview/Apply/Cancel, generation, deterministic ON/OFF, exact HTTP count, Predicted/Confirmed, restart, shared connection, stale CAS, validation, failures and no rejected-command I/O')
    finally:
        stop()
        device.shutdown(); device.server_close(); thread.join(timeout=2)
        log.close()
