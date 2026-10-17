#!/usr/bin/env python3
"""Pure file-only Mosquitto configuration staging validation."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def run():
    if len(sys.argv) != 2:
        raise RuntimeError("expected repository root")
    script = pathlib.Path(sys.argv[1]) / "packaging/mqtt/stage-local-mqtt.py"
    with tempfile.TemporaryDirectory(prefix="firefly-mqtt-stage-") as root:
        work = pathlib.Path(root)
        good = work / "candidate"
        result = subprocess.run(
            [sys.executable, str(script), "--output", str(good),
             "--lan-ip", "192.168.68.15"], capture_output=True, text=True, timeout=5)
        if result.returncode != 0:
            raise RuntimeError(f"valid staging failed: {result.stderr}")
        config = (good / "etc/firefly/mqtt/mosquitto.conf").read_text()
        if ("allow_anonymous false" not in config or
                "password_file /etc/firefly/mqtt/passwords" not in config or
                "listener 1883 127.0.0.1" not in config or
                "listener 1883 192.168.68.15" not in config):
            raise RuntimeError("broker configuration is not authenticated/private")
        if "0.0.0.0" in config or "@LAN_IPV4@" in config:
            raise RuntimeError("public/wildcard or unprovisioned listener")
        manifest = json.loads((good / "manifest.json").read_text())
        if manifest["service_default"] != "disabled_and_stopped" or \
                manifest["broker_installation"] != "NOT_PERFORMED":
            raise RuntimeError("staging claimed installation or activation")
        for path, digest in manifest["files_sha256"].items():
            if hashlib.sha256((good / path).read_bytes()).hexdigest() != digest:
                raise RuntimeError("staging SHA256 mismatch")
        if (good / "etc/firefly/mqtt/passwords").exists():
            raise RuntimeError("staging wrote secrets")
        for index, unsafe in enumerate(("127.0.0.1", "0.0.0.0", "8.8.8.8",
                       "169.254.1.2", "224.0.0.1", "100.94.87.92",
                       "2001:db8::1", "192.168.68.15\nlistener 1884 0.0.0.0"):
            out = work / ("bad_" + str(abs(hash(unsafe))))
            attempt = subprocess.run(
                [sys.executable, str(script), "--output", str(out),
                 "--lan-ip", unsafe], capture_output=True, text=True, timeout=5)
            if attempt.returncode == 0 or out.exists():
                raise RuntimeError(f"unsafe LAN IP accepted: {unsafe!r}")
        repeat = subprocess.run(
            [sys.executable, str(script), "--output", str(good),
             "--lan-ip", "10.2.3.4"], capture_output=True, text=True, timeout=5)
        if repeat.returncode == 0 or "listener 1883 192.168.68.15" not in \
                (good / "etc/firefly/mqtt/mosquitto.conf").read_text():
            raise RuntimeError("staging overwrote existing output")
        link = work / "symlink"
        link.symlink_to(good, target_is_directory=True)
        blocked = subprocess.run(
            [sys.executable, str(script), "--output", str(link),
             "--lan-ip", "10.1.2.3"], capture_output=True, text=True, timeout=5)
        if blocked.returncode == 0:
            raise RuntimeError("staging followed symlink")
        print("PASS file-only Mosquitto staged configuration, checksum and negative LAN cases")


if __name__ == "__main__":
    try:
        run()
    except (RuntimeError, OSError, subprocess.TimeoutExpired, KeyError) as error:
        print(f"FAIL local broker staging: {error}", file=sys.stderr)
        sys.exit(1)
