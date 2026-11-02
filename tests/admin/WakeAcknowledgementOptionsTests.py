"""Real startup options/health/wake gating, fake devices/models, private stores."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

app, root = (Path(value).resolve() for value in sys.argv[1:])
invalid = [
    ['--wake-beep-enabled', value] for value in ('', '0', '1', 'TRUE', 'yes')
] + [
    ['--wake-beep-volume-percent', value]
    for value in ('', '-1', '0', '31', '100', 'nan', 'inf', '30.0', '1e1', '+30', ' 30', '30 ', '30x', '999999999999999999999')
] + [['--wake-beep-enabled'], ['--wake-beep-volume-percent'], ['--wake-command-mode']] + [
    ['--wake-command-mode',value] for value in ('','on','AUTO','inline','after_beep')
] + [['--voice','--wake-beep-enabled','false']]

with tempfile.TemporaryDirectory(prefix='firefly-beep-options-') as directory:
    temp = Path(directory)
    store = temp / 'must-not-be-created'
    for options in invalid:
        result = subprocess.run([str(app), '--store', str(store), *options],
                                text=True, capture_output=True, timeout=3)
        assert result.returncode == 1, (options, result)
        assert 'Demo startup:' in result.stderr, (options, result.stderr)
        assert not store.exists(), 'Invalid options touched ConfigStore'
        assert 'FIXTURE' not in result.stderr and 'stub init' not in result.stderr, result.stderr

    help_result = subprocess.run([str(app), '--help'], text=True, capture_output=True, timeout=3)
    assert help_result.returncode == 0
    for flag in ('--wake-beep-enabled', '--wake-beep-volume-percent', '--wake-command-mode', 'default: after-beep'):
        assert flag in help_result.stdout

    scenarios = [
        ('default', [], 'ack-success', True, 30, 'ready'),
        ('quiet', ['--wake-beep-volume-percent', '1'], 'ack-success', True, 1, 'ready'),
        ('enabled', ['--wake-beep-enabled', 'true', '--wake-beep-volume-percent', '12'], 'ack-success', True, 12, 'ready'),
        ('disabled-auto', ['--wake-command-mode','auto','--wake-beep-enabled', 'false'], 'ack-success', False, 30, 'ready'),
        ('failed-auto', ['--wake-command-mode','auto'], 'ack-failure', True, 30, 'degraded'),
        ('failed-default', [], 'ack-failure', True, 30, 'degraded'),
    ]
    for name, options, mode, enabled, volume, health in scenarios:
        control = temp / (name + '-control')
        control.write_text(mode)
        log = temp / (name + '.log')
        store = temp / name
        store.mkdir(mode=0o700)
        with log.open('w') as output:
            process = subprocess.Popen([
                str(app), '--store', str(store), '--init-config', str(root / 'config/empty-home.json'),
                '--model', 'fixture-no-npu', '--admin-port', '0', '--voice',
                '--stt-model-dir', 'fixture-no-stt', '--vad-model', 'fixture-no-vad',
                '--audio-device', 'fixture-no-alsa', '--wake-python', 'fixture-no-child', *options,
            ], stdout=output, stderr=output,
                env={**os.environ, 'FIREFLY_TEST_VOICE_CONTROL': str(control)})
        try:
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                text = log.read_text()
                assert process.poll() is None, text
                if name=='failed-default':
                    if 'VOICE_STATE ACKNOWLEDGEMENT_FAILED' in text and text.rfind('VOICE_STATE WAITING_WAKE')>text.rfind('VOICE_STATE ACKNOWLEDGEMENT_FAILED'):
                        break
                elif 'VOICE_STATE LISTENING_COMMAND' in text:
                    break
                time.sleep(.01)
            else:
                raise AssertionError(text)
            assert 'VOICE_STATE WAKE_DETECTED' in text and 'CONTINUATION_SEPARATED' in text, text
            assert f'VOICE_ACK enabled={str(enabled).lower()} volume_percent={volume}' in text, text
            assert f'VOICE_INPUT mode={"auto" if name.endswith("-auto") else "after-beep"}' in text, text
            if name=='failed-default':
                assert 'VOICE_STATE LISTENING_COMMAND' not in text, text
            assert ('FIXTURE beep started' in text) == enabled, text
            assert ('VOICE_STATE BEEP_START' in text) == enabled, text
            assert ('VOICE_STATE ACKNOWLEDGEMENT_FAILED' in text) == (mode == 'ack-failure'), text
            if mode == 'ack-failure':
                assert 'VOICE_ACK failure=fixture playback failure' in text, text
            url = next(line.split()[1] for line in text.splitlines() if line.startswith('DEMO_LISTEN '))
            with urllib.request.urlopen(url + '/api/status', timeout=2) as response:
                status = json.load(response)
            assert status['health']['overall'] == health, status
            assert status['health']['subsystems']['wake']['status'] == 'ready', status
            assert status['runtime_ready'] is True and status['stt_ready'] is True, status
            if mode == 'ack-failure':
                assert status['health']['subsystems']['acknowledgement']['status'] == 'failed', status
                assert status['health']['subsystems']['acknowledgement']['error'] == 'playback_failed', status
            with urllib.request.urlopen(url + '/api/executions', timeout=2) as response:
                assert json.load(response)['executions'] == []
            before = (store / 'configuration.json').read_bytes()
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=3) == 0, log.read_text()
            assert (store / 'configuration.json').read_bytes() == before
            assert 'VOICE_RECOVERY failure' not in log.read_text(), log.read_text()
            assert 'SHUTDOWN complete' in log.read_text(), log.read_text()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)

print(f'{len(invalid)} invalid argv cases, help and {len(scenarios)} startup/health/wake cases passed; no hardware/NPU')
