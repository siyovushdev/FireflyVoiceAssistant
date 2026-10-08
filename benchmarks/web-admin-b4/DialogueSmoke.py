"""Explicit live RKLLM gate; never CTest. Run with exclusive NPU ownership.

Uses the actual rebuilt firefly-demo and a fresh private store. No fake inference,
Accept, Save, Apply, commands, probe or poll. Raw generation diagnosis is separate.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--root', required=True)
parser.add_argument('--output', required=True)
args = parser.parse_args()
output = Path(args.output).resolve()
if output.exists():
    raise SystemExit('Refusing to overwrite evidence')
for proc in Path('/proc').iterdir():
    if proc.name.isdigit():
        try:
            if 'librkllmrt.so' in (proc / 'maps').read_text():
                raise SystemExit('Another RKLLM owner is running: ' + proc.name)
        except OSError:
            pass
root = Path(args.root).resolve()
private = Path(tempfile.mkdtemp(prefix='firefly-b4-dialogue-live-'))
(private / 'store').mkdir(mode=0o700)
report = {'private_directory': str(private), 'binary': str(Path(args.binary).resolve()),
          'cases': [], 'requests': [], 'passed': False, 'real_rkllm': True,
          'voice': 'not_run', 'isolation': 'empty draft and focus reset between requests; normal per-request KV clear'}

def save():
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')

save()
with (private / 'server.log').open('w') as log:
    process = subprocess.Popen([args.binary, '--store', str(private / 'store'),
        '--init-config', str(root / 'config/empty-home.json'),
        '--model', '/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm',
        '--admin-bind', '127.0.0.1', '--admin-port', '0'], stdout=log, stderr=subprocess.STDOUT)
    try:
        url = None
        for _ in range(240):
            for line in (private / 'server.log').read_text().splitlines():
                if line.startswith('DEMO_LISTEN '):
                    url = line.split()[1]
            if url:
                break
            if process.poll() is not None:
                raise RuntimeError('demo startup failed')
            time.sleep(1)
        if not url:
            raise RuntimeError('no listener')
        report['url'] = url

        def call(path, body=None):
            report['requests'].append({'path': path, 'method': 'GET' if body is None else 'POST'})
            request = urllib.request.Request(url + path,
                data=None if body is None else json.dumps(body, ensure_ascii=False).encode(),
                headers={'Origin': url, 'Content-Type': 'application/json', 'X-Firefly-Admin': '1'})
            try:
                with urllib.request.urlopen(request, timeout=300) as response:
                    value = json.load(response)
            except urllib.error.HTTPError as error:
                raise RuntimeError(f'{path}: {error.code}: {error.read().decode()}') from error
            return value

        for _ in range(180):
            if call('/api/status')['runtime_ready']:
                break
            time.sleep(1)
        else:
            raise RuntimeError('runtime unavailable')
        original = call('/api/config')
        active = call('/api/config/active')['active']
        report['stored_before'] = {'hash': original['hash'], 'revision': original['revision']}
        report['active_before'] = active
        state = call('/api/setup/start', {'async_enter': True})
        token = state['session_id']
        for _ in range(180):
            state = call('/api/setup/heartbeat', {'session_id': token})
            if state['status'] == 'editing':
                break
            if state['status'] != 'entering':
                raise RuntimeError('setup entry failed: ' + state['status'])
            time.sleep(1)
        else:
            raise RuntimeError('setup entry timeout')
        owner = 'создай цех зону и добавь туда вентилятор где он может включаться и выключаться по питанию'
        cases = [('C', 'Добавь серверный ИБП без зоны.'),
                 ('B', 'В цехе есть вентилятор, его можно включать и выключать.'),
                 ('A1', owner), ('A2', owner), ('A3', owner)]
        for label, text in cases:
            state = call('/api/setup/draft', {'session_id': token, 'version': state['version'], 'configuration': original['configuration']})
            assert state['configuration'] == original['configuration']
            checkpoint = json.loads((private / 'store/setup-draft.json').read_text())
            assert checkpoint['focus'] == [] and checkpoint['unresolved_text'] == ''
            assert not state.get('message_incomplete', False)
            previous_trace_size = len(checkpoint['trace'])
            call('/api/setup/heartbeat', {'session_id': token})
            started = time.monotonic()
            state = call('/api/setup/message', {'session_id': token, 'version': state['version'],
                'request_id': label, 'atomic_message': True, 'text': text})
            record = {'case': label, 'text': text, 'wall_seconds': time.monotonic() - started, 'state': state}
            diagnostic = json.loads((private / 'store/setup-draft.json').read_text())
            record['trace'] = diagnostic['trace'][previous_trace_size:]
            report['cases'].append(record)
            save()
            config = state['configuration']
            assert state['status'] == 'editing' and not state.get('message_incomplete', False), record
            assert len(config['devices']) == 1
            device = config['devices'][0]
            if label == 'C':
                assert config['rooms'] == [] and device['room_id'] is None
                assert device['name'].casefold() == 'серверный ибп'
                assert device['action_ids'] == [] and device['state'] == [] and config['actions'] == []
            else:
                assert len(config['rooms']) == 1 and config['rooms'][0]['name'].casefold() == 'цех'
                assert device['name'].casefold() == 'вентилятор' and device['room_id'] == config['rooms'][0]['id']
                assert len(device['action_ids']) == 2 and len(device['state']) == 1
                assert len(config['actions']) == 2
                assert all(a['parameters'] == [] and len(a['assignments']) == 1 and a['assignments'][0]['field'] == 'power' for a in config['actions'])
                assert {a['assignments'][0]['value'] for a in config['actions']} == {False, True}
            assert not config['bindings'] and not config.get('connections', []) and not config.get('observations', [])
            assert call('/api/config')['hash'] == original['hash']
            assert call('/api/config/active')['active'] == active
            assert call('/api/executions')['executions'] == []
            record['passed'] = True
            record['stored_active_unchanged'] = True
            save()
            print(label, 'PASS', round(record['wall_seconds'], 3), flush=True)
        call('/api/setup/cancel', {'session_id': token})
        assert call('/api/config')['hash'] == original['hash']
        assert call('/api/executions')['executions'] == []
        report['passed'] = True
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
    finally:
        save()
        process.terminate()
        process.wait(timeout=180)
if not report['passed']:
    raise SystemExit(report.get('error', 'live gate failed'))
print('PASS real RKLLM: A 3/3, B, C; no config publication or equipment requests')
