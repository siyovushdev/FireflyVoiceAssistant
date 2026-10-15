#!/usr/bin/env python3
"""Error-evidence contract for the existing stress CLI; offline PC HTTP fixture only."""
import base64
import ast
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
import urllib.request
import urllib.error
from types import SimpleNamespace
from unittest.mock import Mock

sys.dont_write_bytecode = True
CLIENT = Path(__file__).resolve().parents[1] / 'benchmarks/web-admin-b4/DialogueSmoke.py'


class StressEvidenceTests(unittest.TestCase):
    def run_failure(self, response, status=409, remove_trace=False, reset_body=False, diagnostic_only=False):
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
                    '--deadline-monotonic', str(time.monotonic() + 30),
                    *(['--diagnostic-only'] if diagnostic_only else [])], capture_output=True, text=True, timeout=10)
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
            self.assertEqual(case['case'], 'long' if diagnostic_only else 'control')
            self.assertEqual(case['input_bytes'], 1606 if diagnostic_only else 136)
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

    def test_diagnostic_sends_only_one_long_request_and_keeps_failure(self):
        report=self.run_failure(b'{"ok":false}',diagnostic_only=True)
        self.assertEqual(report['profile'],'ADMIN4096_DIAGNOSTIC')
        self.assertEqual(len(report['cycles']),1)
        self.assertEqual(len(report['cycles'][0]['cases']),1)


class RepeatabilityTests(unittest.TestCase):
    def client_function(self, name, namespace):
        # Load only an existing nested helper; importing the legacy CLI would
        # execute its separate live entrypoint. No backend or hardware here.
        module = ast.parse(CLIENT.read_text())
        external = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == 'external_stress')
        function = next(n for n in external.body if isinstance(n, ast.FunctionDef) and n.name == name)
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(CLIENT), 'exec'), namespace)
        return namespace[name]

    def test_restore_uses_remaining_original_and_phase_deadline(self):
        clock = SimpleNamespace(monotonic=lambda: 100.)
        opener = Mock(side_effect=RuntimeError('stop after timeout observation'))
        namespace = {'deadline': 1000., 'time': clock, 'report': {'requests': []},
            'allowed_get': {'/api/status'}, 'allowed_post': set(),
            'args': SimpleNamespace(stress_url='http://127.0.0.1:1234', repeatability_only=True),
            'urllib': SimpleNamespace(request=SimpleNamespace(Request=urllib.request.Request, urlopen=opener),
                error=urllib.error), 'json': json}
        call = self.client_function('call', namespace)
        with self.assertRaisesRegex(RuntimeError, 'timeout observation'):
            call('/api/status', operation_deadline=110.)
        self.assertEqual(opener.call_args.kwargs['timeout'], 10.)
        opener.reset_mock(); clock.monotonic = lambda: 111.
        with self.assertRaises(TimeoutError): call('/api/status', operation_deadline=110.)
        opener.assert_not_called()

    def test_ready_after_shared_restore_deadline_cannot_pass(self):
        ticks = [100.]
        def late_ready(deadline):
            self.assertEqual(deadline, 110.); ticks[0] = 111.; return True
        namespace = {'deadline': 1000., 'time': SimpleNamespace(monotonic=lambda: ticks[0]), 'ready': late_ready}
        wait = self.client_function('wait_ready', namespace)
        with self.assertRaisesRegex(TimeoutError, 'arrived after restore deadline'): wait(110.)

    @staticmethod
    def proposal():
        return {'status': 'editing', 'message_incomplete': False, 'questions': [], 'version': 3,
            'configuration': {'system': {'name': 'Firefly'}, 'rooms': [{'id': 'room', 'name': 'Кабинет'}],
                'devices': [{'name': 'Лампа', 'room_id': 'room', 'kind': 'generic',
                    'state': [{'id': 'power', 'type': 'boolean'}], 'action_ids': ['on', 'off']}],
                'actions': [{'id': name, 'enabled': True, 'parameters': [],
                    'assignments': [{'field': 'power', 'value': value}]} for name, value in [('on', True), ('off', False)]],
                'bindings': []}}

    def run_profile(self, mutate=None, restore_failure=False, tight_deadline=False, stage_delay=None):
        with tempfile.TemporaryDirectory(prefix='m19-repeatability-offline-') as temp:
            root = Path(temp); (root/'offline-test-marker').touch()
            store = root/'store'; store.mkdir(); mock = root/'mock.jsonl'; mock.touch()
            output = root/'report.json'; calls = []; cancelled = []
            state = self.proposal()
            if mutate: mutate(state)
            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_): pass
                def send(self, value, status=200):
                    raw = json.dumps(value).encode(); self.send_response(status)
                    self.send_header('Content-Length', str(len(raw))); self.end_headers(); self.wfile.write(raw)
                def do_GET(self):
                    calls.append(('GET', self.path))
                    if stage_delay == 'resources' and self.path == '/api/status' and calls.count(('GET', '/api/status')) == 3:
                        time.sleep(1.6)
                    if self.path == '/api/status' and cancelled and restore_failure:
                        self.send({'error': 'restore failed'}, 503); return
                    self.send({'/api/status': {'runtime_ready': True, 'worker_pid': os.getpid()},
                        '/api/config': {'hash': 'offline', 'configuration': {'system': {'name': 'Firefly'}}},
                        '/api/config/active': {'active': {'hash': 'offline'}},
                        '/api/executions': {'executions': []}}[self.path])
                def do_POST(self):
                    calls.append(('POST', self.path)); body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    if self.path == '/api/setup/start': self.send({'status': 'editing', 'session_id': 'private', 'version': 1})
                    elif self.path == '/api/setup/draft':
                        if stage_delay == 'draft': time.sleep(1.6)
                        (store/'setup-draft.json').write_text(json.dumps({'focus': [], 'unresolved_text': '', 'trace': []}))
                        self.send({'status': 'editing', 'version': 2})
                    elif self.path == '/api/setup/message':
                        assert len(body['text'].encode()) == 1606
                        if tight_deadline: time.sleep(1.6)
                        (store/'setup-draft.json').write_text(json.dumps({'trace': [], 'status': state['status'], 'message_incomplete': state['message_incomplete']}))
                        self.send(state)
                    elif self.path == '/api/setup/cancel': cancelled.append(True); self.send({'active': False})
                    else: self.send({'unexpected_mutation': self.path}, 500)
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            try:
                result = subprocess.run([sys.executable, '-B', str(CLIENT), '--repeatability-only', '--offline',
                    '--stress-url', f'http://127.0.0.1:{server.server_port}', '--store', str(store),
                    '--mock-log', str(mock), '--output', str(output), '--deadline-monotonic',
                    str(time.monotonic() + (6 if tight_deadline or stage_delay else 30))], capture_output=True, text=True, timeout=10)
            finally: server.shutdown(); server.server_close(); thread.join(timeout=2)
            report = json.loads(output.read_text())
            self.assertFalse(report['real_rkllm']); self.assertFalse(report['full_4096_context_verified'])
            self.assertTrue(report['repeatability_only']); self.assertFalse(report['diagnostic_only'])
            self.assertEqual(mock.read_bytes(), b'')
            self.assertFalse(any(p in ('/api/setup/apply', '/api/config/apply', '/api/test/command') for _, p in calls))
            return result, report, calls

    def test_two_strict_proposals_and_two_runtime_restores(self):
        result, report, calls = self.run_profile()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(report['passed']); self.assertEqual(len(report['cycles']), 2)
        self.assertTrue(all(c['restored'] and len(c['cases']) == 1 for c in report['cycles']))
        for path in ('/api/setup/start', '/api/setup/message', '/api/setup/cancel'):
            self.assertEqual(calls.count(('POST', path)), 2)
        self.assertEqual(report['apply_calls'], 0); self.assertEqual(report['device_requests'], 0)

    def test_clarification_is_not_a_repeatability_pass(self):
        result, report, calls = self.run_profile(lambda s: s.update(status='clarification_required'))
        self.assertNotEqual(result.returncode, 0); self.assertFalse(report['passed'])
        self.assertEqual(calls.count(('POST', '/api/setup/message')), 1)
        self.assertEqual(calls.count(('POST', '/api/setup/start')), 1)

    def test_wrong_capability_and_system_are_not_proposals(self):
        for mutate in [lambda s: s['configuration']['devices'][0]['state'][0].update(type='integer'),
                       lambda s: s['configuration']['system'].update(name='Кабинет')]:
            result, report, calls = self.run_profile(mutate)
            self.assertNotEqual(result.returncode, 0); self.assertFalse(report['passed'])
            self.assertEqual(calls.count(('POST', '/api/setup/message')), 1)

    def test_restore_failure_never_starts_second_cycle(self):
        result, report, calls = self.run_profile(restore_failure=True)
        self.assertNotEqual(result.returncode, 0); self.assertFalse(report['passed'])
        self.assertFalse(report['cycles'][0]['restored'])
        self.assertEqual(calls.count(('POST', '/api/setup/start')), 1)

    def test_original_deadline_blocks_second_cycle_without_extension(self):
        result, report, calls = self.run_profile(tight_deadline=True)
        self.assertNotEqual(result.returncode, 0); self.assertFalse(report['passed'])
        self.assertTrue(report['cycles'][0]['restored']); self.assertEqual(len(report['cycles']), 1)
        self.assertEqual(calls.count(('POST', '/api/setup/start')), 1)
        self.assertIn('insufficient original window', report['error'])

    def test_modes_are_mutually_exclusive_before_backend_access(self):
        result = subprocess.run([sys.executable, '-B', str(CLIENT), '--validate-workload',
            '--repeatability-only', '--diagnostic-only'], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 2)

    def test_slow_resources_prevent_setup_start(self):
        result, report, calls = self.run_profile(stage_delay='resources')
        self.assertNotEqual(result.returncode, 0); self.assertFalse(report['passed'])
        self.assertEqual(calls.count(('POST', '/api/setup/start')), 0)
        self.assertIn('after resources', report['error'])

    def test_slow_draft_prevents_message_submission(self):
        result, report, calls = self.run_profile(stage_delay='draft')
        self.assertNotEqual(result.returncode, 0); self.assertFalse(report['passed'])
        self.assertEqual(calls.count(('POST', '/api/setup/start')), 1)
        self.assertEqual(calls.count(('POST', '/api/setup/message')), 0)
        self.assertIn('after draft', report['error'])


if __name__ == '__main__': unittest.main(verbosity=2)
