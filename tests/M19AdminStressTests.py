#!/usr/bin/env python3
"""Error-evidence contract for the existing stress CLI; offline PC HTTP fixture only."""
import base64
import http.server
import json
import os
import socket
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.dont_write_bytecode = True
CLIENT = Path(__file__).resolve().parents[1] / 'benchmarks/web-admin-b4/DialogueSmoke.py'


class StressEvidenceTests(unittest.TestCase):
    def run_failure(self, response, status=409, remove_trace=False, reset_body=False):
        with tempfile.TemporaryDirectory(prefix='m19-stress-error-') as temp:
            root = Path(temp)
            (root / 'offline-test-marker').touch()
            store = root / 'store'; store.mkdir()
            mock = root / 'mock.jsonl'; mock.touch()
            output = root / 'report.json'
            calls = []
            draft = store / 'setup-draft.json'
            failure_trace = {'attempt': 1, 'stage': 'decode', 'error_code': 'invalid_ai_response',
                             'rejected_raw_hex': b'{broken'.hex(), 'rejected_raw_bytes': 7,
                             'rejected_raw_truncated': False}

            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_): pass

                def send(self, value, code=200):
                    raw = value if isinstance(value, bytes) else json.dumps(value).encode()
                    self.send_response(code)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(raw)))
                    self.end_headers()
                    if reset_body and code==status:
                        self.wfile.flush()
                        self.connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                        self.close_connection=True
                        return
                    self.wfile.write(raw)

                def do_GET(self):
                    calls.append(('GET', self.path))
                    values = {'/api/status': {'runtime_ready': True, 'worker_pid': os.getpid()},
                              '/api/config': {'hash': 'offline', 'configuration': {}},
                              '/api/config/active': {'active': {'hash': 'offline'}},
                              '/api/executions': {'executions': []}}
                    self.send(values[self.path])

                def do_POST(self):
                    calls.append(('POST', self.path))
                    self.rfile.read(int(self.headers['Content-Length']))
                    if self.path == '/api/setup/start':
                        self.send({'status': 'editing', 'session_id': 'offline-fixture', 'version': 1})
                    elif self.path == '/api/setup/draft':
                        draft.write_text(json.dumps({'focus': [], 'unresolved_text': '', 'trace': []}))
                        self.send({'status': 'editing', 'version': 2})
                    elif self.path == '/api/setup/message':
                        if remove_trace: draft.unlink()
                        else: draft.write_text(json.dumps({'trace': [failure_trace], 'status': 'planning_failed', 'message_incomplete': True}))
                        self.send(response, status)
                    else:
                        self.send({'unexpected_mutation': self.path}, 500)

            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            try:
                result = subprocess.run([sys.executable, '-B', str(CLIENT), '--offline',
                    '--stress-url', f'http://127.0.0.1:{server.server_port}', '--store', str(store),
                    '--mock-log', str(mock), '--output', str(output),
                    '--deadline-monotonic', str(time.monotonic() + 30)], capture_output=True, text=True, timeout=10)
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=2)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(f'HTTP Error {status}', result.stderr)
            report = json.loads(output.read_text())
            self.assertFalse(report['passed'])
            self.assertFalse(report['real_rkllm'])
            self.assertFalse(report['full_4096_context_verified'])
            self.assertEqual(report['http_error']['status'], status)
            self.assertEqual(report['http_error']['path'], '/api/setup/message')
            self.assertEqual(calls.count(('POST', '/api/setup/message')), 1, 'mutation never retried')
            self.assertTrue(all(path in ('/api/setup/start', '/api/setup/draft', '/api/setup/message') for method, path in calls if method == 'POST'))
            self.assertEqual(mock.read_bytes(), b'')
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            case = report['cycles'][0]['cases'][0]
            self.assertEqual(case['case'], 'control')
            self.assertIsNone(case['state'])
            self.assertIn('wall_seconds',case,result.stderr)
            self.assertGreaterEqual(case['wall_seconds'], 0)
            if remove_trace: self.assertIn('trace_capture_error', case)
            else:
                self.assertEqual(case['private_status'], 'planning_failed')
                self.assertTrue(case['message_incomplete'])
                self.assertEqual(case['trace'], [failure_trace])
            return report

    def test_structured_conflict_and_failed_case_are_retained(self):
        raw = json.dumps({'ok': False, 'errors': [{'code': 'setup_failed', 'message': '[{"code":"invalid_ai_response"}]'}]}).encode()
        report = self.run_failure(raw)
        self.assertEqual(base64.b64decode(report['http_error']['body_base64']), raw)
        self.assertEqual(report['http_error']['body_json'], json.loads(raw))
        self.assertFalse(report['http_error']['truncated'])

    def test_non_json_invalid_utf8_body_is_lossless(self):
        raw = b'upstream error\xff\xfe\xd0'
        report = self.run_failure(raw)
        self.assertEqual(base64.b64decode(report['http_error']['body_base64']), raw)
        self.assertNotIn('body_json', report['http_error'])

    def test_oversized_body_is_bounded_and_labelled(self):
        raw = b'x' * (70 * 1024)
        report = self.run_failure(raw)
        self.assertTrue(report['http_error']['truncated'])
        self.assertEqual(report['http_error']['captured_bytes'], 64 * 1024)
        self.assertEqual(base64.b64decode(report['http_error']['body_base64']), raw[:64 * 1024])

    def test_trace_capture_failure_does_not_mask_http_error(self):
        self.run_failure(b'{"ok":false}', remove_trace=True)

    def test_service_error_is_not_retried_or_reported_as_model_success(self):
        self.run_failure(b'{"ok":false,"error":"unavailable"}', status=503)

    def test_lone_surrogate_json_does_not_mask_http_error(self):
        raw = b'{"error":"\\ud800"}'
        report = self.run_failure(raw)
        self.assertEqual(base64.b64decode(report['http_error']['body_base64']), raw)

    def test_connection_reset_during_body_preserves_original_conflict(self):
        report=self.run_failure(b'x'*100000,reset_body=True)
        self.assertFalse(report['http_error']['capture_complete'])
        self.assertIn('body_capture_error',report['http_error'])

    def test_deep_json_body_does_not_mask_original_conflict(self):
        raw=b'['*1500+b'0'+b']'*1500
        report=self.run_failure(raw)
        self.assertIn(report['http_error']['body_parse_error'],('RecursionError','ValueError'))
        self.assertEqual(base64.b64decode(report['http_error']['body_base64']),raw)


if __name__ == '__main__': unittest.main(verbosity=2)
