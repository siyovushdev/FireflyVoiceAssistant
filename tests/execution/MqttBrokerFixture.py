#!/usr/bin/env python3
"""Run Firefly G1 native smoke against a temporary loopback-only Mosquitto.

No production/system broker, service, package installation or secret files.
Required: native test executable, repository root, provisioned mosquitto binary.
"""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run():
    if len(sys.argv) != 4:
        raise RuntimeError("usage: MQTTBrokerFixture.py NATIVE_TEST REPO_ROOT MOSQUITTO_BINARY")
    test, root, mosquitto = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="firefly-g1-mqtt-") as directory:
        work = pathlib.Path(directory)
        port = free_port()
        settings = work / "mosquitto.conf"
        settings.write_text(
            f"listener {port} 127.0.0.1\n"
            "allow_anonymous true\n"
            "persistence false\n"
            "autosave_interval 0\n"
            "log_type warning\n",
            encoding="ascii",
        )
        log = work / "broker.log"
        with log.open("wb") as output:
            broker = subprocess.Popen(
                [mosquitto, "-c", str(settings)],
                cwd=work,
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline:
                    if broker.poll() is not None:
                        raise RuntimeError("isolated broker stopped during startup")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("isolated broker not listening")
                result = subprocess.run(
                    [test, root, str(port)],
                    cwd=work,
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=35,
                    check=False,
                )
                sys.stdout.write(result.stdout)
                sys.stderr.write(result.stderr)
                if result.returncode != 0:
                    raise RuntimeError(f"native G1 smoke returned {result.returncode}")
                print("PASS isolated G1 broker fixture")
            finally:
                if broker.poll() is None:
                    broker.terminate()
                    try:
                        broker.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        broker.kill()
                        broker.wait(timeout=3)
                if broker.returncode not in (0, -15) and log.exists():
                    sys.stderr.write(log.read_text(encoding="utf-8", errors="replace")[-4000:])


if __name__ == "__main__":
    try:
        run()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"MQTT BROKER FIXTURE FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
