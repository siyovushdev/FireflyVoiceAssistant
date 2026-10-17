#!/usr/bin/env python3
"""Cold-start MQTT subscription recovery with a disposable loopback-only broker.

The native Firefly test must prove that it has attempted to subscribe and
marked the target Unreachable before this runner starts Mosquitto. Nothing
in this fixture uses systemd, sudo, production state or existing broker ports.
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
        raise RuntimeError("usage: MqttInitialRecoveryFixture.py TEST REPO_ROOT MOSQUITTO")
    test, root, mosquitto = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="firefly-mqtt-initial-offline-") as tmp:
        work = pathlib.Path(tmp)
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
        broker = None
        native = None
        log = work / "broker.log"
        with log.open("wb") as output:
            try:
                native = subprocess.Popen(
                    [test, root, str(port), str(work), "--initial-offline"],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    cwd=work,
                )
                deadline = time.monotonic() + 9
                while time.monotonic() < deadline and native.poll() is None:
                    if (work / "startup-request").exists():
                        break
                    time.sleep(0.05)
                else:
                    if native.poll() is None:
                        raise RuntimeError("native test never reached initial connection failure")
                    stdout, stderr = native.communicate(timeout=3)
                    sys.stdout.write(stdout)
                    sys.stderr.write(stderr)
                    raise RuntimeError(f"native test failed before broker startup: {native.returncode}")
                # The first connection attempt has failed and been explicitly
                # observed before launching the test broker.
                broker = subprocess.Popen(
                    [mosquitto, "-c", str(settings)],
                    stdin=subprocess.DEVNULL,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    cwd=work,
                )
                deadline = time.monotonic() + 7
                while time.monotonic() < deadline:
                    if broker.poll() is not None:
                        raise RuntimeError("isolated broker failed to start")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("isolated broker never listened")
                (work / "broker-ready").write_text("ready\n", encoding="ascii")
                stdout, stderr = native.communicate(timeout=28)
                sys.stdout.write(stdout)
                sys.stderr.write(stderr)
                if native.returncode != 0:
                    raise RuntimeError(f"initial offline recovery failed: {native.returncode}")
                if "PASS initial broker absent" not in stdout:
                    raise RuntimeError("native test omitted the recovery assertions")
                print("PASS isolated initial MQTT broker failure recovery")
            finally:
                if native is not None and native.poll() is None:
                    native.kill()
                    native.communicate(timeout=3)
                if broker is not None and broker.poll() is None:
                    broker.terminate()
                    try:
                        broker.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        broker.kill()
                        broker.wait(timeout=3)


if __name__ == "__main__":
    try:
        run()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"INITIAL MQTT RECOVERY FIXTURE FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
