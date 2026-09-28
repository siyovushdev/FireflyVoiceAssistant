"""Owned mock readiness must never be satisfied by someone else's TCP listener."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

root = Path(sys.argv.pop(1))
mock = root / 'scripts/v1-mock-device.py'


class Tests(unittest.TestCase):
    def test_ready_only_after_owned_bind(self):
        with tempfile.TemporaryDirectory() as directory:
            ready = Path(directory) / 'ready.json'
            process = subprocess.Popen([sys.executable, str(mock), '--port', '0', '--ready-file', str(ready)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                value = None
                while time.monotonic() < deadline:
                    self.assertIsNone(process.poll())
                    try:
                        value = json.loads(ready.read_text())
                        break
                    except (FileNotFoundError, json.JSONDecodeError):
                        time.sleep(.02)
                self.assertIsNotNone(value)
                self.assertEqual(value['pid'], process.pid)
                with socket.create_connection((value['host'], value['port']), timeout=1):
                    pass
            finally:
                process.terminate()
                process.communicate(timeout=5)

    def test_occupied_port_never_signals_ready(self):
        with tempfile.TemporaryDirectory() as directory, socket.socket() as other:
            other.bind(('127.0.0.1', 0))
            other.listen()
            ready = Path(directory) / 'ready.json'
            result = subprocess.run([sys.executable, str(mock), '--port', str(other.getsockname()[1]),
                                     '--ready-file', str(ready)], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(ready.exists())
            # The unrelated listener remains usable and owned by this test.
            with socket.create_connection(other.getsockname(), timeout=1):
                pass


unittest.main()
