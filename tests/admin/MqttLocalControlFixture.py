#!/usr/bin/env python3
"""Rootless local MQTT broker controller E2E. Never uses real systemctl.

A test-only C++ helper accepts one fixed systemd socket FD and invokes
a synthetic, fixed-name systemctl program inside a temporary directory.
No privileged commands, real units or MQTT brokers are modified.
"""
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time


def runner():
    if len(sys.argv) != 4:
        raise RuntimeError("usage: MqttLocalControlFixture.py TEST_HELPER TEST_CLIENT REPO_ROOT")
    helper, client, repo = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="firefly-mqtt-local-control-") as d:
        work = pathlib.Path(d)
        fake = work / "synthetic-systemctl"
        fake.write_text(
            "#!/usr/bin/env python3\n"
            "import os,pathlib,stat,sys\n"
            "p=pathlib.Path(__file__).parent\n"
            "# FD 3 must never leak from the privileged broker controller.\n"
            "try:\n"
            "    if stat.S_ISSOCK(os.fstat(3).st_mode):\n"
            "        (p/'leaked-listening-fd').write_text('leaked')\n"
            "except OSError: pass\n"
            "args=sys.argv[1:]\n"
            "with (p/'calls.txt').open('a') as log: log.write(' '.join(args)+'\\n')\n"
            "if len(args)<3 or args[0]!='--system' or args[-1]!='firefly-mqtt-broker.service': sys.exit(99)\n"
            "state=p/'state.txt'\n"
            "now=state.read_text().strip() if state.exists() else 'disabled stopped'\n"
            "if args[1]=='is-enabled' and len(args)==3:\n"
            "    print(now.split()[0]); sys.exit(0 if now.split()[0]=='enabled' else 1)\n"
            "if args[1]=='is-active' and len(args)==3:\n"
            "    print('active' if now.split()[1]=='running' else 'inactive')\n"
            "    sys.exit(0 if now.split()[1]=='running' else 3)\n"
            "if args[1:3]==['enable','--now'] and len(args)==4:\n"
            "    if (p/'fail-enable').exists():\n"
            "        state.write_text('enabled stopped\\n'); sys.exit(0)\n"
            "    if (p/'fail-command').exists():\n"
            "        state.write_text('enabled stopped\\n'); sys.exit(1)\n"
            "    state.write_text('enabled running\\n'); sys.exit(0)\n"
            "if args[1:3]==['disable','--now'] and len(args)==4:\n"
            "    state.write_text('disabled stopped\\n'); sys.exit(0)\n"
            "sys.exit(99)\n", encoding="utf-8")
        fake.chmod(0o700)
        binary = work / "mosquitto-test-bin"
        binary.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
        binary.chmod(0o700)
        (work / "state.txt").write_text("disabled stopped\n", encoding="ascii")
        socket_path = str(work / "helper.sock")

        # Force FD4+: reserve the normally available FD3 first. The test
        # must prove socket activation even when the parent's socket is NOT 3.
        reserved = os.open(os.devnull, os.O_RDONLY)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            if listener.fileno() <= 3:
                raise RuntimeError("test did not allocate listener on FD4+")
            listener.bind(socket_path)
            listener.listen(16)
            # File actions execute atomically in the spawned process and keep
            # the new FD3 alive across exec; pass_fds+preexec_fn could close it.
            actions = [(os.POSIX_SPAWN_DUP2, listener.fileno(), 3),
                       (os.POSIX_SPAWN_CLOSE, listener.fileno()),
                       (os.POSIX_SPAWN_OPEN, 1, os.devnull, os.O_WRONLY, 0o600),
                       (os.POSIX_SPAWN_OPEN, 2, os.devnull, os.O_WRONLY, 0o600)]
            env = dict(os.environ, FIREFLY_TEST_SYSTEMCTL=str(fake),
                       FIREFLY_TEST_MOSQUITTO_BINARY=str(binary))
            pid = os.posix_spawn(helper, [helper], env, file_actions=actions)
        finally:
            os.close(reserved)
        reaped = False
        try:
            time.sleep(0.2)
            waited, code = os.waitpid(pid, os.WNOHANG)
            if waited:
                reaped = True
                raise RuntimeError(
                    f"isolated helper exited before FD4 test: {os.waitstatus_to_exitcode(code)}")
            # Direct low-level unauthorized commands never reach systemctl.
            direct_tests = ["RESTART\n", "ENABLE serviceX\n",
                            "ENABLE\nDISABLE\n", "ENABLE\x00\n", "STATUS extra\n"]
            for command in direct_tests:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(3.0)
                    connection.connect(socket_path)
                    connection.sendall(command.encode("utf-8"))
                    connection.shutdown(socket.SHUT_WR)
                    reply = connection.recv(96).decode("ascii", errors="replace")
                if reply != "ERROR broker_invalid_state\n":
                    raise RuntimeError(f"unknown operation accepted: {command!r}")
            if (work / "calls.txt").exists():
                raise RuntimeError("invalid socket messages reached systemctl")
            result = subprocess.run([client, repo, socket_path], cwd=work,
                                    capture_output=True, text=True, timeout=18)
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode != 0:
                raise RuntimeError(f"isolated Admin broker control failed: {result.returncode}")
            operations = (work / "calls.txt").read_text(encoding="utf-8").splitlines()
            allowed = {
                "--system is-enabled firefly-mqtt-broker.service",
                "--system is-active firefly-mqtt-broker.service",
                "--system enable --now firefly-mqtt-broker.service",
                "--system disable --now firefly-mqtt-broker.service",
            }
            if set(operations) - allowed:
                raise RuntimeError("disallowed commands reached systemctl")
            if (work / "leaked-listening-fd").exists():
                raise RuntimeError("privileged AF_UNIX listener FD3 leaked into execve child")
            if operations.count("--system enable --now firefly-mqtt-broker.service") != 3 or \
               operations.count("--system disable --now firefly-mqtt-broker.service") != 6:
                raise RuntimeError("broker enabled/disabled or compensation incorrect number of times")
            if (work / "state.txt").read_text().strip() != "disabled stopped":
                raise RuntimeError("final simulated service not stopped and disabled")
            print("PASS rootless fixed-operation MQTT broker helper / API integration")
        finally:
            if not reaped:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    waited, _ = os.waitpid(pid, os.WNOHANG)
                    if waited:
                        reaped = True
                        break
                    time.sleep(0.05)
                if not reaped:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    os.waitpid(pid, 0)
            listener.close()


if __name__ == "__main__":
    try:
        runner()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"MqttLocalBrokerControl fixture FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
