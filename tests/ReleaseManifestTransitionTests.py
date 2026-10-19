#!/usr/bin/env python3
"""Explicit immutable old/new archive fixtures; no models, services or store access.

Supply real extracted manifests AND their protected artifact trees. No missing
input skip and no generated replacement manifests for the positive control.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging import firefly_release as r
spec = importlib.util.spec_from_file_location('release_runner', Path(__file__).resolve().parents[1] / 'scripts/release-runner.py')
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['m19-release', 'm20-release', 'report']:
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    old, new = [json.loads((p / 'manifest.json').read_bytes()) for p in [a.m19_release, a.m20_release]]
    protected = lambda e: e['path'].startswith(('lib/', 'share/firefly/wake/', 'share/firefly/config/', 'share/firefly/systemd/'))
    counts = []
    for tree, doc in [(a.m19_release, old), (a.m20_release, new)]:
        entries = r.assert_artifact_manifest(doc['artifacts'], mqtt=doc.get('mqtt', {}).get('enabled') is True)
        for e in entries.values():
            if protected(e):
                r._expect(tree / e['path'], e['sha256'], e['bytes'])
        counts.append(sum(map(protected, doc['artifacts'])))
    runner.preservation_contract(old, new, a.m19_release, a.m20_release)
    negatives = []
    frozen = next(i for i, e in enumerate(new['artifacts']) if protected(e) and e['path'] in {x['path'] for x in old['artifacts']})
    for fault in ['removed', 'digest', 'size', 'property', 'extra-library', 'mode', 'mode-missing', 'shared-model', 'duplicate']:
        bad = copy.deepcopy(new)
        if fault == 'removed': bad['artifacts'].pop(frozen)
        elif fault == 'digest': bad['artifacts'][frozen]['sha256'] = '0' * 64
        elif fault == 'size': bad['artifacts'][frozen]['bytes'] += 1
        elif fault == 'property': bad['artifacts'][frozen]['unexpected'] = True
        elif fault == 'extra-library': bad['artifacts'].append(dict(path='lib/unapproved.so', sha256='0' * 64, bytes=1, mode=0o755))
        elif fault == 'mode': bad['artifacts'][frozen]['mode'] = 0o6755
        elif fault == 'mode-missing': bad['artifacts'][frozen].pop('mode')
        elif fault == 'shared-model': bad['shared_models'][0]['sha256'] = '0' * 64
        else: bad['artifacts'].append(copy.deepcopy(bad['artifacts'][frozen]))
        try: runner.preservation_contract(old, bad, a.m19_release, a.m20_release)
        except RuntimeError: negatives.append(dict(case=fault, result='REJECTED'))
        else: raise AssertionError('Accepted ' + fault)
    result = dict(result='PASS', old=old['build']['head'], new=new['build']['head'], protected_counts=counts, frozen_bytes_verified=True, actual_modes_verified=True, original_shared_references_equal=old['shared_models'] == new['shared_models'], negatives=negatives)
    a.report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
