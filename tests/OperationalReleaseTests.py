"""CPU/files only; does not access production units/stores/models or audio."""
import hashlib,importlib.util,json,os
from pathlib import Path
import shutil,subprocess,sys,tempfile,unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from packaging import firefly_release as r,firefly_checkpoint as c,voice_profile as v
APP=Path(sys.argv.pop(1)).resolve()
class OperationalRelease(unittest.TestCase):
 def test_legacy_unit_remains_exact_and_new_flags_are_bounded(self):
  old=r.render_unit(r.PRODUCTION_PREFIX)
  self.assertEqual(old,(ROOT/'packaging/firefly-voice-assistant.service').read_text())
  new=r.render_unit(r.PRODUCTION_PREFIX,True)
  extra='  --tts-model-dir '+r.PRODUCTION_PREFIX+'/models/tts \\\n  --wake-stt-confirmation true \\\n'
  self.assertEqual(new.replace(extra,''),old)
  self.assertEqual(new.count('--wake-stt-confirmation true'),1)
 def test_f2_pins_equal_runtime_and_exact_manifest(self):
  source=(ROOT/'src/voice/F2SpeechOutput.cpp').read_text()
  for name,(size,digest) in v.FILES.items():
   self.assertIn('{"'+name+'",'+str(size)+',"'+digest+'"}',source)
  self.assertEqual(len(v.metadata()['assets']),7)
 def test_asset_coverage_hash_modes_symlinks_and_no_byte_copy(self):
  with tempfile.TemporaryDirectory(prefix='firefly-profile-') as directory:
   base=Path(directory);a=base/'source';a.mkdir(mode=0o700)
   data=b'private cpu model fixture';f=a/'model';f.write_bytes(data);f.chmod(0o600)
   with patch.object(v,'FILES',{'model':(len(data),hashlib.sha256(data).hexdigest())}):
    v.verify_models(a)
    (a/'unexpected').write_text('x')
    with self.assertRaises(ValueError):v.verify_models(a)
    (a/'unexpected').unlink();f.chmod(0o666)
    with self.assertRaises(ValueError):v.verify_models(a)
    f.chmod(0o600);f.rename(a/'actual');f.symlink_to('actual')
    with self.assertRaises(ValueError):v.verify_models(a)
    f.unlink();(a/'actual').rename(f)
    v.provision_models(a,base/'target');self.assertNotEqual(f.stat().st_ino,(base/'target/model').stat().st_ino)
    self.assertEqual(f.read_bytes(),data);self.assertEqual(f.stat().st_mode&0o777,0o600)
    v.provision_models(a,base/'target')
    f.write_bytes(b'x'*len(data))
    v.verify_models(base/'target')
    (base/'target/model').write_bytes(b'x'*len(data))
    with self.assertRaises(ValueError):v.verify_models(base/'target')
 def test_explicit_release_checkpoint_keeps_legacy_m19_authority_closed(self):
  with tempfile.TemporaryDirectory(prefix='firefly-profile-store-') as directory:
   base=Path(directory);binary=base/'config';shutil.copy2(APP,binary);binary.chmod(0o755)
   pin=c.digest(binary);store=base/'store';store.mkdir(mode=0o700)
   subprocess.run([str(binary),'init','--store',str(store),'--file',str(ROOT/'tests/admin/v2/system.json')],check=True,stdout=subprocess.DEVNULL)
   with patch.object(c,'M19_CONFIG_SHA256','0'*64):
    with self.assertRaises(c.CheckpointError):c.create(store,base/'denied',binary)
    p=c.create(store,base/'checkpoint',binary,authority_sha256=pin)
    c.verify(base/'checkpoint',binary,p,authority_sha256=pin)
    with self.assertRaises(c.CheckpointError):c.verify(base/'checkpoint',binary,p,authority_sha256='0'*64)
    c.restore(store,base/'checkpoint',binary,p,base/'transaction',authority_sha256=pin)
    c.verify(base/'checkpoint',binary,p,authority_sha256=pin)
class UnitPublication(unittest.TestCase):
 def setUp(self):
  from packaging import operational_transition as o
  self.o=o;self.tmp=tempfile.TemporaryDirectory(prefix='firefly-unit-profile-');self.addCleanup(self.tmp.cleanup)
  self.base=Path(self.tmp.name);self.unit=self.base/'voice.service';self.dirs=[self.base/'etc.d',self.base/'run.d',self.base/'usr.d']
  for d in self.dirs:d.mkdir(mode=0o755)
  for name,value in [('UNIT',self.unit),('DROPIN_DIRS',self.dirs),('TRUST_UID',os.getuid())]:
   context=patch.object(o,name,value);context.start();self.addCleanup(context.stop)
  self.unit.write_text('approved old unit');self.unit.chmod(0o644)
  self.drop=self.dirs[0]/'90-amina-candidate.conf';self.drop.write_text('approved old model binding');self.drop.chmod(0o644)
  self.before=o.files();self.after={str(self.unit):{'text':'approved new unit','sha256':hashlib.sha256(b'approved new unit').hexdigest()}}
  self.calls=[]
 def run_command(self,args,**kwargs):self.calls.append(args)
 def test_complete_update_and_exact_old_restore(self):
  self.o.apply_bundle(self.after,self.before,self.after,self.run_command)
  self.assertEqual(self.o.files(),self.after);self.assertFalse(self.drop.exists())
  self.o.apply_bundle(self.before,self.before,self.after,self.run_command)
  self.assertEqual(self.o.files(),self.before);self.assertEqual(self.calls,[['systemctl','daemon-reload']]*2)
 def test_actual_crashes_during_unit_and_dropin_publication_restore(self):
  for point in ['unit_written','dropin_removed','unit_reloaded']:
   with self.subTest(point=point):
    child=os.fork()
    if child==0:
     try:self.o.apply_bundle(self.after,self.before,self.after,self.run_command,lambda step:os._exit(73) if point==step else None)
     except BaseException:os._exit(91)
     os._exit(92)
    _,status=os.waitpid(child,0);self.assertEqual(os.waitstatus_to_exitcode(status),73)
    self.o.apply_bundle(self.before,self.before,self.after,self.run_command)
    self.assertEqual(self.o.files(),self.before)
 def test_unknown_content_or_dropin_cannot_be_repaired_or_reloaded(self):
  self.unit.write_text('unapproved executable')
  with self.assertRaises(RuntimeError):self.o.apply_bundle(self.after,self.before,self.after,self.run_command)
  self.assertEqual(self.calls,[]);self.assertEqual(self.unit.read_text(),'unapproved executable')
  self.unit.write_text(self.before[str(self.unit)]['text'])
  foreign=self.dirs[1]/'malicious.conf';foreign.write_text('[Service]');foreign.chmod(0o644)
  with self.assertRaises(RuntimeError):self.o.apply_bundle(self.after,self.before,self.after,self.run_command)
  self.assertEqual(self.calls,[])
 def test_corrupt_bundle_and_unsafe_unit_fail_before_reload(self):
  import copy
  wrong=copy.deepcopy(self.after);wrong[str(self.unit)]['text']='different bytes'
  with self.assertRaises(RuntimeError):self.o.apply_bundle(wrong,self.before,self.after,self.run_command)
  self.unit.chmod(0o666)
  with self.assertRaises(RuntimeError):self.o.apply_bundle(self.after,self.before,self.after,self.run_command)
  self.assertEqual(self.calls,[])

class TargetAdmission(unittest.TestCase):
 def test_new_flags_require_matching_loaded_command_before_start(self):
  import copy
  spec=importlib.util.spec_from_file_location('legacy_recovery_matrix',ROOT/'tests/ReleaseRecoveryTests.py')
  legacy=importlib.util.module_from_spec(spec);spec.loader.exec_module(legacy)
  case=legacy.VoiceUnitGateTests('runTest');case.setUp()
  try:
   op=case.ops;new=copy.deepcopy(op.before)
   new['argv']=op.before['argv'][:-1]+['--tts-model-dir','/opt/firefly/voice-assistant/models/tts','--wake-stt-confirmation','true','']
   op.plan={'old':'old','new':'new','operational_transition':{'new':{'before':new}}};op.current=lambda:'new'
   with self.assertRaises(RuntimeError):op.start()
   self.assertEqual(case.commands,[])
   case.properties=legacy.voice_properties(new['argv'])
   op.start();self.assertEqual(case.commands,[['systemctl','start',op.unit]])
   case.commands.clear();case.file.write_text('changed after profile preparation')
   with self.assertRaises(RuntimeError):op.start()
   self.assertEqual(case.commands,[])
  finally:case.doCleanups()
 def test_profile_cannot_omit_or_corrupt_operator_dependencies(self):
  spec=importlib.util.spec_from_file_location('profile_controller',ROOT/'scripts/production-rollout.py')
  module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  op=object.__new__(module.Production);op.plan={'operational_transition':{'new':{}},'operational_tools_sha256':{}}
  with self.assertRaisesRegex(RuntimeError,'Incomplete operational helper closure'):op.supervisor_files()

class RecoveryRunnerAuthority(unittest.TestCase):
 def test_copied_runner_preserves_explicit_immutable_authority_before_arming(self):
  from types import SimpleNamespace
  from unittest.mock import Mock
  spec=importlib.util.spec_from_file_location('profile_runner',ROOT/'scripts/release-runner.py')
  runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
  controller=runner.load(ROOT/'scripts/production-rollout.py')
  with tempfile.TemporaryDirectory(prefix='firefly-runner-authority-') as directory:
   base=Path(directory);(base/'scripts').mkdir();script=base/'scripts/release-runner.py';shutil.copyfile(ROOT/'scripts/release-runner.py',script)
   pin=controller.sha(script)
   for operational in [False,True]:
    with self.subTest(operational=operational):
     marker=base/'RUNNER_STARTED'
     if marker.exists():marker.unlink()
     capsule={'release':'old','config_binary_sha256':'immutable-config'}
     helper=SimpleNamespace(control=Mock(return_value=capsule),pin=Mock(return_value='capsule-pin'),verify=Mock(return_value='receipt'),validate_capsule=Mock(),validate_release_capsule=Mock())
     checkpoint={'path':'/fixture/checkpoint','m19_config':'/fixture/config','pin':'point-pin','recovery_capsule':'/fixture/capsule','capsule_pin':'capsule-pin'}
     if operational:checkpoint['authority_kind']='immutable-operational-release'
     op=Mock();op.root=base;op.prefix=base/'prefix';op.plan_sha256='plan-pin'
     op.plan={'runner_sha256':pin,'m19_checkpoint':checkpoint,'recovery_only':True,'old':'old','rollback_timer':'fixture.timer','worker_unit':'fixture.service','rollback_unit':'recovery.service'}
     (base/'PREPARED').write_text(json.dumps({'plan_sha256':'plan-pin'}))
     module=SimpleNamespace(Production=Mock(return_value=op),sha=controller.sha,utc=controller.utc)
     def loaded(path):
      if path.name=='deployment-launcher.py':return SimpleNamespace(controller=module)
      if path.name=='firefly_checkpoint.py':return helper
      self.fail(str(path))
     with patch.object(runner,'load',side_effect=loaded),patch.object(runner,'__file__',str(script)),patch.object(runner,'arm',side_effect=RuntimeError('stop before real timer')) as arm:
      runner.run(base,'fixture-authorized')
     kwargs={'authority_sha256':'immutable-config'} if operational else {}
     helper.verify.assert_called_once_with(Path('/fixture/checkpoint'),Path('/fixture/config'),'point-pin',**kwargs)
     validator=helper.validate_release_capsule if operational else helper.validate_capsule
     validator.assert_called_once_with(capsule,'receipt','point-pin',base/'prefix/releases/old')
     (helper.validate_capsule if operational else helper.validate_release_capsule).assert_not_called()
     op.bootstrap_preflight.assert_called_once_with(armed=False);arm.assert_called_once()

class WholeStoreTransition(unittest.TestCase):
 def setUp(self):
  from packaging import operational_transition as o
  self.o=o;self.temp=tempfile.TemporaryDirectory(prefix='firefly-profile-flow-');self.addCleanup(self.temp.cleanup)
  self.base=Path(self.temp.name);self.store=self.base/'store';self.store.mkdir(mode=0o700)
  self.prefix=self.base/'prefix';self.unit=self.base/'voice.service';self.dirs=[self.base/'etc.d',self.base/'run.d',self.base/'usr.d']
  for d in self.dirs:d.mkdir()
  for name,value in [('UNIT',self.unit),('DROPIN_DIRS',self.dirs),('TRUST_UID',os.getuid()),('STORE',self.store),('PREFIX',self.prefix)]:
   context=patch.object(o,name,value);context.start();self.addCleanup(context.stop)
  # Only the mount-persistence boundary is substituted. Real config executables,
  # file ownership/modes/hashes, CAS/history proof and renameat2 are exercised.
  guard=patch.object(c,'require_persistent');guard.start();self.addCleanup(guard.stop)
  self.prefix.mkdir(mode=0o755);(self.prefix/'releases').mkdir(mode=0o755)
  identity=dict(head='a'*40,source_sha256='b'*64,built_utc='2026-11-05T00:00:00Z',dirty=False,admin_assets_sha256='c'*64)
  boundary=patch.object(r,'build_identity',return_value=identity);boundary.start();self.addCleanup(boundary.stop)
  for rid in ['old','new']:
   root=self.prefix/'releases'/rid;root.mkdir(mode=0o755);(root/'bin').mkdir(mode=0o755)
   for name in ['firefly-config','firefly-demo']:
    shutil.copy2(APP,root/'bin'/name);(root/'bin'/name).chmod(0o755)
   (root/'share/firefly/systemd').mkdir(parents=True)
   (root/'share/firefly/systemd/firefly-voice-assistant.service').write_text(r.render_unit(str(self.prefix),rid=='new'))
   (root/'share/firefly/systemd/firefly-voice-assistant.service').chmod(0o644)
   doc={'build':identity,'artifacts':[{'path':n} for n in ['bin/firefly-demo','bin/firefly-config','share/firefly/systemd/firefly-voice-assistant.service']]}
   if rid=='new':doc['operational_voice']=v.metadata()
   (root/'manifest.json').write_text(json.dumps(doc))
  self.old=self.prefix/'releases/old/bin/firefly-config';self.new=self.prefix/'releases/new/bin/firefly-config'
  self.cfg=json.loads((ROOT/'tests/admin/v2/system.json').read_text());self.input=self.base/'input.json';self.input.write_text(json.dumps(self.cfg))
  self.call(self.old,'init','--store',self.store,'--file',self.input);self.lkg(self.store)
  self.incoming=self.base/'incoming';c.clone(self.store,self.incoming)
  self.cfg['system']['voice_input']={'inline_commands':True};self.cfg['system']['voice_dialogue']={'responses':'brief','questions_enabled':True}
  self.save(self.incoming,self.cfg);self.lkg(self.incoming)
  self.unit.write_text(r.render_unit(str(self.prefix)));self.unit.chmod(0o644)
  old=c.invoke(self.old,self.store,'audit','--store',self.store)
  before={'status':{'active':{'revision':old['revision'],'hash':old['content_sha256'],'generation_id':1},'stored':{'revision':old['revision'],'hash':old['content_sha256']}},'argv':o.unit_argv(self.unit.read_text()),'unit_sha256':c.digest(self.unit),'store_manifest':c.legacy_inventory(c.inventory(self.store))}
  self.root=self.base/'lease';self.root.mkdir(mode=0o700)
  self.profile=o.prepare(self.root,self.prefix,'old','new',before,self.incoming,self.base/'persistent',lambda tree:c.legacy_inventory(c.inventory(tree)))
  self.plan={'old':'old','new':'new','operational_transition':self.profile};self.calls=[]
 def call(self,binary,*args):
  return json.loads(subprocess.check_output([str(binary),*map(str,args)],text=True))
 def save(self,store,cfg):
  self.input.write_text(json.dumps(cfg));a=self.call(self.new,'audit','--store',store)
  return self.call(self.new,'save','--store',store,'--file',self.input,'--expected-revision',a['revision'],'--expected-hash',a['content_sha256'])
 def lkg(self,store):
  a=self.call(self.new,'audit','--store',store)
  shutil.copy2(store/'configuration.json',store/'last_known_good_snapshot.json')
  f=store/'last_known_good.json';f.write_text(json.dumps({'revision':a['revision'],'hash':a['content_sha256']}));f.chmod(0o600)
 def command(self,args,**kw):self.calls.append(args)
 def test_forward_then_rollback_preserves_both_complete_histories_and_cas(self):
  old=c.inventory(self.store);incoming=c.inventory(self.incoming)
  self.o.restore_target(self.root,self.plan,'new',self.command)
  self.assertEqual(c.inventory(self.store),incoming)
  self.assertEqual(json.loads((self.store/'configuration.json').read_text())['system']['voice_input'],{'inline_commands':True})
  edited=json.loads((self.store/'configuration.json').read_text());edited['system']['name']='new owner edit';self.save(self.store,edited)
  newer=c.inventory(self.store)
  self.o.restore_target(self.root,self.plan,'old',self.command)
  self.assertEqual(c.inventory(self.store),old)
  preserved=Path(self.profile['old']['transaction'])/'preserved-store';self.assertEqual(c.inventory(preserved),newer)
  self.call(self.old,'audit','--store',self.store);self.call(self.old,'revisions','--store',self.store)
  cfg=json.loads((self.store/'configuration.json').read_text());cfg['system']['name']='old CAS still works';self.save(self.store,cfg)
  self.assertEqual(c.inventory(preserved),newer)
 def test_real_crash_after_store_exchange_before_unit_publication_recovers_old(self):
  old=c.inventory(self.store)
  child=os.fork()
  if child==0:
   self.o.apply_bundle=lambda *a,**kw:os._exit(73)
   try:self.o.restore_target(self.root,self.plan,'new',self.command)
   except BaseException:os._exit(91)
   os._exit(92)
  _,status=os.waitpid(child,0);self.assertEqual(os.waitstatus_to_exitcode(status),73)
  self.o.restore_target(self.root,self.plan,'old',self.command)
  self.assertEqual(c.inventory(self.store),old);self.assertEqual(self.o.files(),self.profile['old']['unit_bundle'])
 def test_recovery_restores_partial_transition_when_pointer_still_names_old(self):
  from types import SimpleNamespace
  import time
  from packaging.firefly_recovery import recover
  spec=importlib.util.spec_from_file_location('transition_controller',ROOT/'scripts/production-rollout.py')
  controller=importlib.util.module_from_spec(spec);spec.loader.exec_module(controller)
  expected=c.inventory(self.store);self.o.restore_target(self.root,self.plan,'new',self.command)
  # Crash before stage-release activation: current is still old, state is new.
  restored=[];running=[False]
  def gates(target):
   if not running[0] or c.inventory(self.store)!=expected:raise RuntimeError('not the old ready state')
   self.o.assert_bundle(self.profile['old']['unit_bundle'])
   return {'ready':True,'release':'old'}
  def switch(target):
   self.o.restore_target(self.root,self.plan,target,self.command);restored.append(target)
  op=SimpleNamespace(plan=self.plan,current=lambda:'old',gates=gates,stop=lambda:running.__setitem__(0,False),
   resource_snapshot=lambda:{'owners':[],'cgroup_pids':[],'main_pid':0,'control_pid':0,'active_state':'inactive'},
   record=lambda *a:None,switch=switch,start=lambda:running.__setitem__(0,True),wait_ready=lambda target,seconds:gates(target))
  op.requires_restore=lambda target:controller.Production.requires_restore(op,target)
  result=recover(op,'old',time.monotonic()+10)
  self.assertTrue(result['ready']);self.assertEqual(restored,['old']);self.assertEqual(c.inventory(self.store),expected)
 def test_durable_release_capsule_rejects_changed_authority_identity_catalog_or_mode(self):
  import copy
  profile=self.profile;state=profile['old'];capsule=profile['rollback_capsule']
  receipt=c.verify(Path(state['checkpoint']),self.old,state['checkpoint_pin'],authority_sha256=state['binary_sha256'])
  root=self.prefix/'releases/old'
  c.validate_release_capsule(capsule,receipt,state['checkpoint_pin'],root)
  for field in ['format','authority','checkpoint_pin','config_binary_sha256','binary_sha256','manifest_sha256','release','identity','before','artifact_modes']:
   with self.subTest(field=field):
    wrong=copy.deepcopy(capsule)
    if field=='identity':wrong[field]['dirty']=True
    elif field=='before':wrong[field]['status']['stored']['revision']+=1
    elif field=='artifact_modes':wrong[field]['bin/firefly-config']=0o777
    else:wrong[field]='corrupt'
    with self.assertRaises(c.CheckpointError):c.validate_release_capsule(wrong,receipt,state['checkpoint_pin'],root)
 def test_corrupt_checkpoint_or_wrong_binary_blocks_before_mutation(self):
  old=c.inventory(self.store)
  self.profile['new']['binary']=self.profile['old']['binary']
  with self.assertRaises(RuntimeError):self.o.restore_target(self.root,self.plan,'new',self.command)
  self.assertEqual(c.inventory(self.store),old);self.assertEqual(self.calls,[])
  self.profile['new']['binary']=str(self.new)
  point=Path(self.profile['new']['checkpoint'])/'store/configuration.json';point.write_bytes(point.read_bytes()+b' ')
  with self.assertRaises(c.CheckpointError):self.o.restore_target(self.root,self.plan,'new',self.command)
  self.assertEqual(c.inventory(self.store),old);self.assertEqual(self.calls,[])

if __name__=='__main__':unittest.main()
