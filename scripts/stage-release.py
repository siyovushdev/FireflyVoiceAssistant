#!/usr/bin/env python3
"""Stage, verify, activate or roll back a relocatable Firefly release.

Staging writes only to the caller-specified destination. It does not install
systemd units, restart services, or modify /var/lib/firefly/store.
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging.firefly_release import main


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
