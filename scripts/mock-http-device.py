#!/usr/bin/env python3
"""Loopback device for Connection/Binding acceptance. No real hardware.

POST /relay/1 {"on": true|false}; GET /relay/1 -> {"on": bool}.
The request log and fault modes are available in-process to integration tests.
"""
import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockDevice(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address=('127.0.0.1', 0)):
        super().__init__(address, Handler)
        self.on = False
        self.requests = []
        self.mode = ''
        self.lock = threading.Lock()

    def snapshot(self):
        with self.lock:
            return list(self.requests)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def handle_request(self):
        length = int(self.headers.get('Content-Length', 0))
        if length > 16384:
            self.send_error(413)
            return
        raw = self.rfile.read(length)
        with self.server.lock:
            self.server.requests.append((self.command, self.path, raw.decode()))
            mode = self.server.mode
        if mode == 'timeout':
            time.sleep(.3)
        status = int(mode) if mode in ('400', '500', '302', '201') else 200
        if self.path != '/relay/1':
            status = 404
        elif self.command == 'POST' and status == 200:
            try:
                data = json.loads(raw)
                if set(data) != {'on'} or type(data['on']) is not bool:
                    raise ValueError('Invalid command')
                self.server.on = data['on']
            except (ValueError, TypeError):
                status = 400
        body = json.dumps({'on': self.server.on}).encode()
        if self.command == 'GET':
            if mode == 'malformed': body = b'{'
            if mode == 'wrong_type': body = b'{"on":"true"}'
            if mode == 'duplicate': body = b'{"on":true,"on":false}'
            if mode == 'missing': body = b'{}'
        if mode == 'oversize': body = b'x' * 70000
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Connection', 'close')
        if mode == 'oversize_headers': self.send_header('X-Padding', 'x' * 9000)
        if mode == '302': self.send_header('Location', '/relay/1')
        if mode == 'chunked': self.send_header('Transfer-Encoding', 'chunked')
        else: self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            if mode == 'chunked':
                for part in [body[:4], body[4:]]:
                    self.wfile.write(f'{len(part):x}\r\n'.encode() + part + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            elif mode == 'fragmented':
                for part in [body[:3], body[3:]]:
                    self.wfile.write(part)
                    self.wfile.flush()
                    time.sleep(.02)
            elif mode == 'truncated': self.wfile.write(body[:3])
            else: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    do_POST = handle_request
    do_GET = handle_request


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    server = MockDevice(('127.0.0.1', args.port))
    print(f'MOCK_LISTEN http://127.0.0.1:{server.server_port}/relay/1', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
