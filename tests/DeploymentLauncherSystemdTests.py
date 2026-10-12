#!/usr/bin/env python3
"""Real root transient systemd fixtures. Only private files and fixture units."""
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
spec = importlib.util.spec_from_file_location('launcher', SOURCE / 'scripts/deployment-launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

WORKER = '''import json,os,sys,time
from pathlib import Path
root=Path(sys.argv[1]);delay=float(sys.argv[2]);status=int(sys.argv[3])
started=time.monotonic()
(root/'started.json').write_text(json.dumps(dict(pid=os.getpid(),ppid=os.getppid(),monotonic=started)))
time.sleep(delay) # Simulated work, deliberately longer than the old client timeout.
(root/'completed.json').write_text(json.dumps(dict(duration=time.monotonic()-started,status=status)))
sys.exit(status)
'''

ROLLBACK = '''import importlib.util,json,os,subprocess,sys,time
from pathlib import Path
root=Path(sys.argv[1]);unit=sys.argv[2]
assert root.parent==Path('/run') and root.name.startswith('firefly-launcher-fixture-')
assert unit==root.name+'-worker.service'
reply=subprocess.run(['systemctl','show',unit,'-p','LoadState','-p','ActiveState','-p','MainPID','-p','ControlPID'],capture_output=True,text=True,check=True,timeout=5)
state=dict(line.split('=',1) for line in reply.stdout.splitlines())
if state['LoadState']=='not-found':
 assert state['ActiveState']=='inactive' and state['MainPID']=='0' and state['ControlPID']=='0'
else:
 subprocess.run(['systemctl','stop',unit],check=True,timeout=10)
spec=importlib.util.spec_from_file_location('controller',root/'scripts/production-rollout.py')
controller=importlib.util.module_from_spec(spec);spec.loader.exec_module(controller)
ops=controller.Fixture()
result=controller.recover(ops,'old',time.monotonic()+5)
result.update(pid=os.getpid(),ppid=os.getppid(),monotonic=time.monotonic(),scope='file-only fixture')
(root/'restored.json').write_text(json.dumps(result))
'''


class FixtureOps:
    systemd_properties = launcher.controller.Production.systemd_properties

    def __init__(self, root):
        self.root = root
        self.operation_deadline = time.monotonic() + 45

    def record(self, name, value):
        with (self.root / 'events.jsonl').open('a') as file:
            file.write(json.dumps(dict(monotonic=time.monotonic(),event=name,value=value)) + '\n')

    def run(self, argv, timeout=15):
        assert not any(text in arg for arg in argv for text in
                       ['firefly-voice-assistant', '/dev/', '/opt/', '/var/lib/'])
        started = time.monotonic()
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=min(timeout, max(.01, self.operation_deadline-started)),
                                env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
        self.record('command',dict(argv=argv,returncode=result.returncode,stdout=result.stdout,
                                  stderr=result.stderr,duration=time.monotonic()-started))
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()


def wait_file(path, deadline):
    while time.monotonic() < deadline:
        if path.exists():
            return json.loads(path.read_text())
        time.sleep(.05)
    raise RuntimeError('Fixture file deadline: '+str(path))


def run_case(name, evidence):
    root = Path(tempfile.mkdtemp(prefix='firefly-launcher-fixture-'+name+'-',dir='/run'))
    root.chmod(0o700)
    worker_unit = root.name+'-worker.service'
    rollback_base = root.name+'-rollback'
    ops = FixtureOps(root)
    result = dict(case=name,root=str(root),worker_unit=worker_unit,rollback_timer=rollback_base+'.timer')
    for file in ['scripts/production-rollout.py','scripts/deployment-launcher.py',
                 'packaging/__init__.py','packaging/firefly_recovery.py']:
        target = root/file; target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(SOURCE/file,target)
    (root/'worker.py').write_text(WORKER)
    (root/'rollback.py').write_text(ROLLBACK)
    try:
        interval = 8 if name=='observation_deadline' else 23
        ops.run(['systemd-run','--unit='+rollback_base,'--on-active='+str(interval)+'s',
                 '--timer-property=AccuracySec=250ms','--property=Type=exec',
                 '--property=Restart=on-failure','--property=RestartSec=1s',
                 '--property=RestartPreventExitStatus=78','--property=StartLimitIntervalSec=0',
                 '--property=TimeoutStartSec=10s','--property=TimeoutStopSec=10s',
                 '--property=RuntimeMaxSec=360s','/usr/bin/python3',str(root/'rollback.py'),
                 str(root),worker_unit])
        delay = 25 if name=='observation_deadline' else (19 if name=='old_client_timeout' else 17)
        status = 42 if name=='worker_failure' else 0
        argv = ['/usr/bin/python3',str(root/'worker.py'),str(root),str(delay),str(status)]
        if name=='startup_failure':
            argv = ['/usr/bin/python3',str(root/'nonexistent-worker.py')]
        started = time.monotonic()
        if name=='old_client_timeout':
            command = launcher.worker_command(worker_unit,argv)
            command.remove('--no-block')
            command.remove('--property=RemainAfterExit=yes')
            client = subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                client.communicate(timeout=15)
                raise AssertionError('Old synchronous command unexpectedly returned before 15s')
            except subprocess.TimeoutExpired:
                client.kill(); stdout,stderr = client.communicate(timeout=3)
            worker = wait_file(root/'started.json',time.monotonic()+1)
            state = ops.systemd_properties(worker_unit,'Service')
            assert launcher.controller.property_value(state,'MainPID','u')==worker['pid']
            os.kill(worker['pid'],0)
            result.update(client_pid=client.pid,client_returncode=client.returncode,
                          client_duration=time.monotonic()-started,worker_after_client_exit=worker,
                          original_command=command,stdout=stdout,stderr=stderr)
            completed = wait_file(root/'completed.json',started+22)
            assert completed['duration']>=19 and completed['status']==0
            result.update(outcome='BASELINE_REPRODUCED',completed=completed)
        else:
            submission = launcher.submit_worker(ops,worker_unit,argv)
            duration = time.monotonic()-started
            assert submission['state']=='SUBMITTED' and duration<15
            observe_deadline = time.monotonic()+3 if name=='observation_deadline' else started+22
            observation = launcher.observe_worker(ops,worker_unit,observe_deadline)
            expected = {'success':'WORKER_SUCCEEDED','worker_failure':'FORWARD_FAILED',
                        'startup_failure':'FORWARD_FAILED','observation_deadline':'OBSERVATION_DEADLINE'}[name]
            assert observation['state']==expected,observation
            result.update(submission=submission,submission_duration=duration,
                          observed_duration=time.monotonic()-started,observation=observation)
            if name in ('success','worker_failure'):
                completed = wait_file(root/'completed.json',started+22)
                assert completed['duration']>=17 and result['observed_duration']>15
                if name=='worker_failure':
                    assert observation['observation']['exit_status']==42
                result['completed']=completed
            if name=='startup_failure':
                assert observation['observation']['exit_status']==2
            if name=='observation_deadline':
                assert not (root/'completed.json').exists()
                assert launcher.controller.property_value(ops.systemd_properties(worker_unit,'Service'),'MainPID','u')>0
        timer = ops.systemd_properties(rollback_base+'.timer','Unit')
        assert launcher.controller.property_value(timer,'ActiveState','s')=='active'
        result['independent_timer_after_observation']=timer
        restored = wait_file(root/'restored.json',started+35)
        assert restored['ready'] and restored['release']=='old' and restored['ppid']==1
        result.update(restoration=restored,pass_case=True)
    except Exception as error:
        result.update(pass_case=False,error=repr(error))
    finally:
        journal=subprocess.run(['journalctl','-u',worker_unit,'-u',rollback_base+'.service',
                                '-u',rollback_base+'.timer','--no-pager','-o','json'],
                               capture_output=True,text=True,timeout=10)
        (root/'journal.jsonl').write_text(journal.stdout)
        # Cleanup only the names created by this fixture, never production.
        subprocess.run(['systemctl','stop',worker_unit,rollback_base+'.timer',rollback_base+'.service'],
                       capture_output=True,text=True,timeout=15)
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        shutil.copytree(root,evidence/name)
    return result


if __name__=='__main__':
    assert os.geteuid()==0,'Run with sudo -n; fixtures use only files and uniquely named transient units'
    os.umask(0o077)
    evidence=Path(sys.argv[1]);evidence.mkdir(mode=0o700)
    names=['old_client_timeout','success','worker_failure','startup_failure','observation_deadline']
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        results=list(pool.map(lambda name:run_case(name,evidence),names))
    (evidence/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps([dict(case=r['case'],passed=r['pass_case'],outcome=r.get('outcome',r.get('observation',{}).get('state')),error=r.get('error')) for r in results]))
    sys.exit(0 if all(r['pass_case'] for r in results) else 1)
