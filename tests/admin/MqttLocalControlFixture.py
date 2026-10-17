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
            "import pathlib,sys\n"
            "p=pathlib.Path(__file__).parent\n"
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
            "    state.write_text('enabled running\\n'); sys.exit(0)\n"
            "if args[1:3]==['disable','--now'] and len(args)==4:\n"
            "    state.write_text('disabled stopped\\n'); sys.exit(0)\n"
            "sys.exit(99)\n", encoding="utf-8")
        fake.chmod(0o700)
        (work / "state.txt").write_text("disabled stopped\n", encoding="ascii")
        socket_path = str(work / "helper.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(socket_path)
        listener.listen(16)
        listener.set_inheritable(True)
        # The helper uses systemd's fd 3, and test mode permits only a
        # synthetic executable (never /usr/bin/systemctl).
        def child_setup():
            os.dup2(listener.fileno(), 3)
            os.set_inheritable(3, True)
        env = dict(os.environ, FIREFLY_TEST_SYSTEMCTL=str(fake))
        proc = subprocess.Popen([helper], env=env, pass_fds=(listener.fileno(),),
                                preexec_fn=child_setup, cwd=work,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            time.sleep(0.2)
            if proc.poll() is not None:
                raise RuntimeError(f"isolated helper unexpectedly exited: {proc.returncode}")
            # Direct low-level unauthorized commands never reach systemctl.
            direct_tests = ["RESTART\n", "ENABLE serviceX\n",
                            "ENABLE\nDISABLE\n", "ENABLE\x00\n", "STATUS extra\n"]
            for command in direct_tests:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(3.0)
                    connection.connect(socket_path)
                    connection.sendall(command.encode("utf-8"))
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
            if operations.count("--system enable --now firefly-mqtt-broker.service") != 1 or \
               operations.count("--system disable --now firefly-mqtt-broker.service") != 1:
                raise RuntimeError("broker enabled/disabled incorrect number of times")
            if (work / "state.txt").read_text().strip() != "disabled stopped":
                raise RuntimeError("final simulated service not stopped and disabled")
            print("PASS rootless fixed-operation MQTT broker helper / API integration")
        finally:
            proc.send_signal(signal.SIGTERM) if proc.poll() is None else None
            try:
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate(timeout=3)
            listener.close()


if __name__ == "__main__":
    try:
        runner()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"MqttLocalBrokerControl fixture FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
