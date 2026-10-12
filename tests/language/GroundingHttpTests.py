"""Adversarial offline inference + real C++ command lanes + loopback HTTP observer.

This is not a Qwen/NPU/acoustic acceptance. The original live configuration's
semantic fixture has no endpoint; each run supplies a disposable loopback mock.
"""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APP, ROOT = sys.argv[1:]

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def handle_request(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        with self.server.lock:
            self.server.requests.append((self.command, self.path, body))
        try:
            data = json.loads(body)
        except ValueError:
            data = None
        valid = (self.command == 'POST' and self.path.startswith('/relay/device_')
                 and isinstance(data, dict) and set(data) == {'on'} and type(data['on']) is bool)
        result = b'{}'
        self.send_response(200 if valid else 400)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(result)))
        self.end_headers()
        self.wfile.write(result)

    do_POST = handle_request
    do_GET = handle_request


class MockDevice(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.requests = []
        self.lock = threading.Lock()

    def snapshot(self):
        with self.lock:
            return list(self.requests)

with tempfile.TemporaryDirectory(prefix='firefly-grounding-http-') as directory:
    device = MockDevice()
    thread = threading.Thread(target=device.serve_forever, daemon=True)
    thread.start()
    process = None
    reports = []
    try:
        with open(Path(directory) / 'runtime.log', 'w+') as log:
            process = subprocess.Popen([APP, ROOT, str(device.server_port)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=log, text=True)
            count = 0
            while True:
                assert select.select([process.stdout], [], [], 20)[0], 'C++ case timeout'
                line = process.stdout.readline()
                if not line:
                    break
                report = json.loads(line)
                requests = device.snapshot()
                delta = requests[count:]
                report['observed_http'] = delta
                reports.append(report)
                assert report['pass'], report
                assert len(delta) == report['expected_post_count'], report
                if delta:
                    method, path, body = delta[0]
                    assert method == 'POST' and path == '/relay/' + report['expected_target'], report
                    assert json.loads(body) == {'on': report['expected_on']}, report
                count = len(requests)
                process.stdin.write('checked\n')
                process.stdin.flush()
            process.stdin.close()
            result = process.wait(timeout=10)
            log.seek(0)
            assert result == 0, log.read()
            assert len(reports) == 450, len(reports)
            positives = sum(r['expected_post_count'] for r in reports)
            print(f'PASS {len(reports)} authorization cases; {positives} validated POST; all rejected cases zero outbound HTTP; no NPU/mic')
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process:
            process.stdout.close()
            if not process.stdin.closed:
                process.stdin.close()
        device.shutdown()
        device.server_close()
        thread.join(timeout=2)
        evidence = os.environ.get('FIREFLY_GROUNDING_EVIDENCE')
        if evidence:
            Path(evidence).write_text(json.dumps({'scope': 'offline scripted inference / synthetic recognition, real C++ HTTP execution',
                'cases': reports, 'request_log': device.snapshot()}, ensure_ascii=False, indent=2) + '\n')
