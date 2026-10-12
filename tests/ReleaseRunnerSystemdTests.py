#!/usr/bin/python3
"""Full coordinator path with real systemd, private files and ordinary CPUs only."""
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('runner', SOURCE / 'scripts/release-runner.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

RUNTIME = '''import json,os,sys,time
from pathlib import Path
root=Path(sys.argv[1]);assert root.parent==Path('/run') and root.name.startswith('firefly-release-runner-fixture-')
resource=(root/'resource').open('r')
partial=root/'runtime-ready.tmp';partial.write_text(json.dumps(dict(pid=os.getpid(),release=(root/'current-release').read_text().strip())))
partial.replace(root/'runtime-ready.json')
while True: time.sleep(1)
'''

CONTROLLER = '''import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests'))
import ReleaseRunnerFixture as fixture
globals().update({name:value for name,value in vars(fixture.reviewed).items() if not name.startswith('__')})
if __name__=='__main__': fixture.reviewed.main()
'''


def wait_for(predicate, deadline):
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise RuntimeError('Fixture deadline')


def case(name, evidence):
    root = Path(tempfile.mkdtemp(prefix='firefly-release-runner-fixture-' + name + '-', dir='/run'))
    root.rmdir()  # Only the newly allocated empty directory; prepare creates it exclusively.
    runner.prepare_files(root, SOURCE)
    m = runner.load(root / 'scripts/production-rollout.py')
    shutil.copyfile(root / 'scripts/production-rollout.py', root / 'scripts/reviewed-controller.py')
    shutil.copyfile(SOURCE / 'tests/ReleaseRunnerFixture.py', root / 'tests/ReleaseRunnerFixture.py')
    (root / 'scripts/production-rollout.py').write_text(CONTROLLER)
    (root / 'runtime.py').write_text(RUNTIME)
    (root / 'store').mkdir(mode=0o700)
    (root / 'store/configuration.json').write_text('{"fixture":"immutable"}\n')
    (root / 'store/configuration.json').chmod(0o600)
    shutil.copytree(root / 'store', root / 'backup-store', copy_function=shutil.copy2)
    shutil.copystat(root / 'store', root / 'backup-store')
    (root / 'resource').write_text('file-only resource\n')
    (root / 'scenario').write_text(name)
    (root / 'current-release').write_text('old\n')
    before = {'store_manifest': m.manifest(root / 'store')}
    runner.write(root / 'production-before.json', before)
    now = time.monotonic()
    plan = dict(old='old', new='new', backup_store=str(root / 'backup-store'),
                boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                verification_budget_seconds=240, recovery_budget_seconds=360,
                armed_monotonic=now, deadline_monotonic=now + 240,
                worker_unit=root.name + '-forward.service', rollback_unit=root.name + '-rollback.service',
                rollback_timer=root.name + '-rollback.timer',
                controller_sha256=m.sha(root / 'scripts/production-rollout.py'),
                recovery_helper_sha256=m.sha(root / 'packaging/firefly_recovery.py'),
                launcher_sha256=m.sha(root / 'scripts/deployment-launcher.py'),
                runner_sha256=m.sha(root / 'scripts/release-runner.py'))
    runner.write(root / 'plan.json', plan)
    runner.write(root / 'PREPARED', dict(plan_sha256=m.sha(root / 'plan.json'), scope='FILE_ONLY runtime boundary'))
    runtime = root.name + '-runtime.service'
    units = [runtime, plan['worker_unit'], plan['rollback_timer'], plan['rollback_unit']]
    process = None
    result = {'case': name, 'root': str(root), 'scope': 'FILE_ONLY; no production/API/NPU/microphone', 'pass': False}
    try:
        subprocess.run(['systemd-run', '--unit=' + runtime, '--property=Type=exec', '/usr/bin/python3',
                        str(root / 'runtime.py'), str(root)], check=True, capture_output=True, text=True, timeout=10)
        wait_for(lambda: (root / 'runtime-ready.json').exists(), time.monotonic() + 5)
        command = ['/usr/bin/python3', str(root / 'scripts/release-runner.py'), 'run', str(root),
                   '--owner-authorization', 'OFFLINE FILE-ONLY FIXTURE; not production authorization']
        started = time.monotonic()
        with (root / 'runner.stdout').open('w') as stdout, (root / 'runner.stderr').open('w') as stderr:
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
        wait_for(lambda: (root / 'PLAN_FROZEN').exists() or process.poll() is not None, time.monotonic() + 10)
        assert (root / 'PLAN_FROZEN').exists(), (root / 'runner.stderr').read_text()
        frozen = (root / 'plan.json').read_bytes()
        if name == 'runner_crash':
            wait_for(lambda: (root / 'worker-waiting').exists() or process.poll() is not None, time.monotonic() + 10)
            assert (root / 'worker-waiting').exists(), (root / 'runner.stderr').read_text()
            process.kill(); process.wait(timeout=5)
            restored = wait_for(lambda: (root / 'evidence/rollback-result.json').exists(), started + 275)
            outcome = json.loads((root / 'evidence/rollback-result.json').read_bytes())
            assert outcome['result'] == 'ROLLED_BACK' and outcome['ready']
        else:
            process.wait(timeout=275)
            assert (root / 'evidence/runner-result.json').exists(), (root / 'runner.stderr').read_text()
            outcome = json.loads((root / 'evidence/runner-result.json').read_bytes())
            assert outcome['result'] == ('DEPLOYED_VERIFIED' if name in ('success', 'diagnostic_error') else 'ROLLED_BACK'), outcome
            assert process.returncode == (0 if name in ('success', 'diagnostic_error') else 1)
        assert (root / 'plan.json').read_bytes() == frozen, 'Original timer plan changed'
        assert m.manifest(root / 'store') == before['store_manifest']
        assert m.manifest(root / 'backup-store') == before['store_manifest']
        switches = [json.loads(line) for line in (root / 'switches.jsonl').read_text().splitlines()]
        assert sum(x['target'] == 'new' for x in switches) == 1
        assert sum(x['target'] == 'old' for x in switches) == (name not in ('success', 'diagnostic_error'))
        final = json.loads((root / 'runtime-ready.json').read_bytes())
        assert final['release'] == ('new' if name in ('success', 'diagnostic_error') else 'old')
        if name in ('success', 'diagnostic_error'):
            assert time.monotonic() - started > 15
            assert (root / 'ACCEPTED').exists()
            assert (root / 'evidence/runner-independent-acceptance.json').exists()
        if name == 'diagnostic_error':
            assert 'error' in outcome['diagnostics'] and outcome['watchdog_disarmed_after_verification']
        if name == 'rollback_crash':
            assert (root / 'recovery-crashed').exists()
            journal = subprocess.check_output(['journalctl', '-u', plan['rollback_unit'], '--no-pager', '-o', 'json'], text=True)
            assert 'status=23/' in journal and 'Scheduled restart job' in journal
            result['restart_evidence'] = 'Durable recovered file state and original journal; no NRestarts snapshot assertion'
        # Repeat invocation cannot re-arm/reset/relaunch, even after completion.
        retry = subprocess.run(command, capture_output=True, text=True, timeout=5)
        assert retry.returncode != 0 and (root / 'plan.json').read_bytes() == frozen
        assert (root / 'switches.jsonl').read_text().count('\n') == len(switches)
        result.update(pass_case=True, **{'pass': True}, result=outcome['result'], duration_seconds=time.monotonic() - started,
                      final_runtime=final, switches=switches, original_deadline_preserved=True, repeat_launch_blocked=True)
    except Exception as error:
        result['error'] = repr(error)
    finally:
        if process and process.poll() is None:
            process.kill(); process.wait(timeout=5)
        journal = subprocess.run(['journalctl', *sum([['-u', unit] for unit in units], []), '--no-pager', '-o', 'json'], capture_output=True, text=True, timeout=10)
        (root / 'journal.jsonl').write_text(journal.stdout)
        subprocess.run(['systemctl', 'stop', *units], capture_output=True, text=True, timeout=20)
        runner.write(root / 'result.json', result)
        shutil.copytree(root, evidence / name)
    return result


if __name__ == '__main__':
    if os.geteuid() != 0:
        raise RuntimeError('Root required for isolated transient fixture units')
    os.umask(0o077)
    evidence = Path(sys.argv[1]); evidence.mkdir(mode=0o700)
    # The exact mandatory suite is exercised once before all coordinator cases.
    seed = Path(tempfile.mkdtemp(prefix='firefly-release-runner-fixture-preflight-', dir='/run'))
    seed.rmdir(); runner.prepare_files(seed, SOURCE)
    runner.mandatory_fixtures(seed)
    shutil.copytree(seed, evidence / 'mandatory-preflight')
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda name: case(name, evidence),
                               ['success', 'forward_failure', 'worker_crash', 'runner_crash', 'diagnostic_error', 'rollback_crash']))
    runner.write(evidence / 'summary.json', results)
    print(json.dumps([dict(case=x['case'], PASS=x['pass'], result=x.get('result'), error=x.get('error')) for x in results]))
    raise SystemExit(0 if all(x['pass'] for x in results) else 1)
