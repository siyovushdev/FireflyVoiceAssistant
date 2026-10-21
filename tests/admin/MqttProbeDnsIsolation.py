#!/usr/bin/env python3
"""Actual slow/absent libc DNS through HttpServer, in private user/net/mount NS.

No sudo, packages, host DNS changes, systemd, production store or devices.
The outer process always invokes unshare before mounting or opening DNS port 53.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ProcChildren import children


class Dns:
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 53))
        self.socket.settimeout(0.1)
        self.stopping = False
        self.queries = []
        self.workers = []
        self.arrived = threading.Event()
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def serve(self):
        while not self.stopping:
            try:
                data, address = self.socket.recvfrom(4096)
            except socket.timeout:
                continue
            worker = threading.Thread(target=self.answer, args=(data, address))
            self.workers.append(worker)
            worker.start()

    def answer(self, data, address):
        end, labels = 12, []
        while data[end]:
            size = data[end]
            labels.append(data[end + 1:end + 1 + size].decode("ascii"))
            end += size + 1
        end += 1
        name = ".".join(labels)
        kind = int.from_bytes(data[end:end + 2], "big")
        self.queries.append({"host": name, "type": kind})
        self.arrived.set()
        if name != "probe-delay.review.invalid":
            return  # A real resolver request which never receives an answer.
        time.sleep(1.25)
        question = data[12:end + 4]
        answer = (b"\xc0\x0c\x00\x01\x00\x01" + struct.pack("!IH", 0, 4) +
                  socket.inet_aton("127.0.0.1")) if kind == 1 else b""
        packet = (data[:2] + b"\x81\x80\x00\x01" +
                  (b"\x00\x01" if answer else b"\x00\x00") + b"\x00\x00\x00\x00" +
                  question + answer)
        self.socket.sendto(packet, address)

    def close(self):
        self.stopping = True
        self.thread.join()
        for worker in self.workers:
            worker.join()
        self.socket.close()


class Server:
    def __init__(self, executable, root, directory):
        self.directory = directory
        for name in ("store", "credentials"):
            (directory / name).mkdir(mode=0o700)
        self.audit = directory / "process.jsonl"
        self.log = (directory / "server.log").open("w")
        self.process = subprocess.Popen(
            [str(executable), str(directory / "store"), str(directory / "credentials"),
             str(root / "config/empty-home.json"), str(self.audit)],
            stdout=subprocess.PIPE, stderr=self.log, text=True)
        try:
            assert select.select([self.process.stdout], [], [], 4)[0], "HttpServer startup timeout"
            line = self.process.stdout.readline().strip()
            assert line.startswith("ADMIN_LISTEN http://127.0.0.1:"), line
            self.url = line.split()[-1]
        except BaseException:
            self.close()
            raise

    def request(self, method, route, body=None):
        request = urllib.request.Request(
            self.url + route, method=method,
            data=None if body is None else json.dumps(body).encode(),
            headers={"Content-Type": "application/json", "X-Firefly-Admin": "1"})
        try:
            response = urllib.request.urlopen(request, timeout=4)
        except urllib.error.HTTPError as error:
            response = error
        return response.status, json.loads(response.read())

    def probe(self, port, host="127.0.0.1", timeout=100, credentials=""):
        connection = {"id": "probe", "name": "Temporary probe", "type": "mqtt",
                      "enabled": True, "timeout_ms": timeout,
                      "network": {"host": host, "port": port}, "mqtt": {"tls": False}}
        if credentials:
            connection["mqtt"]["credentials_id"] = credentials
        start = time.monotonic()
        status, reply = self.request("POST", "/api/connections/probe", {"connection": connection})
        return {"status": status, "reply": reply,
                "elapsed_ms": (time.monotonic() - start) * 1000}

    def snapshot(self):
        files = {str(path.relative_to(self.directory / "store")):
                 hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in (self.directory / "store").rglob("*") if path.is_file()}
        return {"files": files, "stored": self.request("GET", "/api/config"),
                "active": self.request("GET", "/api/config/active"),
                "state": self.request("GET", "/api/devices/state")}

    def children(self):
        return children(self.process.pid)

    def resources(self):
        base = Path("/proc", str(self.process.pid))
        return {"fds": len(list((base / "fd").iterdir())),
                "threads": len(list((base / "task").iterdir()))}

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        self.process.stdout.close()
        self.log.close()


def failure(result, code):
    assert result["status"] == 422, result
    assert result["reply"]["errors"][0]["code"] == code, result


def inside(args):
    subprocess.run([args.mount, "--make-rprivate", "/"], check=True)
    subprocess.run([args.ip, "link", "set", "lo", "up"], check=True)
    with tempfile.TemporaryDirectory(prefix="firefly-probe-dns-") as temporary:
        work = Path(temporary)
        resolver = work / "resolv.conf"
        resolver.write_text("nameserver 127.0.0.1\noptions timeout:2 attempts:1\n")
        subprocess.run([args.mount, "--bind", str(resolver), "/etc/resolv.conf"], check=True)
        settings = work / "mosquitto.conf"
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        settings.write_text(f"listener {port} 127.0.0.1\nuser root\n"
                            "allow_anonymous true\npersistence false\nlog_type all\n")
        log = (work / "broker.log").open("w")
        broker = subprocess.Popen([args.mosquitto, "-c", str(settings)],
                                  stdout=log, stderr=log)
        dns, server = None, None
        try:
            deadline = time.monotonic() + 4
            while True:
                assert broker.poll() is None, (work / "broker.log").read_text()
                try:
                    with socket.create_connection(("127.0.0.1", port), 0.1):
                        break
                except OSError:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
            dns = Dns()
            directory = work / "http"
            directory.mkdir()
            server = Server(Path(args.server), Path(args.root), directory)
            # Synthetic credentials must reach the worker only through IPC.
            secret = "isolated-dns-synthetic-password"
            assert server.request("POST", "/api/mqtt/credentials", {
                "id": "dns", "username": "test_dns", "password": secret})[0] == 200
            before = server.snapshot()
            time.sleep(0.1)
            baseline = server.resources()
            results = []
            for host in ("probe-delay.review.invalid", "probe-never.review.invalid"):
                for _ in range(5):
                    result = server.probe(port, host, credentials="dns")
                    failure(result, "mqtt_probe_timeout")
                    assert 95 <= result["elapsed_ms"] < 450, result
                    assert not server.children(), "worker not reaped before response"
                    results.append({"host": host, **result})
                following = server.probe(port, timeout=1000)
                assert following["status"] == 200 and following["elapsed_ms"] < 450, following
            dns.arrived.clear()
            with concurrent.futures.ThreadPoolExecutor(1) as pool:
                pending = pool.submit(server.probe, port, "probe-never.review.invalid", 500, "dns")
                assert dns.arrived.wait(2)
                child = next(iter(server.children()))
                proc = Path("/proc", str(child))
                assert secret.encode() not in (proc / "cmdline").read_bytes()
                assert secret.encode() not in (proc / "environ").read_bytes()
                assert len([part for part in (proc / "cmdline").read_bytes().split(b"\0") if part]) == 1
                assert not (proc / "fd/71").exists(), "foreign non-CLOEXEC FD leaked"
                busy = server.probe(port, timeout=1000)
                assert busy["status"] == 409 and busy["reply"]["errors"][0]["code"] == "mqtt_probe_busy"
                failure(pending.result(), "mqtt_probe_timeout")
                assert not proc.exists()
            time.sleep(0.1)
            assert server.resources() == baseline
            assert server.snapshot() == before
            events = [json.loads(line) for line in server.audit.read_text().splitlines()]
            spawned = [event for event in events if event["event"] == "spawn" and not event["result"]]
            reaped = [event for event in events if event["event"] == "reap"]
            killed = [event for event in events if event["event"] == "kill"]
            assert len(spawned) == len(reaped) and len(killed) == 11
            assert all(event["signal"] == signal.SIGKILL and not event["result"] for event in killed)
            assert sum(event["signal"] == signal.SIGKILL for event in reaped) == 11
            trace = (work / "broker.log").read_text()
            assert "Received PUBLISH" not in trace and "Received SUBSCRIBE" not in trace
            assert any(query["host"] == "probe-delay.review.invalid" for query in dns.queries)
            # Real helper failures must stay distinct from a budget timeout.
            # Modify only private copies, never the original build artifacts.
            stage = work / "bad-helper"
            stage.mkdir()
            copied_server = stage / "server"
            shutil.copy2(args.server, copied_server)
            helper = stage / "firefly-mqtt-probe-worker"
            for variant in ("missing", "invalid-executable", "writable"):
                if variant == "invalid-executable":
                    helper.write_bytes(b"not an executable\n")
                    helper.chmod(0o755)
                elif variant == "writable":
                    shutil.copy2(Path(args.server).parent / helper.name, helper)
                    helper.chmod(0o775)
                private = work / variant
                private.mkdir()
                invalid = Server(copied_server, Path(args.root), private)
                try:
                    snapshot = invalid.snapshot()
                    failure(invalid.probe(port, timeout=1000), "mqtt_probe_worker_unavailable")
                    assert invalid.snapshot() == snapshot
                    assert not invalid.children()
                finally:
                    invalid.close()
            print(json.dumps({"result": "PASS", "timeout_ms": 100, "dns_delay_ms": 1250,
                              "request_ms": [round(result["elapsed_ms"], 3) for result in results],
                              "killed_reaped": len(killed), "resources": baseline,
                              "genuine_helper_errors": 3,
                              "PUBLISH": 0, "SUBSCRIBE": 0, "store_active_state_unchanged": True}))
        finally:
            if server:
                server.close()
            if dns:
                dns.close()
            broker.terminate()
            try:
                broker.wait(timeout=3)
            except subprocess.TimeoutExpired:
                broker.kill()
                broker.wait(timeout=3)
            log.close()


def main():
    parser = argparse.ArgumentParser()
    for option in ("server", "root", "mosquitto", "unshare", "mount", "ip"):
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--inside", action="store_true")
    args = parser.parse_args()
    if args.inside:
        inside(args)
    else:
        subprocess.run([args.unshare, "-Ur", "-m", "-n", "--", sys.executable,
                        str(Path(__file__).resolve()), *sys.argv[1:], "--inside"], check=True, timeout=25)


if __name__ == "__main__":
    main()
