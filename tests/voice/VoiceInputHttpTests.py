"""Real HTTP, ConfigStore, runtime generations; no mic/NPU/device bindings."""
import json,select,subprocess,sys,tempfile,urllib.request
from pathlib import Path
app,root=map(Path,sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='firefly-operational-input-') as directory:
    tmp=Path(directory);tmp.chmod(0o700);(tmp/'store').mkdir(mode=0o700)
    cfg=tmp/'initial.json';cfg.write_bytes((root/'tests/admin/v2/system.json').read_bytes());cfg.chmod(0o600)
    with (tmp/'log').open('w') as log:
        child=subprocess.Popen([str(app),str(tmp/'store'),str(cfg)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
        def msg(value):
            child.stdin.write(json.dumps(value,ensure_ascii=False)+'\n');child.stdin.flush()
            assert select.select([child.stdout],[],[],15)[0],'fixture timeout'
            return json.loads(child.stdout.readline())
        def api(path,body=None,method=None):
            req=urllib.request.Request(url+path,data=None if body is None else json.dumps(body).encode(),method=method,headers={'Content-Type':'application/json','X-Firefly-Admin':'1','Origin':url})
            with urllib.request.urlopen(req,timeout=15) as r:return json.load(r)
        def voice(g):return msg({'method':'voice','text':'включи свет','input_generation':g})
        try:
            assert select.select([child.stdout],[],[],15)[0];hello=json.loads(child.stdout.readline());url=hello['url']
            initial=api('/api/config');old=(tmp/'store/configuration.json').read_bytes()
            r=msg({'method':'input-defaults','after_beep':False,'confirmation':True});assert not r['input_policy']['after_beep']
            assert api('/api/status')['voice_input']=={'inline_commands':True,'mode':'auto','confirmation_enabled':True}
            msg({'method':'input-defaults','after_beep':True,'confirmation':True})
            generation=api('/api/config/active')['active']['generation_id']
            for enabled in (True,False,True):
                stored=api('/api/config');candidate=stored['configuration'];candidate['system']['voice_input']={'inline_commands':enabled}
                saved=api('/api/config',{'configuration':candidate,'expected':{'revision':stored['revision'],'hash':stored['hash']}},'PUT')
                assert api('/api/config/active')['active']['generation_id']==generation,'Save activated setting'
                api('/api/config/apply',{'expected':{'revision':saved['revision'],'hash':saved['hash']}})
                next_generation=api('/api/config/active')['active']['generation_id'];assert next_generation!=generation
                r=voice(generation);assert r['route']=='CANCELLED' and r['stt_calls']==0,r
                assert r['input_policy']=={'after_beep':not enabled,'generation_id':next_generation}
                status=api('/api/status');assert status['worker_pid']==hello['pid'] and status['voice_input']['inline_commands']==enabled
                assert api('/api/executions')['executions']==[]
                assert api('/api/config')['configuration']['system'].get('voice_dialogue')==initial['configuration']['system'].get('voice_dialogue')
                generation=next_generation
            r=voice(generation);assert r['stt_calls']==1 and r['route']!='CANCELLED',r
            assert old!=(tmp/'store/configuration.json').read_bytes()
            child.stdin.close();assert child.wait(timeout=10)==0
            # Whole history remains readable after reopening the real ConfigStore.
            second=subprocess.Popen([str(app),str(tmp/'store'),str(cfg)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
            try:
                assert select.select([second.stdout],[],[],15)[0];hello2=json.loads(second.stdout.readline());url=hello2['url']
                assert api('/api/status')['voice_input']['inline_commands'] is True
                assert api('/api/config')['configuration']['system']['voice_input']=={'inline_commands':True}
                second.stdin.close();assert second.wait(timeout=10)==0
            finally:
                if second.poll() is None:second.terminate();second.wait(timeout=10)
        finally:
            if child.poll() is None:child.terminate();child.wait(timeout=10)
print('PASS input preferences: CLI fallback, strict CAS Save→Apply hot toggle, generation cancellation before STT, F2 preserved, restart persistence, zero execution')
