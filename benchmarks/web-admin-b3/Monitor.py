"""Explicit exclusive live NPU gate. Never run together with CTest or another model owner."""
import argparse
import datetime
import json
from pathlib import Path
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--prefix', required=True)
scope = parser.add_mutually_exclusive_group()
scope.add_argument('--language-profiles', action='store_true', help='Isolated location profile smoke: Цех, Кухня, Склад')
scope.add_argument('--no-zone', action='store_true', help='One isolated no-zone semantic retest')
scope.add_argument('--remaining', help='Prior report with successful B fixture; run only non-home and contextual cases')
args = parser.parse_args()
prefix = Path(args.prefix)
prefix.parent.mkdir(parents=True, exist_ok=True)
if any(Path(str(prefix) + suffix).exists() for suffix in ('.json', '.log', '-memory.jsonl')):
    raise SystemExit('Choose a new output prefix; existing evidence is not overwritten')

def available():
    return int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:')))

for directory in Path('/proc').iterdir():
    if not directory.name.isdigit():
        continue
    try:
        if 'librkllmrt.so' in (directory / 'maps').read_text():
            raise SystemExit('Another RKLLM owner is running: ' + directory.name)
    except OSError:
        pass
if available() < 6 * 1024 * 1024:
    raise SystemExit('6 GiB available memory required for existing Admin admission')

with open(str(prefix) + '.log', 'w') as log, open(str(prefix) + '-memory.jsonl', 'w', buffering=1) as memory:
    command = ['build/src/admin/admin_b3_acceptance', str(prefix) + '.json']
    if args.language_profiles:
        command += ['--language-profiles']
    if args.no_zone:
        command += ['--no-zone']
    if args.remaining:
        command += ['--remaining', args.remaining]
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    start = time.monotonic()
    stopped = None
    while process.poll() is None:
        elapsed = time.monotonic() - start
        kib = available()
        memory.write(json.dumps({'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'pid': process.pid, 'elapsed_s': elapsed, 'available_kib': kib}) + '\n')
        if stopped is None and (kib < 1024 * 1024 or elapsed > 1200):
            process.send_signal(signal.SIGTERM)
            stopped = time.monotonic()
        if stopped and time.monotonic() - stopped > 120:
            raise SystemExit('Graceful shutdown has not completed; do not start another NPU owner. PID ' + str(process.pid))
        time.sleep(1)
    print('Live NPU exit:', process.returncode, 'elapsed_s:', round(time.monotonic() - start, 2))
    raise SystemExit(process.returncode)
