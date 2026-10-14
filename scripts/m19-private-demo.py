#!/usr/bin/env python3
"""Prepare a private M19 browser demo. No production runtime, model or microphone.

AI replies are scripted; ON/OFF and explicit Observe use real C++/HTTP sockets.
For synthetic wake/voice proof run CTest M19DemoVerticalSlice. Live voice requires
separate owner acceptance described in docs/m19-demo.md.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import threading
import urllib.request

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--admin-port', type=int, default=18080)
    parser.add_argument('--mock-port', type=int, default=8765)
    args = parser.parse_args()
    app = args.build_dir.resolve() / 'firefly-admin'
    worker = args.build_dir.resolve() / 'src/admin/m19_test_worker'
    if not app.is_file() or not worker.is_file():
        parser.error('Build firefly-admin and m19_test_worker first; see docs/m19-demo.md')
    if not all(0 <= port <= 65535 for port in (args.admin_port, args.mock_port)):
        parser.error('Ports must be 0..65535 (0 selects a free port)')
    # Hard boundary: this launcher has no appliance/model/voice options and never
    # accepts a user-supplied ConfigStore or installed production binary.
    if Path('/opt/firefly') in app.parents or Path('/opt/firefly') in worker.parents:
        parser.error('Use an isolated test build, not an installed release')
    spec = importlib.util.spec_from_file_location('m19_mock', ROOT / 'scripts/mock-http-device.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    stopped = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stopped.set())
    process = None
    device = None
    thread = None
    with tempfile.TemporaryDirectory(prefix='firefly-m19-demo-') as temporary:
        private = Path(temporary)
        store = private / 'store'
        store.mkdir(mode=0o700)
        try:
            device = module.MockDevice(('127.0.0.1', args.mock_port))
            thread = threading.Thread(target=device.serve_forever, daemon=True)
            thread.start()
            with (private / 'server.log').open('w') as log:
                process = subprocess.Popen([
                    str(app), '--store', str(store), '--init-config', str(ROOT / 'config/empty-home.json'),
                    '--runtime-worker', str(worker), '--admin-port', str(args.admin_port),
                ], stdout=subprocess.PIPE, stderr=log, text=True,
                    env={**os.environ, 'FIREFLY_M19_VOICE_CONTROL': str(private / 'voice-control')})
                if not select.select([process.stdout], [], [], 15)[0]:
                    raise RuntimeError('Test backend did not listen')
                line = process.stdout.readline().strip()
                if not line.startswith('ADMIN_LISTEN http://127.0.0.1:'):
                    raise RuntimeError('Test backend failed: ' + (private / 'server.log').read_text())
                url = line.split()[1]
                print('M19 PRIVATE OFFLINE DEMO — scripted AI; no microphone/STT/NPU', flush=True)
                print('Откройте: ' + url + '/#/setup', flush=True)
                print(f'HTTP endpoint для wizard: http://127.0.0.1:{device.server_port}', flush=True)
                print('POST /relay/1: on = Да / Нет; GET /relay/1: on → Питание', flush=True)
                print('Private store: ' + str(store), flush=True)
                print('Инструкция: docs/m19-demo.md. Ctrl+C завершает только стенд и удаляет его private store.', flush=True)
                while not stopped.wait(.2):
                    if process.poll() is not None:
                        raise RuntimeError('Test backend exited: ' + (private / 'server.log').read_text())
                # Diagnostic read does not trigger inference, commands or Observe.
                with urllib.request.urlopen(url + '/api/status', timeout=5) as response:
                    status = json.load(response)
                print(json.dumps({'runtime_ready': status['runtime_ready'], 'http_requests': device.snapshot()}, ensure_ascii=False), flush=True)
        finally:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                process.stdout.close()
            if device is not None:
                device.shutdown()
                device.server_close()
            if thread is not None:
                thread.join(timeout=2)
    print('Изолированный стенд удалён; production не изменён.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        print('M19 demo: ' + str(error), file=sys.stderr)
        sys.exit(1)
