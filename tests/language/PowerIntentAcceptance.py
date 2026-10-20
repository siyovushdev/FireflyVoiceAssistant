#!/usr/bin/env python3
"""Opt-in real Qwen/Mosquitto acceptance inside a fresh rootless net namespace.

Run only with prepared local model, llama-server and Mosquitto paths. No
downloads, host services, production endpoints, NPU or physical devices.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def field(s):
    b = s.encode()
    return struct.pack('!H', len(b)) + b


def packet(kind, body):
    n = len(body)
    prefix = bytearray([kind])
    while True:
        digit = n % 128
        n //= 128
        prefix.append(digit | (128 if n else 0))
        if not n:
            break
    return bytes(prefix) + body


def read_packet(s):
    def exact(n):
        data = b''
        while len(data) < n:
            b = s.recv(n - len(data))
            assert b, 'MQTT disconnected'
            data += b
        return data
    kind = exact(1)[0]
    n, mult = 0, 1
    for _ in range(4):
        b = exact(1)[0]
        n += (b & 127) * mult
        if not b & 128:
            break
        mult *= 128
    else:
        raise AssertionError('Invalid MQTT remaining length')
    assert n < 4096
    return kind, exact(n)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def http(url, payload=None, method=None):
    r = urllib.request.Request(url, data=None if payload is None else json.dumps(payload).encode(),
        method=method or ('GET' if payload is None else 'POST'),
        headers={'Content-Type': 'application/json', 'X-Firefly-Admin': '1'})
    try:
        response = urllib.request.urlopen(r, timeout=90)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.load(response)


def main():
    p = argparse.ArgumentParser()
    for arg in ['root', 'fixture', 'server', 'model', 'mosquitto', 'output']:
        p.add_argument('--' + arg, type=Path, required=True)
    p.add_argument('--host-netns', help=argparse.SUPPRESS)
    p.add_argument('--mosquitto-libs', type=Path, help='Existing private native broker library directory, if required')
    p.add_argument('--chromium', type=Path, help='Optional real Chromium for visible Preview/diagnostic checks')
    args = p.parse_args()
    if args.host_netns is None:
        assert os.getuid() != 0, 'Start as an ordinary user, never host root'
        host = os.readlink('/proc/self/ns/net')
        os.execvp('unshare', ['unshare', '--user', '--map-root-user', '--net', sys.executable,
            str(Path(__file__).resolve()), *sys.argv[1:], '--host-netns', host])
    assert os.getuid() == 0 and os.readlink('/proc/self/ns/net') != args.host_netns, 'Fresh rootless net namespace required'
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    args.output.mkdir(mode=0o700)
    out = args.output
    server_port, broker_port = free_port(), free_port()
    model_url = f'http://127.0.0.1:{server_port}'
    processes, files = [], []
    calls, tests, errors = [], [], []
    observer = None
    result = {'status': 'RUNNING', 'model': str(args.model), 'llama_server': str(args.server),
        'model_backend': 'real Qwen3-4B GGUF Q4_K_M on CPU, not target RKLLM W8A8',
        'namespace': os.readlink('/proc/self/ns/net'), 'calls': calls, 'tests': tests,
        'production_or_physical_access': False}
    try:
        log = (out / 'qwen.log').open('w'); files.append(log)
        qwen = subprocess.Popen([str(args.server), '-m', str(args.model), '--host', '127.0.0.1',
            '--port', str(server_port), '--ctx-size', '4096', '--parallel', '1', '--threads', '4',
            '--threads-batch', '4', '--n-gpu-layers', '0', '--no-webui', '--reasoning', 'off',
            '--reasoning-budget', '0', '--cache-ram', '0', '--timeout', '60'], stdout=log, stderr=log)
        processes.append(qwen)
        deadline = time.monotonic() + 45
        while True:
            assert qwen.poll() is None, 'Qwen server exited'
            try:
                if http(model_url + '/health')[0] == 200:
                    break
            except (OSError, ValueError):
                pass
            assert time.monotonic() < deadline, 'Qwen readiness timeout'
            time.sleep(.1)
        conf = out / 'mosquitto.conf'
        conf.write_text(f'user root\nlistener {broker_port} 127.0.0.1\nallow_anonymous true\npersistence false\nlog_type all\n')
        log = (out / 'mosquitto.log').open('w'); files.append(log)
        broker_env = os.environ.copy()
        if args.mosquitto_libs:
            broker_env['LD_LIBRARY_PATH'] = str(args.mosquitto_libs)
        broker = subprocess.Popen([str(args.mosquitto), '-c', str(conf)], stdout=log, stderr=log, env=broker_env)
        processes.append(broker)
        deadline = time.monotonic() + 5
        while True:
            assert broker.poll() is None, 'Private broker exited'
            try:
                observer = socket.create_connection(('127.0.0.1', broker_port), timeout=1)
                break
            except OSError:
                assert time.monotonic() < deadline
                time.sleep(.05)
        observer.settimeout(2)
        # This private passive observer sends no PINGREQ. Disable its idle
        # keepalive expiry so slow real-model/browser checks cannot disconnect
        # it between cases. The bounded operations and finally block below own
        # lifetime/cleanup; production clients and their limits are unchanged.
        observer.sendall(packet(0x10, b'\x00\x04MQTT\x04\x02\x00\x00' + field('acceptance-observer')))
        assert read_packet(observer) == (0x20, b'\0\0')
        observer.sendall(packet(0x82, b'\0\1' + field('fixture/power/set') + b'\0'))
        assert read_packet(observer) == (0x90, b'\0\1\0')

        doc = json.loads((args.root / 'tests/admin/v2/system.json').read_text())
        device, room = doc['devices'][0], doc['rooms'][0]
        device.update(id='kitchen_light', name='Свет', spoken_aliases=['свет'], room_id='kitchen')
        room.update(id='kitchen', name='Кухня', speech={'mentions': ['кухня', 'кухне'], 'locations': ['на кухне']})
        doc.update(devices=[device], rooms=[room], observations=[], connections=[{
            'id': 'isolated_broker', 'name': 'Private fixture only', 'type': 'mqtt', 'enabled': True,
            'timeout_ms': 1500, 'network': {'host': '127.0.0.1', 'port': broker_port}, 'mqtt': {'tls': False}}],
            bindings=[{'target_id': 'kitchen_light', 'action_id': action['id'], 'connection_id': 'isolated_broker',
                'executor': 'mqtt', 'mqtt': {'topic': 'fixture/power/set',
                    'payload': 'ON' if action['assignments'][0]['value'] else 'OFF', 'qos': 1, 'retain': False}}
                for action in doc['actions']])
        (out / 'configuration.json').write_text(json.dumps(doc, ensure_ascii=False))
        for name in ['store', 'credentials']:
            (out / name).mkdir(mode=0o700)
        log = (out / 'http.log').open('w'); files.append(log)
        fixture = subprocess.Popen([str(args.fixture), str(out / 'store'), str(out / 'credentials'),
            str(out / 'configuration.json')], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
            text=True, bufsize=1)
        processes.append(fixture)
        listens = queue.Queue()

        def relay():
            try:
                for line in fixture.stdout:
                    event = json.loads(line)
                    if event['event'] == 'listen':
                        listens.put(event['url']); continue
                    assert event['event'] == 'model_request'
                    content = event['text'] if event['schema'] is None else json.dumps(
                        {'schema': event['schema'], 'text': event['text']}, ensure_ascii=False, separators=(',', ':'))
                    start = time.monotonic()
                    code, response = http(model_url + '/v1/chat/completions', {'messages': [
                        {'role': 'system', 'content': event['system']}, {'role': 'user', 'content': content}],
                        'max_tokens': event['budget'], 'temperature': 0, 'seed': 0,
                        'chat_template_kwargs': {'enable_thinking': False}})
                    assert code == 200, response
                    raw = response['choices'][0]['message']['content']
                    usage = response['usage']
                    if event['schema'] and event['schema'].get('task') == 'power_intent':
                        assert usage['prompt_tokens'] + event['budget'] <= 1024, 'Runtime context budget'
                    calls.append({**event, 'raw': raw, 'usage': usage,
                                  'elapsed_ms': round((time.monotonic() - start) * 1000, 2)})
                    fixture.stdin.write(json.dumps({'raw': raw, 'metrics': {
                        'output_tokens': usage['completion_tokens'], 'generation_ms': calls[-1]['elapsed_ms']}}) + '\n')
                    fixture.stdin.flush()
            except BaseException as error:
                errors.append(repr(error))
                try:
                    fixture.stdin.write(json.dumps({'error': 'real Qwen relay failed'}) + '\n'); fixture.stdin.flush()
                except OSError:
                    pass
        thread = threading.Thread(target=relay, daemon=True); thread.start()
        url = listens.get(timeout=10)
        assert url.startswith('http://127.0.0.1:')

        def api(path, payload=None, method=None, expected=200):
            code, reply = http(url + path, payload, method)
            assert code == expected and reply.get('ok'), (code, reply)
            return reply

        def drain():
            messages = []
            while select.select([observer], [], [], .25)[0]:
                kind, body = read_packet(observer)
                assert kind >> 4 == 3 and not kind & 1, 'Unexpected/retained command'
                n = struct.unpack('!H', body[:2])[0]
                assert body[2:2 + n].decode() == 'fixture/power/set'
                messages.append(body[2 + n:].decode())
            return messages

        before = api('/api/config')
        cases = [('включи свет на кухне', 'ON', False), ('выключи свет на кухне', 'OFF', False),
            ('вырубай свет на кухне', 'OFF', True), ('да будет свет на кухне', 'ON', True),
            ('пусть погаснет свет на кухне', 'OFF', True), ('врубай свет на кухне', 'ON', True),
            ('не выключай свет на кухне', None, False), ('не вырубай свет на кухне', None, False),
            ('врубай и вырубай свет на кухне', None, False), ('да будет свет на кухне в подвале', None, False),
            ('если темно да будет свет на кухне', None, False), ('через минуту вырубай свет на кухне', None, False),
            ('да будет свет на кухне 40', None, False), ('как вырубай свет на кухне', None, False),
            ('свет на кухне горит', None, False), ('свет на кухне выключен', None, False),
            ('приглуши свет на кухне', None, False), ('сделай свет на кухне ярче', None, False),
            ('почини свет на кухне', None, True), ('поменяй свет на кухне', None, False),
            ('отключи свет на кухне', 'OFF', True),
            ('пожалуйста включи свет', 'ON', False),
            ('выключи свет пожалуйста', 'OFF', False),
            ('можешь включить свет пожалуйста', 'ON', True),
            ('а можешь отключить свет на кухне', 'OFF', True),
            ('а отключи ка свет на кухне', 'OFF', True),
            ('а выключи ка свет на кухне', 'OFF', True),
            ('а отключи-ка свет на кухне', 'OFF', True),
            ('а включи-ка свет', 'ON', True),
            ('можешь свет включить на кухне пожалуйста', 'ON', True),
            ('свет на кухне можешь выключить пожалуйста', 'OFF', True),
            ('пожалуйста можешь вырубить свет на кухне', 'OFF', True),
            ('можете включить свет на кухне пожалуйста', 'ON', True),
            ('будь добр вырубай свет на кухне', 'OFF', True),
            ('можешь включить свет на кухне?', 'ON', True),
            ('можешь не включать свет на кухне', None, None),
            ('а не отключай ка свет на кухне', None, None),
            ('можешь включить и выключить свет на кухне', None, None),
            ('можешь включить свет в подвале', None, None),
            ('можешь свет в подвале включить', None, None),
            ('а можешь вырубить свет на кухне возле гаража', None, None),
            ('можешь ли включать свет на кухне', None, None),
            ('можешь включать свет на кухне?', None, None),
            ('свет на кухне включён?', None, None),
            ('скажи можешь включить свет на кухне', None, None),
            ('если можешь включи свет на кухне', None, None),
            ('можешь включить свет завтра', None, None),
            ('можешь пожалуйста свет', None, None),
            ('можешь ка включить свет', None, None)]
        for text, expected, infer in cases:
            count = len(calls)
            reply = api('/api/test-command', {'text': text, 'execute': True, 'production': True})
            assert not errors, errors
            wire = drain()
            summary = {k: reply.get(k) for k in ['result','route','language_route','llm','raw_rkllm','reason','rejection_stage','execution']}
            result['last_checked_case'] = {'text': text, 'wire': wire, **summary}
            assert wire == ([] if expected is None else [expected]), (text, wire, summary)
            invoked = len(calls) - count
            assert invoked in (0, 1), (text, 'unexpected inference retry', summary)
            if infer is not None:
                assert invoked == int(infer), (text, 'wrong inference routing', summary)
            if expected:
                assert reply['execution']['success'] and reply['result']['targetId'] == 'kitchen_light'
                assert reply['result']['actionId'] == ('action_on' if expected == 'ON' else 'action_off')
            else:
                assert reply['result'] == {'unsupported': True} and reply['execution'] is None
            assert reply['llm']['invoked'] == bool(invoked)
            tests.append({'text': text, 'expected': expected, 'wire': wire,
                          'route': reply['language_route'], 'llm': reply['llm'],
                          'result': reply['result'], 'raw_rkllm': reply.get('raw_rkllm'),
                          'expected_inference': infer, 'rejection_stage': reply['rejection_stage']})
        assert api('/api/config')['hash'] == before['hash']
        if args.chromium:
            subprocess.run(['node', str(args.root / 'tests/language/PowerIntentChromium.cjs'),
                url, str(args.chromium), str(out / 'chromium')], check=True, timeout=90)
            assert not drain(), 'Browser Preview publishes commands'
            assert api('/api/config')['hash'] == before['hash'], 'Browser Preview writes ConfigStore'
            result['chromium'] = json.loads((out / 'chromium/result.json').read_text())
        profile = api('/api/admin/language-profile', {'configuration': before['configuration'], 'entity': 'action', 'id': 'action_off'})
        aliases = profile['suggestion']['aliases']
        assert any(a in aliases for a in ['отключи', 'отключите']), aliases
        assert {'выруби', 'вырубай'} <= set(aliases), aliases
        assert {'выключи','выключите','выключить'} <= set(aliases)
        assert profile['saved'] is False and profile['applied'] is False and not drain()
        assert api('/api/config')['hash'] == before['hash'], 'Proposal writes ConfigStore'
        candidate = before['configuration']
        next(a for a in candidate['actions'] if a['id'] == 'action_off')['speech_aliases'] = aliases
        on = api('/api/admin/language-profile', {'configuration': candidate, 'entity': 'action', 'id': 'action_on'})
        on_aliases = on['suggestion']['aliases']
        assert {'включи','включите','включить'} <= set(on_aliases)
        assert {'вруби', 'врубай'} <= set(on_aliases), on_aliases
        assert on['saved'] is False and on['applied'] is False and not drain()
        assert api('/api/config')['hash'] == before['hash']
        next(a for a in candidate['actions'] if a['id'] == 'action_on')['speech_aliases'] = on_aliases
        api('/api/config/validate', {'configuration': candidate})
        saved = api('/api/config', {'configuration': candidate, 'expected': {'revision': before['revision'], 'hash': before['hash']}}, 'PUT')
        api('/api/config/apply', {'expected': {'revision': saved['revision'], 'hash': saved['hash']}})
        assert not drain(), 'Save/Apply publishes commands'
        counts = len(calls)
        word = next(a for a in ['вырубай','выруби','отключи'] if a in aliases)
        reply = api('/api/test-command', {'text': word + ' свет на кухне', 'execute': False})
        assert len(calls) == counts and reply['route'] == 'deterministic' and reply['execution'] is None
        assert not drain()
        result.update(status='PASS', real_mqtt_publish_count=sum(len(t['wire']) for t in tests),
            model_calls=len(calls), profile_aliases=aliases, on_profile_aliases=on_aliases, profile_proposal_does_not_save=True,
            save_apply_publish_count=0, enriched_alias_uses_fast_path=True,
            target_RKLLM_and_hardware='NOT_RUN; CPU GGUF acceptance is not RKLLM/physical acceptance')
    except BaseException as error:
        result.update(status='FAIL', error=repr(error), relay_errors=errors)
        raise
    finally:
        if observer:
            observer.close()
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
        for f in files:
            f.close()
        result['processes_reaped'] = all(p.poll() is not None for p in processes)
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps({k: v for k, v in result.items() if k not in ['calls', 'tests']}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
