#!/usr/bin/env python3
"""Local process lifetime and transport safety; no remote/device operations."""
import importlib.util
import copy
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pc', Path(__file__).resolve().parents[1] / 'scripts/m19-pc-browser.py')
pc = importlib.util.module_from_spec(spec); spec.loader.exec_module(pc)

class PcTransportTests(unittest.TestCase):
    def test_diagnostic_classification_is_bound_to_failed_long_request(self):
        trace={'stage':'decode','error_code':'invalid_ai_response','error':'Invalid JSON',
               'rejected_raw_hex':b'{broken'.hex(),'rejected_raw_bytes':7,'rejected_raw_truncated':False}
        case={'case':'long','input_bytes':1606,'state':None,'private_status':'planning_failed',
              'message_incomplete':True,'trace':[trace,trace]}
        report={'profile':'ADMIN4096_DIAGNOSTIC','real_rkllm':True,'passed':False,
                'cycles':[{'restored':False,'cases':[case]}],
                'http_error':{'path':'/api/setup/message','method':'POST','status':409,'capture_complete':True}}
        self.assertEqual(pc.diagnostic_outcome(report,1),'AI_REJECTION_CAPTURED')
        for field,value in [('path','/api/setup/cancel'),('method','GET'),('status',503),('capture_complete',False)]:
            bad=copy.deepcopy(report);bad['http_error'][field]=value
            self.assertIsNone(pc.diagnostic_outcome(bad,1))
        self.assertIsNone(pc.diagnostic_outcome(report,0))
        for field,value in [('state',{'status':'editing'}),('private_status','editing'),('message_incomplete',False),('input_bytes',136)]:
            bad=copy.deepcopy(report);bad['cycles'][0]['cases'][0][field]=value
            self.assertIsNone(pc.diagnostic_outcome(bad,1))
        bad=copy.deepcopy(report);bad['cycles'][0]['cases'][0]['trace'][1]['error_code']='runtime_rejected'
        self.assertIsNone(pc.diagnostic_outcome(bad,1))

    def test_diagnostic_success_does_not_claim_stress_acceptance(self):
        report={'profile':'ADMIN4096_DIAGNOSTIC','real_rkllm':True,'passed':True,
                'cycles':[{'restored':True,'cases':[{'case':'long','input_bytes':1606}]}]}
        self.assertEqual(pc.diagnostic_outcome(report,0),'AI_REJECTION_NOT_REPRODUCED')
        self.assertIsNone(pc.diagnostic_outcome(report,1))

    def test_clean_candidate_bridge_is_explicit_and_shell_quoted(self):
        import json
        clean = '/home/firefly/FireflyVoiceAssistantLinux-m19-rollout'
        transport = pc.Transport('firefly@host', '/control', clean)
        with patch.object(pc.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '{}', '')) as run:
            transport.call('offline-start')
            self.assertIn(clean + '/scripts/m19-pc-bridge.py', run.call_args.args[0][-1])
            self.assertEqual(json.loads(run.call_args.kwargs['input']), {'action':'offline-start'})
        for path in ['relative', '/home/../tmp', '/home//firefly']:
            with self.assertRaises(ValueError): pc.Transport('host', None, path)

    def test_default_profile_is_stable_for_pre_window_live_recheck(self):
        from types import SimpleNamespace
        args=SimpleNamespace(chromium='/bin/true', node='/bin/true', host='firefly@host',
                             voice_mode='owner-live', remote_repo=pc.REMOTE_REPO)
        with patch.dict(os.environ, {'DISPLAY':':1'}), \
             patch.object(pc.subprocess,'check_output',return_value='fixture-version'):
            profile=pc.browser_profile(args)
            self.assertEqual(profile['remote_repo'], pc.REMOTE_REPO)
            self.assertEqual(pc.browser_profile(args), profile)

    def test_owner_cues_are_forwarded_once_without_reading_partial_json(self):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            events=Path(tmp)/'events.jsonl'
            rows=[{'phase':'restore_pass'},
                  {'phase':'owner_waiting_ready','on':True,'readyFile':'on.json','requestId':'on-request','timeoutSeconds':120},
                  {'phase':'owner_voice_ready','on':True,'instruction':'ON','timeoutSeconds':90},
                  {'phase':'owner_voice_ready','on':False,'instruction':'OFF','timeoutSeconds':90}]
            events.write_text(''.join(json.dumps(r)+'\n' for r in rows)+'{"partial":')
            stop=type('S',(),{'wait':lambda self,t:True})()
            cues=[]; pc.relay_owner_cues(events,stop,cues.append)
            self.assertEqual([c['on'] for c in cues],[True,True,False])
            self.assertEqual(cues[0]['phase'],'OWNER_WAITING_READY')
            self.assertEqual(cues[0]['readyFile'],'on.json')
            self.assertEqual([c['instruction'] for c in cues[1:]],['ON','OFF'])

    def test_owner_readiness_ack_cannot_be_reused_for_another_phase(self):
        import shutil
        adapter = Path(__file__).resolve().parents[1] / 'tests/admin/v2/M19Remote.js'
        script = '''const fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const assert=require('node:assert/strict');const {consumeOwnerReady,verifyOwnerBaseline,ownerReadyTimeoutSeconds}=require(process.argv[1]);
const root=fs.mkdtempSync(path.join(os.tmpdir(),'m19-ready-'));const file=path.join(root,'ready.json');
try { assert.equal(consumeOwnerReady(file,'on-id',true),false);
fs.writeFileSync(file,JSON.stringify({requestId:'old-id',on:true}));
assert.throws(()=>consumeOwnerReady(file,'on-id',true));
fs.writeFileSync(file,JSON.stringify({requestId:'on-id',on:false}));
assert.throws(()=>consumeOwnerReady(file,'on-id',true));
fs.writeFileSync(file,JSON.stringify({requestId:'on-id',on:true}));
assert.equal(consumeOwnerReady(file,'on-id',true),true);
assert.equal(consumeOwnerReady(file,'on-id',true),false);
assert.equal(fs.existsSync(file+'.consumed'),true);
verifyOwnerBaseline([],[],0,0);
assert.deepEqual(verifyOwnerBaseline([{method:'GET',path:'/relay/1',body:''}],[],0,0),[{method:'GET',path:'/relay/1',body:''}]);
assert.throws(()=>verifyOwnerBaseline([{method:'POST'}],[{source:'voice'}],0,0));
assert.throws(()=>verifyOwnerBaseline([],[{source:'voice'}],0,0));
assert.throws(()=>verifyOwnerBaseline([{method:'POST',path:'/relay/1',body:'{"on":false}'}],[],0,0));
assert.throws(()=>verifyOwnerBaseline([{method:'GET',path:'/other',body:''}],[],0,0));
assert.throws(()=>verifyOwnerBaseline([{method:'GET',path:'/relay/1',body:'{"on":false}'}],[],0,0));
assert.equal(ownerReadyTimeoutSeconds({remainingSeconds:237,readyReserveSeconds:140}),87);
assert.equal(ownerReadyTimeoutSeconds({remainingSeconds:400,readyReserveSeconds:140}),120);
assert.throws(()=>ownerReadyTimeoutSeconds({remainingSeconds:150,readyReserveSeconds:140}));
assert.throws(()=>ownerReadyTimeoutSeconds({remainingSeconds:237,readyReserveSeconds:130}));
} finally { fs.rmSync(root,{recursive:true,force:true}); }
'''
        env=dict(os.environ);env.pop('FIREFLY_M19_REMOTE_PLAN',None)
        r=subprocess.run([shutil.which('node'),'-e',script,str(adapter)],env=env,capture_output=True,text=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stderr)

    def test_owner_voice_evidence_rejects_text_execution_and_wrong_configuration(self):
        import shutil
        adapter = Path(__file__).resolve().parents[1] / 'tests/admin/v2/M19Remote.js'
        script = '''const assert=require('node:assert/strict');
const {verifyVoiceEvidence}=require(process.argv[1]);
const status={active:{generation_id:2},last_voice:{audio_detected:true,command_text:'включи лампу в кабинете',
transcript:'включи лампу в кабинете',stt_ms:300,inline:false,wake_prefix_removed:false,
command:{input:'включи лампу в кабинете',generation_id:2,hash:'private',safety:'PASS',domain:'PASS',execution:{success:true,targetId:'lamp',actionId:'on'}}}};
const events=[{source:'voice',generation_id:2,execution:{success:true,targetId:'lamp',actionId:'on'}}];
const check=(s,e)=>verifyVoiceEvidence(s,e,0,'включи лампу в кабинете','private',true,'lamp','on');
check(status,events);
for(const mutate of [s=>s.last_voice.command.hash='production',s=>s.last_voice.command.safety='REJECT',
s=>s.last_voice.command_text='выключи лампу в кабинете',s=>s.last_voice.stt_ms=0,
s=>s.last_voice.inline=true,s=>s.last_voice.transcript='другая речь',s=>s.last_voice.command.input='другая речь',
s=>s.last_voice.command.generation_id=1,s=>s.last_voice.command.execution.targetId='other']) { const s=structuredClone(status); mutate(s); assert.throws(()=>check(s,events)); }
assert.throws(()=>check(status,[{source:'text',execution:{success:true}}]));
assert.throws(()=>check(status,[...events,...events]));
const wrong=structuredClone(events);wrong[0].execution.actionId='off';assert.throws(()=>check(status,wrong));
'''
        env=dict(os.environ); env.pop('FIREFLY_M19_REMOTE_PLAN',None)
        r=subprocess.run([shutil.which('node'),'-e',script,str(adapter)],env=env,capture_output=True,text=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stderr)

    def test_hardware_startup_waits_for_remote_ready_before_browser(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(pc.time,'sleep'):
            transport = type('T',(),{})()
            from unittest.mock import Mock
            transport.call = Mock(side_effect=[{'ready':False}, {'ready':True,'status':{'voice':'WAITING_WAKE'}}])
            result = pc.wait_test_ready(transport,'/remote',Path(tmp))
            self.assertTrue(result['ready'])
            self.assertEqual(transport.call.call_count,2)
            self.assertTrue((Path(tmp)/'runtime-ready.json').exists())

    def test_closed_window_aborts_readiness_without_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            from unittest.mock import Mock
            transport = type('T',(),{})()
            transport.call = Mock(side_effect=RuntimeError('Watchdog closed window'))
            with self.assertRaises(RuntimeError): pc.wait_test_ready(transport,'/remote',Path(tmp))
            transport.call.assert_called_once()

    def test_timeout_stops_own_child_group_preserving_unrelated_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            pidfile = Path(tmp) / 'child.pid'
            unrelated = subprocess.Popen([sys.executable,'-c','import time;time.sleep(10)'])
            command = [sys.executable,'-c',
                'import subprocess,sys,time;from pathlib import Path;'
                'p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(10)"]);'
                'Path(sys.argv[1]).write_text(str(p.pid));time.sleep(10)',str(pidfile)]
            try:
                with open(os.devnull,'w') as log:
                    with self.assertRaises(subprocess.TimeoutExpired): pc.run_driver(command,os.environ,log,.5)
                child = Path('/proc') / pidfile.read_text()
                end = time.monotonic()+2
                while child.exists() and time.monotonic()<end:
                    if (child/'stat').read_text().rsplit(')',1)[1].split()[0]=='Z': break
                    time.sleep(.05)
                self.assertTrue(not child.exists() or (child/'stat').read_text().rsplit(')',1)[1].split()[0]=='Z')
                self.assertIsNone(unrelated.poll())
            finally:
                unrelated.terminate(); unrelated.wait(timeout=3)

    def test_driver_failure_return_code_is_preserved_after_cleanup(self):
        with open(os.devnull,'w') as log:
            self.assertEqual(pc.run_driver([sys.executable,'-c','raise SystemExit(7)'],os.environ,log,3),7)

    def test_live_driver_refuses_native_firefly_style_launch_before_chromium(self):
        import shutil
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ); env.pop('FIREFLY_M19_REMOTE_PLAN',None)
            script = Path(__file__).resolve().parents[1] / 'tests/admin/v2/M19ChromiumTests.js'
            r = subprocess.run([shutil.which('node'),str(script),'/missing-app','/missing-worker',
                '/missing-root','/missing-chromium',tmp,'--controlled-live',
                'http://127.0.0.1:18089','/missing-log','http://127.0.0.1:18765'],
                env=env,capture_output=True,text=True,timeout=5)
            self.assertEqual(r.returncode,1)
            self.assertIn('must run on the PC with SSH transport',r.stderr)
            self.assertNotIn('spawn /missing-chromium',r.stderr)

    def test_tunnel_is_loopback_only_for_offline_and_live(self):
        t = pc.Transport('firefly@192.168.68.111','/tmp/control')
        with patch.object(pc.subprocess,'run',return_value=type('R',(),{'returncode':0,'stderr':''})()) as run:
            t.tunnel(45678); t.tunnel(18089)
            calls=[c.args[0] for c in run.call_args_list]
            self.assertIn('127.0.0.1:18089:127.0.0.1:45678',calls[0])
            self.assertIn('127.0.0.1:18089:127.0.0.1:18089',calls[1])

    def test_trial_requires_new_explicit_start_before_remote_operations(self):
        with patch.object(sys,'argv',['pc','trial','--artifacts','/tmp/not-created']), \
             patch.object(pc.Transport,'call') as call:
            with self.assertRaises(SystemExit): pc.main()
            call.assert_not_called()

if __name__ == '__main__': unittest.main(verbosity=2)
