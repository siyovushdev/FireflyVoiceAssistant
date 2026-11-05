#!/usr/bin/env python3
"""Prepare/verify an M19-readable whole-store checkpoint; explicit offline restore.

Does not stop/start services or switch releases. An explicit sealed operational
transition can restore its exact unit bundle and reload PID 1 while stopped. Keep service stopped after any
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
        p.add_argument('--m19-config', '--config', dest='m19_config', type=Path, required=True)
        p.add_argument('--authority-release', type=Path)
        p.add_argument('--transition-point', type=Path)
        p.add_argument('--transition-pin')
        if mode != 'create':
            p.add_argument('--checkpoint-pin', required=True)
        if mode != 'verify':
            p.add_argument('--store', type=Path, required=True)
        if mode == 'restore':
            p.add_argument('--transaction', type=Path, required=True)
    a = parser.parse_args()
    try:
        authority = None
        transition = None
        if a.authority_release is not None:
            from packaging import firefly_release as r
            prefix = Path(r.PRODUCTION_PREFIX)
            release = a.authority_release
            if (os.geteuid()!=0 or os.environ.get('FIREFLY_OWNER_ACCEPTED')!='1' or
                    release.parent!=prefix/'releases' or release.resolve()!=release or
                    a.m19_config!=release/'bin/firefly-config'):
                raise c.CheckpointError('Owner-authorized exact immutable release required')
            r.verify_tree(prefix,release.name)
            authority = c.digest(a.m19_config)
        if a.transition_point is not None or a.transition_pin is not None:
            if (a.mode!='restore' or authority is None or not a.transition_point or not a.transition_pin):
                raise c.CheckpointError('Complete authorized restore transition required')
            transition = c.control(a.transition_point)
            if c.pin(transition)!=a.transition_pin:
                raise c.CheckpointError('Transition capsule changed')
            old = transition['old']
            if (Path(old['checkpoint'])!=a.checkpoint or old['checkpoint_pin']!=a.checkpoint_pin or
                    Path(old['binary'])!=a.m19_config or old['binary_sha256']!=authority):
                raise c.CheckpointError('Transition target mismatch')
            receipt=c.verify(a.checkpoint,a.m19_config,a.checkpoint_pin,authority_sha256=authority)
            c.validate_release_capsule(transition['rollback_capsule'],receipt,a.checkpoint_pin,a.authority_release)
        kwargs = {} if authority is None else dict(authority_sha256=authority)
        if a.mode == 'create':
            result = dict(result='CHECKPOINT_VERIFIED', checkpoint_pin=c.create(a.store, a.checkpoint, a.m19_config, **kwargs))
        elif a.mode == 'verify':
            c.verify(a.checkpoint, a.m19_config, a.checkpoint_pin, **kwargs)
            result = dict(result='CHECKPOINT_VERIFIED', checkpoint_pin=a.checkpoint_pin)
        else:
            result = c.restore(a.store, a.checkpoint, a.m19_config, a.checkpoint_pin, a.transaction, **kwargs)
        if transition is not None:
            import subprocess
            from packaging.operational_transition import apply_bundle, bound_sources
            def reload(argv, **limits):
                subprocess.run(argv,check=True,timeout=limits.get('timeout',15))
            if bound_sources(transition['old']['unit_bundle'])!=transition['old'].get('bound_sources',{}):
                raise c.CheckpointError('Bound wake sources changed')
            apply_bundle(transition['old']['unit_bundle'],transition.get('baseline_unit_bundle',transition['old']['unit_bundle']),
                         transition['new']['unit_bundle'],reload)
            result['unit_restored']=True
        print(json.dumps(result))
        return 0
    except Exception as error:
        print(json.dumps(dict(result='BLOCKED_KEEP_SERVICE_STOPPED', error=str(error))), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
