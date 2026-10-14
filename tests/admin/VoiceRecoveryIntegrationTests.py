"""Real DemoMain/HTTP/recovery + fake voice devices and RKLLM, private stores only."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error

app, root = map(lambda value: Path(value).resolve(), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='firefly-voice-recovery-') as directory:
    temp = Path(directory)
    control = temp / 'voice-control'

    def mode(value):
        staged = temp / 'next-mode'
        staged.write_text(value)
        staged.replace(control)

    def scenario(failure, stop_during_retry=False):
        mode('absent')
        store = temp / failure
        store.mkdir(mode=0o700)
        log = temp / (failure + '.log')
        with log.open('w') as output:
            process = subprocess.Popen([
                str(app), '--store', str(store), '--init-config', str(root / 'config/empty-home.json'),
                '--model', 'fixture-no-npu', '--admin-port', '0', '--voice',
                '--stt-model-dir', 'fixture-no-stt', '--vad-model', 'fixture-no-vad',
                '--audio-device', 'fixture-no-alsa', '--wake-python', 'fixture-no-child',
            ], stdout=output, stderr=output,
                env={**os.environ, 'FIREFLY_TEST_VOICE_CONTROL': str(control)})
        try:
            original_pid = process.pid

            def wait_for(predicate, timeout=12):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    assert process.poll() is None, log.read_text()
                    result = predicate()
                    if result:
                        return result
                    time.sleep(.01)
                raise AssertionError(log.read_text())

            def listen_url():
                return next((line.split()[1] for line in log.read_text().splitlines()
                             if line.startswith('DEMO_LISTEN ')), None)

            url = wait_for(listen_url)

            def get(path):
                with urllib.request.urlopen(url + path, timeout=2) as response:
                    assert response.status == 200
                    return json.load(response)

            def state(voice, health):
                status = get('/api/status')
                if status.get('voice') != voice or status['health']['overall'] != health:
                    return False
                assert status['runtime_ready'] is True, status
                assert status['health']['subsystems']['admin']['status'] == 'ready', status
                assert status['stt_ready'] is True, status
                assert status['worker_pid'] == original_pid == process.pid, status
                assert status['microphone'] == ('connected' if voice == 'WAITING_WAKE' else 'unavailable'), status
                assert status['health']['subsystems']['wake']['status'] == ('ready' if health == 'ready' else 'failed'), status
                return status

            wait_for(lambda: state('unavailable', 'degraded'))
            # Prove a second OPEN attempt occurs with the microphone still absent.
            wait_for(lambda: 'VOICE_RECOVERY scheduled delay_ms=2000' in log.read_text())
            before = (store / 'configuration.json').read_bytes()
            mode('healthy')
            wait_for(lambda: state('WAITING_WAKE', 'ready'))
            wait_for(lambda: 'VOICE_RECOVERY recovered' in log.read_text())
            mode('stt-failure')
            # Real Setup barrier/resource teardown/restoration. No Admin file is
            # configured: entry fails safely before any real model verification.
            request = urllib.request.Request(url + '/api/setup/start', data=b'{}',
                headers={'Content-Type': 'application/json', 'Origin': url, 'X-Firefly-Admin': '1'}, method='POST')
            try:
                urllib.request.urlopen(request, timeout=5)
                raise AssertionError('unconfigured Admin must not enter Setup')
            except urllib.error.HTTPError as error:
                assert error.code == 409
            wait_for(lambda: 'fixture STT load failure' in log.read_text())
            wait_for(lambda: state('unavailable', 'degraded'))
            mode('healthy')
            wait_for(lambda: log.read_text().count('FIXTURE STT loaded profile=command') == 2)
            wait_for(lambda: state('WAITING_WAKE', 'ready'))
            transition = log.read_text()
            assert transition.index('FIXTURE STT released profile=command') < transition.index('MODEL_RELEASED profile=runtime1024')
            assert transition.index('FIXTURE VAD released') < transition.index('MODEL_RELEASED profile=runtime1024')
            assert 'SETUP_MEMORY phase=after_restore' in transition
            assert 'FIXTURE STT released profile=admin' not in transition
            assert (store / 'configuration.json').read_bytes() == before
            assert get('/api/executions')['executions'] == []
            mode(failure)
            wait_for(lambda: state('unavailable', 'degraded'))
            if failure == 'stall':
                assert 'Wake audio stalled for 2 seconds' in log.read_text()
            mode('healthy')
            wait_for(lambda: state('WAITING_WAKE', 'ready'))
            if stop_during_retry:
                mode('absent')
                wait_for(lambda: state('unavailable', 'degraded'))
                wait_for(lambda: 'VOICE_RECOVERY scheduled delay_ms=8000' in log.read_text())
            assert (store / 'configuration.json').read_bytes() == before
            assert get('/api/executions')['executions'] == []
            with urllib.request.urlopen(url + '/', timeout=2) as response:
                assert response.status == 200
            began = time.monotonic()
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=3) == 0, log.read_text()
            assert time.monotonic() - began < 2, 'shutdown delayed by retry backoff'
            text = log.read_text()
            assert 'TEST_ONLY synthetic setup memory' in text, 'fixture must not depend on host RAM'
            assert 'SHUTDOWN complete' in text and 'stub destroyed' in text, text
            assert text.count('FIXTURE STT loaded profile=command') == 2, text
            assert text.count('FIXTURE STT loaded profile=admin') == 1, text
            assert text.count('FIXTURE VAD loaded') == 2, text
            assert text.count('FIXTURE STT released profile=command') == 2, text
            assert text.count('FIXTURE VAD released') == 2, text
            assert text.count('FIXTURE capture opened owners=1') == text.count('FIXTURE capture closed owners=0'), text
            assert text.count('FIXTURE wake created') == text.count('FIXTURE wake destroyed'), text
            assert 'Concurrent capture owner' not in text
            assert 'Wake initialization while capture is already running' not in text
            assert text.count('stub destroyed') == 2, 'only Setup reloads RKLLM; microphone retries retain it'
            print('PASS', failure, 'same PID; health recovery; retained CPU models; clean shutdown')
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)

    scenario('read-failure')
    scenario('stall', stop_during_retry=True)
