"""Actual DemoMain/HTTP/ConfigStore with fake RKLLM and absent speech paths.

Never starts the installed service, loads an NPU model, opens ALSA, or reads the
operator's store. SIGTERM targets only our own fixture processes.
"""
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ProductionStartupContractTests import arguments, unit_at

app, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
base = arguments(shlex.split(unit_at(root)['Service']['ExecStart'])[1:])

with tempfile.TemporaryDirectory(prefix='firefly-production-startup-') as directory:
    temp = Path(directory)

    def launch(store, seed, fail_model=False):
        store.mkdir(mode=0o700, exist_ok=True)
        options = {**base, '--store': str(store), '--init-config': str(seed),
                   '--admin-port': '0', '--model': 'fake-runtime-no-npu'}
        # All speech inputs deliberately absent: fail before any real models/devices.
        for flag in ['--stt-model-dir', '--vad-model', '--wake-python', '--wake-model']:
            options[flag] = str(temp / ('absent-' + flag[2:]))
        options['--audio-device'] = 'firefly_test_device_that_does_not_exist'
        options['--playback-device'] = 'firefly_test_playback_that_does_not_exist'
        argv = [str(app)]
        for flag, value in options.items():
            argv.append(flag)
            if value is not True:
                argv.append(value)
        env = dict(os.environ)
        env.pop('FIREFLY_TEST_INIT_FAIL', None)
        if fail_model:
            env['FIREFLY_TEST_INIT_FAIL'] = '1'
        log = temp / ('run-' + str(time.monotonic_ns()) + '.log')
        with log.open('w') as stream:
            process = subprocess.Popen(argv, stdout=stream, stderr=stream, env=env)
        try:
            url = None
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                assert process.poll() is None, log.read_text()
                for line in log.read_text().splitlines():
                    if line.startswith('DEMO_LISTEN '):
                        url = line.split()[1]
                if url:
                    break
                time.sleep(.02)
            assert url, log.read_text()

            def get(path):
                with urllib.request.urlopen(url + path, timeout=5) as response:
                    assert response.status == 200
                    return json.load(response)

            while time.monotonic() < deadline:
                status = get('/api/status')
                if status.get('voice') == 'unavailable' and status['health']['subsystems']['wake']['status'] == 'failed':
                    break
                time.sleep(.02)
            else:
                raise AssertionError((status, log.read_text()))
            assert process.poll() is None
            assert status['health']['overall'] == 'degraded', status
            assert status['health']['subsystems']['admin']['status'] == 'ready', status
            assert status['health']['subsystems']['wake']['status'] == 'failed', status
            assert status['runtime_ready'] == (not fail_model), status
            assert status['stt_ready'] is False, status
            assert status['health']['subsystems']['stt']['status'] in ('unavailable', 'failed'), status
            with urllib.request.urlopen(url + '/', timeout=5) as response:
                assert response.status == 200 and b'<html' in response.read()
            assert get('/api/executions')['executions'] == []
            # No child was spawned on this absent-VAD branch (Amina lifecycle is a separate test).
            for children in Path(f'/proc/{process.pid}/task').glob('*/children'):
                assert children.read_text().strip() == ''
            catalog = get('/api/config')
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=10) == 0, log.read_text()
            assert not Path(f'/proc/{process.pid}').exists()
            output = log.read_text()
            assert 'SHUTDOWN requested' in output and 'SHUTDOWN complete' in output, output
            if not fail_model:
                assert 'stub destroyed' in output, 'RKLLM RAII destruction was not reached'
            assert output.count('STARTUP voice unavailable') == 1, 'unexpected voice retry loop'
            return catalog
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    seed = root / 'config/empty-home.json'
    fresh = temp / 'fresh'
    first = launch(fresh, seed)
    for field in ['rooms', 'devices', 'actions', 'bindings']:
        assert first['configuration'][field] == [], first

    # Existing non-default catalog + --init-config empty: two restarts preserve exact bytes/hash.
    custom = json.loads(seed.read_text())
    custom['system']['name'] = 'Лаборатория владельца'
    custom['revision'] = 7
    custom_path = temp / 'custom.json'
    custom_path.write_text(json.dumps(custom, ensure_ascii=False))
    existing = temp / 'existing'
    original = launch(existing, custom_path)
    before = (existing / 'configuration.json').read_bytes()
    restarted = launch(existing, seed)
    assert restarted == original
    assert (existing / 'configuration.json').read_bytes() == before
    # Even unreadable/malformed init seed must not be loaded when configuration exists.
    invalid = temp / 'invalid-init.json'
    invalid.write_text('{invalid')
    assert launch(existing, invalid) == original
    assert (existing / 'configuration.json').read_bytes() == before

    # LLM failure plus unavailable speech still keeps Admin and the current catalog accessible.
    assert launch(existing, seed, fail_model=True) == original
    assert (existing / 'configuration.json').read_bytes() == before

print('PASS production entry: empty seed, preserved existing store on restart, degraded Admin, SIGTERM/RAII; no NPU/microphone')
