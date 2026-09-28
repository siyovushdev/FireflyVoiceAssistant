#!/usr/bin/env python3
"""Firefly V1 simulated kitchen light — loopback HTTP sink for HttpHardwareExecutor.

Does not invent a new executor. Production bindings POST brightness here;
Firefly records Predicted state on HTTP 2xx.

  python3 scripts/v1-mock-device.py
  # listens on 127.0.0.1:8765
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    last_body = b""

    def log_message(self, fmt: str, *args) -> None:
        print(f"v1-mock-device: {self.address_string()} {fmt % args}")

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        Handler.last_body = self.rfile.read(length) if length else b""
        try:
            payload = json.loads(Handler.last_body.decode("utf-8") or "{}")
        except json.JSONDecodeError:
            payload = {"raw": Handler.last_body.decode("utf-8", "replace")}
        print(f"v1-mock-device: {self.path} {payload}")
        body = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--ready-file", type=Path,
                        help="Write owned listener identity after successful bind (live helper)")
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    if args.ready_file:
        args.ready_file.write_text(json.dumps({"pid": os.getpid(), "host": args.host,
                                              "port": server.server_address[1]}) + "\n")
    print(f"v1-mock-device listening on http://{args.host}:{server.server_address[1]}/brightness")
    server.serve_forever()


if __name__ == "__main__":
    main()
