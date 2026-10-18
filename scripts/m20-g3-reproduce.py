#!/usr/bin/env python3
"""Reproduce M20 G3 offline in a NEW output directory, from any cwd.

The verified evidence bundle supplies acceptance harness sources and dependency
hashes. A real detached local clone is created; product config paths never pass
through a worktree symlink. No download, installation, services or hardware.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    if sys.flags.optimize or os.environ.get('PYTHONOPTIMIZE'):
        parser.error('Acceptance requires Python assertions enabled')
    repo, stage, output, bundle = (p.resolve() for p in (args.repo, args.stage, args.output, args.bundle))
    expected = (bundle / 'FINAL-HEAD.txt').read_text().strip()
    head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    if head != expected or subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], text=True):
        parser.error('An exact clean source checkout is required')
    if output.exists():
        parser.error('Output must be NEW; existing evidence is never overwritten')
    environment = json.loads((bundle / 'environment.json').read_text())
    if environment['source_head'] != expected:
        parser.error('Evidence source identity mismatch')
    for relative, digest in environment['dependency_hashes'].items():
        if sha256(stage / relative) != digest:
            parser.error('Provisioned dependency hash mismatch: ' + relative)
    frontend = repo / 'src/admin/frontend'
    if sha256(frontend / 'package-lock.json') != environment['frontend_lock_sha256']:
        parser.error('Frontend lockfile mismatch')
    if not (frontend / 'node_modules').is_dir():
        parser.error('Pre-provision matching frontend dependencies; no installs are performed')
    inputs = json.loads((bundle / 'harness-input-manifest.json').read_text())['files']
    for relative, digest in inputs.items():
        name = Path(relative)
        if name.is_absolute() or '..' in name.parts or sha256(bundle / name) != digest:
            parser.error('Acceptance source hash mismatch: ' + relative)
    if 'acceptance.py' not in inputs:
        parser.error('Acceptance entry point is missing from the input manifest')
    evidence = output / 'evidence/m20-g3-20261018'
    evidence.mkdir(parents=True)
    checkout = output / 'm20-g3-worktree'
    # --shared is a local object-store reference, not a symlink in config paths.
    # All source/config directories are ordinary directories in a detached clone.
    subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout', str(repo), str(checkout)], check=True)
    subprocess.run(['git', '-C', str(checkout), 'checkout', '--quiet', '--detach', expected], check=True)
    shutil.copytree(frontend / 'node_modules', checkout / 'src/admin/frontend/node_modules', symlinks=True)
    location = output / 'evidence/m20-review-c86943e/staging'
    location.mkdir(parents=True)
    (location / 'root').symlink_to(stage, target_is_directory=True)
    for relative in inputs:
        destination = evidence / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(bundle / relative, destination)
    shutil.copy2(bundle / 'environment.json', evidence / 'environment.json')
    shutil.copy2(bundle / 'harness-input-manifest.json', evidence / 'harness-input-manifest.json')
    command = [sys.executable, str(evidence / 'acceptance.py')]
    # Every aggregate/nested failure is a nonzero exit. Never summarize a
    # partially completed run as PASS; preserve its output for investigation.
    subprocess.run(command, cwd=output, check=True)
    if subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip() != expected:
        raise RuntimeError('Source identity changed during acceptance')
    if subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain'], text=True):
        raise RuntimeError('Acceptance changed tracked source')
    print('G3_AGGREGATE_PASS', expected, flush=True)


if __name__ == '__main__':
    main()
