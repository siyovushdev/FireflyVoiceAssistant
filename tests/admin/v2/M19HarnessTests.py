"""Private demo launcher lifecycle: fresh store, no implicit I/O, owned cleanup."""
import json
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import time
import urllib.request

root, build = map(Path, sys.argv[1:])
process = subprocess.Popen([
    sys.executable, str(root / 'scripts/m19-private-demo.py'), '--build-dir', str(build),
    '--admin-port', '0', '--mock-port', '0',
], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    # select on text buffers can miss buffered lines, so read until the bounded
    # startup sentinel while the launcher itself enforces its listener deadline.
    assert select.select([process.stdout], [], [], 20)[0], 'launcher did not start'
    lines = []
    while True:
        line = process.stdout.readline()
        assert line, process.stderr.read()
        lines.append(line.strip())
        if line.startswith('Инструкция:'):
            break
    url = next(line.split(': ', 1)[1].removesuffix('/#/setup') for line in lines if line.startswith('Откройте:'))
    store = Path(next(line.split(': ', 1)[1] for line in lines if line.startswith('Private store:')))
    private = store.parent
    assert private.name.startswith('firefly-m19-demo-') and private.parent == Path('/tmp')
    assert stat.S_IMODE(store.stat().st_mode) == 0o700

    def api(path):
        with urllib.request.urlopen(url + path, timeout=3) as response:
            return json.load(response)

    deadline = time.monotonic() + 10
    while not api('/api/status')['runtime_ready']:
        assert time.monotonic() < deadline
        time.sleep(.02)
    config = api('/api/config')['configuration']
    assert config['devices'] == config['rooms'] == config['actions'] == []
    assert api('/api/executions')['executions'] == []
    assert not api('/api/setup/status')['active']
    before = (store / 'configuration.json').read_bytes()
    process.send_signal(signal.SIGTERM)
    remaining, error = process.communicate(timeout=15)
    assert process.returncode == 0, (remaining, error)
    summary = next(json.loads(line) for line in remaining.splitlines() if line.startswith('{'))
    assert summary['runtime_ready'] and summary['http_requests'] == []
    assert before and not private.exists(), 'owned temporary store not cleaned'
    print('PASS private demo: empty 0700 store; actual C++ READY; zero HTTP; SIGTERM cleanup; no NPU/microphone')
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    process.stdout.close()
    process.stderr.close()

# Exact CLI mock log is usable during the separately authorized live acceptance.
import socket
import tempfile
with tempfile.TemporaryDirectory(prefix='firefly-m19-log-') as directory:
    request_log = Path(directory) / 'http.jsonl'
    mock = subprocess.Popen([sys.executable, str(root / 'scripts/mock-http-device.py'),
                             '--port', '0', '--request-log', str(request_log)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([mock.stdout], [], [], 5)[0]
        line = mock.stdout.readline().strip()
        assert line.startswith('MOCK_LISTEN '), line
        endpoint = line.split()[1]
        port = int(endpoint.split('/')[2].split(':')[1])
        with socket.create_connection(('127.0.0.1', port), timeout=2):
            pass
        assert request_log.read_text() == '', 'TCP probe must not be logged as HTTP'
        for on in (True, False):
            request = urllib.request.Request(endpoint, json.dumps({'on': on}).encode(),
                                             headers={'Content-Type': 'application/json'})
            with urllib.request.urlopen(request, timeout=2) as response:
                assert json.load(response) == {'on': on}
        with urllib.request.urlopen(endpoint, timeout=2) as response:
            assert json.load(response) == {'on': False}
        rows = [json.loads(line) for line in request_log.read_text().splitlines()]
        assert [row['method'] for row in rows] == ['POST', 'POST', 'GET']
        assert all(row['path'] == '/relay/1' for row in rows)
        assert [json.loads(row['body'])['on'] for row in rows[:2]] == [True, False]
        before = request_log.read_bytes()
        duplicate = subprocess.run([sys.executable, str(root / 'scripts/mock-http-device.py'),
                                    '--port', '0', '--request-log', str(request_log)],
                                   capture_output=True, timeout=5)
        assert duplicate.returncode != 0 and request_log.read_bytes() == before
        mock.send_signal(signal.SIGINT)
        _, errors = mock.communicate(timeout=5)
        assert mock.returncode == 0, errors
        print('PASS mock JSONL: TCP=0 entries; POST=2 GET=1; exact booleans; refuses overwrite')
    finally:
        if mock.poll() is None:
            mock.terminate()
            mock.wait(timeout=5)
        mock.stdout.close()
        mock.stderr.close()
