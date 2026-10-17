#!/usr/bin/env python3
"""File-only local Mosquitto staging. Never installs/enables systemd units.

Requires an explicit RFC1918 LAN IPv4 so the broker config never defaults
to 0.0.0.0 or another WAN listener. Does not create broker accounts/passwords.
"""
import argparse
import hashlib
import ipaddress
import json
import os
import pathlib
import shutil
import stat
import sys


def private_lan_address(value: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise ValueError("explicit IPv4 LAN address required") from exc
    networks = [ipaddress.IPv4Network(net) for net in
                ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")]
    if not any(address in n for n in networks):
        raise ValueError("only explicit RFC1918 LAN IPv4 is allowed")
    return str(address)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, help="NEW empty staging directory")
    parser.add_argument("--lan-ip", required=True, help="pre-approved Firefly LAN IPv4")
    arguments = parser.parse_args()
    address = private_lan_address(arguments.lan_ip)
    target = pathlib.Path(arguments.output)
    if target.is_symlink() or target.exists():
        raise ValueError("staging directory must not exist (no overwrite)")
    if not target.is_absolute():
        raise ValueError("absolute staging directory is required")
    target.mkdir(mode=0o700, parents=False)
    root = pathlib.Path(__file__).resolve().parent
    sources = {
        "etc/systemd/system/firefly-mqtt-broker.service": "firefly-mqtt-broker.service",
        "etc/systemd/system/firefly-mqtt-control.socket": "firefly-mqtt-control.socket",
        "etc/systemd/system/firefly-mqtt-control.service": "firefly-mqtt-control.service",
    }
    manifest = {}
    for relative, filename in sources.items():
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        content = (root / filename).read_bytes()
        destination.write_bytes(content)
        destination.chmod(0o644)
        manifest[relative] = hashlib.sha256(content).hexdigest()
    rendered = (root / "mosquitto.conf.template").read_text(
        encoding="utf-8").replace("@LAN_IPV4@", address)
    if "@LAN_IPV4@" in rendered:
        raise RuntimeError("LAN listener substitution incomplete")
    config = target / "etc/firefly/mqtt/mosquitto.conf"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text(rendered, encoding="utf-8")
    config.chmod(0o640)
    manifest[str(config.relative_to(target))] = hashlib.sha256(
        config.read_bytes()).hexdigest()
    data = {
        "kind": "firefly-local-mqtt-staging-v1",
        "lan_ipv4": address,
        "files_sha256": manifest,
        "service_default": "disabled_and_stopped",
        "broker_installation": "NOT_PERFORMED",
        "controller_socket_installation": "NOT_PERFORMED",
        "password_file": "NOT_GENERATED",
        "helper_binary": "NOT_INCLUDED_BUILD_SEPARATELY",
        "production_modified": False,
        "approval_required": True,
    }
    (target / "manifest.json").write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    print(f"STAGED_ONLY {target}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError) as error:
        print(f"STAGING_REJECTED: {error}", file=sys.stderr)
        raise SystemExit(2)
