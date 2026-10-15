"""Explicit live RKLLM gate; never CTest. Run with exclusive NPU ownership.

Uses the actual rebuilt firefly-demo and a fresh private store. No fake inference,
Accept, Save, Apply, commands, probe or poll. Raw generation diagnosis is separate.
"""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def stress_workload(diagnostic_only=False, repeatability_only=False):
    short = 'Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать'
    detail = (' Требуется только питание лампы в кабинете, включение и выключение, '
              'без яркости, каналов и других возможностей, без подключения к сети')
    long = short
    while len((long + detail).encode()) <= 1800:
        long += detail
    # Spaces are deliberately not used to manufacture a claimed token load.
    return [('long', long)] if diagnostic_only or repeatability_only else [('control', short), ('long', long)]


def require_power_proposal(state, initial_system):
    assert state['status'] == 'editing' and state['message_incomplete'] is False and not state.get('questions')
    config = state['configuration']
    assert isinstance(initial_system, dict) and config['system'] == initial_system
    assert [len(config[k]) for k in ('rooms', 'devices', 'actions', 'bindings')] == [1, 1, 2, 0]
    room, device = config['rooms'][0], config['devices'][0]
    assert room['name'] == 'Кабинет' and device['name'] == 'Лампа' and device['room_id'] == room['id']
    assert device['kind'] == 'generic' and len(device['state']) == 1
    assert device['state'][0]['id'] == 'power' and device['state'][0]['type'] == 'boolean'
    assert set(device['action_ids']) == {a['id'] for a in config['actions']}
    for action in config['actions']:
        assert action['enabled'] and not action['parameters'] and len(action['assignments']) == 1
        assignment = action['assignments'][0]
        assert assignment['field'] == 'power' and type(assignment['value']) is bool
    assert {a['assignments'][0]['value'] for a in config['actions']} == {False, True}


def external_stress():
    """Existing API workload against an independently supervised PRIVATE backend.

    It never starts/stops a process or calls Apply/device/command endpoints.
    Actual vendor counts, not text lengths, determine reported context coverage.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument('--stress-url')
    parser.add_argument('--store', type=Path)
    parser.add_argument('--mock-log', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--deadline-monotonic', type=float)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--validate-workload', action='store_true')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--diagnostic-only', action='store_true', help='One long request; evidence collection, not stress acceptance')
    mode.add_argument('--repeatability-only', action='store_true', help='Two complete cycles, one strict long proposal per cycle; no saturation claim')
    args = parser.parse_args()
    workload = stress_workload(args.diagnostic_only, args.repeatability_only)
    if args.validate_workload:
        print(json.dumps([{'case': k, 'bytes': len(v.encode()), 'text': v} for k, v in workload], ensure_ascii=False))
        return
    import ipaddress
    from urllib.parse import urlsplit
    parsed = urlsplit(args.stress_url)
    assert parsed.scheme == 'http' and ipaddress.ip_address(parsed.hostname).is_loopback
    assert parsed.port not in (8080, 8765) and parsed.path in ('', '/')
    assert args.output and not args.output.exists() and args.store
    store = args.store.resolve()
    assert store != Path('/var/lib/firefly/store') and not store.is_relative_to('/var/lib/firefly/store')
    assert (store.parent / 'offline-test-marker').is_file() if args.offline else (store.parent / 'stress-test-marker').is_file()
    assert args.mock_log and args.mock_log.resolve().is_relative_to(store.parent)
    assert args.deadline_monotonic and 0 < args.deadline_monotonic - time.monotonic() <= 600
    report = {'profile': 'ADMIN4096_DIAGNOSTIC' if args.diagnostic_only else 'ADMIN4096_BOUNDED_STRESS', 'real_rkllm': not args.offline,
              'diagnostic_only': args.diagnostic_only,
              'repeatability_only': args.repeatability_only,
              'cycles': [], 'passed': False, 'full_4096_context_verified': False,
              'voice': 'NOT_TESTED', 'requests': [], 'apply_calls': None, 'device_requests': None}
    deadline = args.deadline_monotonic
    next_operation_reserve = 5 if args.offline else 120
    # The fixed repeatability profile budgets a 120s message and 100s restore.
    # A delayed first cycle can make the second INCOMPLETE; never extend time.
    cycle_reserve = 300 if args.repeatability_only and not args.offline else next_operation_reserve
    message_reserve = 220 if args.repeatability_only and not args.offline else next_operation_reserve
    report['budget'] = {'cycle_reserve_seconds': cycle_reserve, 'message_reserve_seconds': message_reserve,
                        'message_timeout_seconds': 120 if args.repeatability_only else 150,
                        'restore_timeout_seconds': 100}
    allowed_get = {'/api/status', '/api/config', '/api/config/active', '/api/executions'}
    allowed_post = {'/api/setup/start', '/api/setup/heartbeat', '/api/setup/draft', '/api/setup/message', '/api/setup/cancel'}

    def save():
        # Error JSON may contain escaped lone surrogates; do not lose the
        # original HTTP failure while serializing its diagnostic response.
        args.output.write_text(json.dumps(report, ensure_ascii=True, indent=2) + '\n')
        args.output.chmod(0o600)

    def call(path, body=None, operation_deadline=None):
        assert path in (allowed_get if body is None else allowed_post)
        report['requests'].append({'path': path, 'method': 'GET' if body is None else 'POST'})
        remaining = min(deadline, operation_deadline if operation_deadline is not None else deadline) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('stress original deadline exhausted')
        request = urllib.request.Request(args.stress_url.rstrip('/') + path,
            data=None if body is None else json.dumps(body, ensure_ascii=False).encode(),
            headers={'Origin': args.stress_url, 'Content-Type': 'application/json', 'X-Firefly-Admin': '1'})
        # A mutation is sent once. No automatic retries, including on timeout.
        try:
            timeout = 120 if args.repeatability_only and path == '/api/setup/message' else 150
            with urllib.request.urlopen(request, timeout=min(timeout, remaining)) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            # Preserve the actual error response once, without retrying a mutation.
            # The setup API may itself omit raw vendor output; never invent it.
            limit = 64 * 1024
            failure = {'path': path, 'method': request.get_method(), 'status': error.code,
                       'content_type': error.headers.get('Content-Type', ''),
                       'body_base64': '', 'captured_bytes': 0, 'truncated': True,
                       'capture_complete': False}
            report['http_error'] = failure
            save()  # Status survives even if reading/decoding the body fails.
            raw = b''
            try:
                raw = error.read(limit + 1)
                failure['truncated'] = len(raw) > limit
                failure['capture_complete'] = not failure['truncated']
                length = error.headers.get('Content-Length', '')
                if length.isdigit() and len(raw) < min(int(length), limit + 1):
                    failure['body_capture_error'] = 'IncompleteBody: response ended before Content-Length'
                    failure['capture_complete'] = False
                    failure['truncated'] = True
            except Exception as capture_error:
                partial = getattr(capture_error, 'partial', b'')
                raw = partial if isinstance(partial, bytes) else b''
                failure['body_capture_error'] = type(capture_error).__name__ + ': ' + str(capture_error)
            finally:
                try: error.close()
                except Exception as close_error:
                    failure['body_close_error'] = type(close_error).__name__ + ': ' + str(close_error)
            captured = raw[:limit]
            failure['body_base64'] = base64.b64encode(captured).decode('ascii')
            failure['captured_bytes'] = len(captured)
            try:
                parsed_body = json.loads(captured)
                pending = [(parsed_body, 0)]
                while pending:
                    value, depth = pending.pop()
                    if depth > 32: raise ValueError('Diagnostic JSON nesting exceeds 32')
                    if isinstance(value, dict): pending.extend((x, depth + 1) for x in value.values())
                    elif isinstance(value, list): pending.extend((x, depth + 1) for x in value)
                failure['body_json'] = parsed_body
            except (ValueError, UnicodeError, RecursionError) as parse_error:
                failure['body_parse_error'] = type(parse_error).__name__
            save()
            raise

    def ready(operation_deadline=None):
        value = call('/api/status', operation_deadline=operation_deadline)
        return value.get('runtime_ready') and (args.offline or
            (value.get('stt_ready') and value.get('voice') == 'WAITING_WAKE' and value.get('health', {}).get('overall') == 'ready'))

    def wait_ready(operation_deadline=None):
        end = min(deadline, operation_deadline if operation_deadline is not None else time.monotonic() + 100)
        while time.monotonic() < end:
            if ready(operation_deadline):
                if operation_deadline is not None and time.monotonic() >= end:
                    raise TimeoutError('runtime READY arrived after restore deadline')
                return
            time.sleep(.2)
        raise TimeoutError('runtime restore not READY')

    def unchanged():
        assert call('/api/config')['hash'] == original['hash']
        assert call('/api/config/active')['active'] == active
        assert not call('/api/executions')['executions']
        rows = [json.loads(x) for x in args.mock_log.read_text().splitlines() if x]
        report['device_requests'] = len(rows)
        report['apply_calls'] = sum(x['method'] == 'POST' and x['path'] in ('/api/config/apply', '/api/setup/apply') for x in report['requests'])
        assert not rows, 'Unexpected actual mock request'

    def resources():
        status = call('/api/status')
        return {'monotonic': time.monotonic(), 'pid': status.get('worker_pid'),
                'meminfo': Path('/proc/meminfo').read_text(),
                'pss': (Path('/proc') / str(status['worker_pid']) / 'smaps_rollup').read_text()}

    try:
        wait_ready()
        original = call('/api/config'); active = call('/api/config/active')['active']
        report['initial_system'] = original['configuration'].get('system')
        report['initial_resources'] = resources()
        for cycle in range(1 if args.diagnostic_only else 2):
            if deadline - time.monotonic() < cycle_reserve:
                raise TimeoutError('insufficient original window for next cycle; no additional model launch')
            baseline = resources()
            if deadline - time.monotonic() < cycle_reserve:
                raise TimeoutError('insufficient original window after resources; no setup start')
            state = call('/api/setup/start', {'async_enter': True}); token = state['session_id']
            record = {'cycle': cycle + 1, 'cases': [], 'restored': False}; report['cycles'].append(record); save()
            record['resources_before'] = baseline
            while state.get('status') == 'entering':
                time.sleep(.2); state = call('/api/setup/heartbeat', {'session_id': token})
            assert state['status'] == 'editing'
            if not args.offline: assert call('/api/status')['context_limit'] == 4096
            for label, text in workload:
                if deadline - time.monotonic() < message_reserve:
                    raise TimeoutError('insufficient original window for next request')
                state = call('/api/setup/draft', {'session_id': token, 'version': state['version'], 'configuration': original['configuration']})
                checkpoint = json.loads((store / 'setup-draft.json').read_text()); offset = len(checkpoint['trace'])
                assert checkpoint['focus'] == [] and not checkpoint['unresolved_text']
                started = time.monotonic()
                case = {'case': label, 'input_bytes': len(text.encode()), 'state': None, 'trace': []}
                record['cases'].append(case); save()
                try:
                    if deadline - time.monotonic() < message_reserve:
                        raise TimeoutError('insufficient original window after draft; no message sent')
                    state = call('/api/setup/message', {'session_id': token, 'version': state['version'],
                        'request_id': f'stress-{cycle}-{label}', 'atomic_message': True, 'text': text})
                    case['state'] = state
                finally:
                    case['wall_seconds'] = time.monotonic() - started
                    try:
                        snapshot = json.loads((store / 'setup-draft.json').read_text())
                        case['trace'] = snapshot['trace'][offset:]
                        case['private_status'] = snapshot.get('status')
                        case['message_incomplete'] = snapshot.get('message_incomplete')
                    except (OSError, ValueError, KeyError) as error:
                        case['trace_capture_error'] = type(error).__name__ + ': ' + str(error)
                    save()
                trace = case['trace']
                assert state['status'] in ('editing', 'clarification_required') and not state.get('message_incomplete')
                if args.repeatability_only:
                    require_power_proposal(state, report['initial_system'])
                if label == 'control':
                    assert state['status'] == 'editing' and len(state['configuration']['devices']) == 1
                    assert len(state['configuration']['rooms']) == 1 and len(state['configuration']['actions']) == 2
                if not args.offline:
                    metrics = [x['metrics'] for x in trace if isinstance(x.get('metrics'), dict) and x['metrics']]
                    assert metrics and not any(x.get('error_code') == 'runtime_rejected' for x in trace)
                    for metric in metrics:
                        reserved = metric['context_tokens'] + metric['input_tokens'] + metric['output_budget']
                        observed = metric['context_tokens'] + metric['input_tokens'] + metric['output_tokens']
                        assert 0 < observed <= reserved <= 4096
                        case.setdefault('context_coverage', []).append({'reserved_tokens': reserved, 'observed_tokens': observed})
                unchanged(); save()
            restore_deadline = min(deadline, time.monotonic() + 100) if args.repeatability_only else None
            call('/api/setup/cancel', {'session_id': token}, operation_deadline=restore_deadline)
            wait_ready(restore_deadline); unchanged()
            record['resources_after'] = resources(); record['restored'] = True; save()
        report['passed'] = True
        report['coverage_note'] = ('Scripted offline API/transport only; no real token or NPU coverage.' if args.offline else
            'One diagnostic long request; not a stress acceptance result.' if args.diagnostic_only else
            'Two complete repeatability cycles with one strict long proposal each; actual token counts recorded; full4096 saturation and soak NOT_VERIFIED.' if args.repeatability_only else
            'Two complete profile cycles; actual token counts recorded. This is not a claim of filling every slot of the 4096-token KV or a long-term soak.')
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
        raise
    finally:
        # On failure the independent existing M19 recovery owns shutdown.
        save()


if '--stress-url' in __import__('sys').argv or '--validate-workload' in __import__('sys').argv:
    external_stress()
    raise SystemExit(0)

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
