#!/usr/bin/env python3
"""Prepare/verify an M19-readable whole-store checkpoint; explicit offline restore.

Does not stop/start services or switch releases. Keep service stopped after any
restore error. Production restoration requires separate owner authorization.
"""
import argparse
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging import firefly_checkpoint as c


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    for mode in ('create', 'verify', 'restore'):
        p = sub.add_parser(mode)
        p.add_argument('--checkpoint', type=Path, required=True)
        p.add_argument('--m19-config', type=Path, required=True)
        if mode != 'create':
            p.add_argument('--checkpoint-pin', required=True)
        if mode != 'verify':
            p.add_argument('--store', type=Path, required=True)
        if mode == 'restore':
            p.add_argument('--transaction', type=Path, required=True)
    a = parser.parse_args()
    try:
        if a.mode == 'create':
            result = dict(result='CHECKPOINT_VERIFIED', checkpoint_pin=c.create(a.store, a.checkpoint, a.m19_config))
        elif a.mode == 'verify':
            c.verify(a.checkpoint, a.m19_config, a.checkpoint_pin)
            result = dict(result='CHECKPOINT_VERIFIED', checkpoint_pin=a.checkpoint_pin)
        else:
            result = c.restore(a.store, a.checkpoint, a.m19_config, a.checkpoint_pin, a.transaction)
        print(json.dumps(result))
        return 0
    except Exception as error:
        print(json.dumps(dict(result='BLOCKED_KEEP_SERVICE_STOPPED', error=str(error))), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
