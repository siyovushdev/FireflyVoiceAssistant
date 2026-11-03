"""Real DemoMain/voice loop, isolated fake ALSA/STT/NPU and a private store."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

app,root=(Path(v).resolve() for v in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='firefly-wake-confirmation-') as directory:
    temp=Path(directory)
    invalid=[['--wake-stt-confirmation',v] for v in ('','0','TRUE','yes')]+[
        ['--wake-stt-confirmation'],
        ['--wake-stt-confirmation','true','--wake-command-mode','auto'],
        ['--wake-stt-confirmation','true','--wake-enabled','false']]
    for flags in invalid:
        store=temp/'must-not-exist'
        r=subprocess.run([str(app),'--store',str(store),*flags],capture_output=True,text=True,timeout=10)
        assert r.returncode==1 and not store.exists(),(flags,r)
    for name,mode,enabled in [('off','ack-confirm-reject',False),('confirmed','ack-confirm-success',True),('noise','ack-confirm-reject',True),('error','ack-confirm-error',True),('cancel','ack-confirm-slow',True)]:
        control=temp/(name+'-control');control.write_text(mode)
        store=temp/name;store.mkdir(mode=0o700);log=temp/(name+'.log')
        with log.open('w') as out:
            process=subprocess.Popen([str(app),'--store',str(store),'--init-config',str(root/'config/empty-home.json'),
                '--model','fixture-no-npu','--admin-port','0','--voice','--stt-model-dir','fixture-no-stt',
                '--vad-model','fixture-no-vad','--audio-device','fixture-no-alsa','--wake-python','fixture-no-child',
                '--wake-stt-confirmation',str(enabled).lower()],stdout=out,stderr=out,
                env={**os.environ,'FIREFLY_TEST_VOICE_CONTROL':str(control)})
        try:
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                value=log.read_text();assert process.poll() is None,value
                if name=='cancel':ready='FIXTURE confirmation STT started' in value
                elif name in ('noise','error'):ready='VOICE_STATE WAKE_REJECTED' in value and value.rfind('WAITING_WAKE')>value.rfind('WAKE_REJECTED')
                else:ready='VOICE_STATE LISTENING_COMMAND' in value
                if ready:break
                time.sleep(.005)
            else:raise AssertionError(value)
            assert ('VOICE_STATE VERIFYING_WAKE' in value)==enabled,value
            if name in ('noise','error','cancel'):assert 'BEEP_START' not in value,value
            else:assert 'BEEP_START' in value,value
            if name=='confirmed':
                assert value.index('WAKE_CONFIRMED')<value.index('BEEP_START'),value
                assert '"text":"афина"' in value,value
            if name in ('noise','error'):
                assert 'COMMAND_TRACE' not in value,value
                assert 'name_not_found' in value if name=='noise' else 'stt_unavailable' in value,value
            url=next(row.split()[1] for row in value.splitlines() if row.startswith('DEMO_LISTEN '))
            with urllib.request.urlopen(url+'/api/executions',timeout=5) as response:assert json.load(response)['executions']==[]
            before=(store/'configuration.json').read_bytes()
            process.send_signal(signal.SIGTERM);assert process.wait(timeout=10)==0,log.read_text()
            value=log.read_text();assert 'SHUTDOWN complete' in value,value
            assert (store/'configuration.json').read_bytes()==before
            assert value.count('FIXTURE capture opened owners=1')==1,value
            if name=='cancel':
                assert 'BEEP_START' not in value and 'VOICE_STATE WAKE_CONFIRMED' not in value,value
                assert value.rfind('FIXTURE confirmation STT finished')<value.rfind('FIXTURE STT released profile=command'),value
        finally:
            if process.poll() is None:process.terminate();process.wait(timeout=10)
print('7 invalid argv and 5 opt-in/default/noise/error/cancellation entrypoint cases passed; no hardware')
