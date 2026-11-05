"""Optional, sealed unit/whole-store transition inside the existing supervisor.

Never called by legacy plans. No startup, release-pointer or broker operation.
"""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
from . import firefly_checkpoint as checkpoint

UNIT = Path('/etc/systemd/system/firefly-voice-assistant.service')
STORE = Path('/var/lib/firefly/store')
PREFIX = Path('/opt/firefly/voice-assistant')
DROPIN_DIRS = [Path('/etc/systemd/system/firefly-voice-assistant.service.d'),
               Path('/run/systemd/system/firefly-voice-assistant.service.d'),
               Path('/usr/lib/systemd/system/firefly-voice-assistant.service.d')]
TRUST_UID = 0


def _regular(path):
    for parent in path.parents:
        s=parent.lstat()
        if not stat.S_ISDIR(s.st_mode) or s.st_uid not in (0,TRUST_UID) or (s.st_mode & 0o022 and not (parent==Path('/tmp') and s.st_mode & stat.S_ISVTX)):
            raise RuntimeError('Untrusted unit parent: '+str(parent))
    fd=os.open(path,os.O_RDONLY|os.O_CLOEXEC|os.O_NONBLOCK|os.O_NOFOLLOW)
    s=os.fstat(fd)
    if (not stat.S_ISREG(s.st_mode) or s.st_nlink!=1 or s.st_uid!=TRUST_UID or
            s.st_gid!=TRUST_UID or stat.S_IMODE(s.st_mode)!=0o644):
        os.close(fd);raise RuntimeError('Untrusted unit file: '+str(path))
    with os.fdopen(fd,'r') as file:
        text=file.read()
        after=os.fstat(file.fileno())
        stable=lambda x:(x.st_dev,x.st_ino,x.st_size,x.st_uid,x.st_gid,x.st_mode,x.st_mtime_ns,x.st_ctime_ns)
        if stable(after)!=stable(s):raise RuntimeError('Unit file changed during read')
    return dict(text=text,sha256=hashlib.sha256(text.encode()).hexdigest())


def files():
    paths=[UNIT]
    for parent in DROPIN_DIRS:
        if parent.is_symlink():raise RuntimeError('Symlink unit directory')
        if parent.exists():
            for path in parent.iterdir():
                if path.suffix=='.conf':paths.append(path)
    return {str(p):_regular(p) for p in paths}


def validate_bundle(bundle):
    if not isinstance(bundle,dict) or str(UNIT) not in bundle:
        raise RuntimeError('Incomplete unit bundle')
    allowed={str(UNIT)}|{str(d/name) for d in DROPIN_DIRS
                       for name in ['90-amina-candidate.conf','91-wake-debug.conf']}
    if not set(bundle)<=allowed:raise RuntimeError('Unapproved unit path')
    for p,item in bundle.items():
        if (set(item)!={'text','sha256'} or not isinstance(item['text'],str) or
                hashlib.sha256(item['text'].encode()).hexdigest()!=item['sha256']):
            raise RuntimeError('Unit bundle content mismatch')


def unit_argv(text):
    import shlex
    line=text.replace('\\\n',' ').split('ExecStart=',1)[1].split('\n',1)[0]
    return shlex.split(line)+['']


def bound_sources(bundle):
    """Closed existing wake overrides: no executable/library/source guessing."""
    refs={}
    text='\n'.join(item['text'] for path,item in bundle.items() if path!=str(UNIT))
    pins={
      '/opt/firefly/wake-debug-tools/candidate.tflite': '24000c0e3f9fd3249f56e5f836719aba8f7a3f23c1f4a2c571bdf0bf6cc8a1dd',
      '/opt/firefly/wake-debug-tools/candidate-worker.py': 'e9db1780ce3f560b04b4c1b94db6b4e018680d3582d8266ceed76cd14a45ffff',
      '/opt/firefly/wake-debug-tools/debug_worker.py': '7b4cc2bbf056b7f0c888d1ac4193ad29b5791b70daac76f726a9e0263bd7a55d',
    }
    for name,pin in pins.items():
        if name not in text:continue
        path=Path(name)
        for parent in path.parents:
            s=parent.lstat()
            if not stat.S_ISDIR(s.st_mode) or s.st_uid!=0 or s.st_mode&0o022:
                raise RuntimeError('Untrusted bound source parent')
        fd=os.open(path,os.O_RDONLY|os.O_CLOEXEC|os.O_NOFOLLOW|os.O_NONBLOCK)
        with os.fdopen(fd,'rb') as file:
            s=os.fstat(file.fileno())
            if not stat.S_ISREG(s.st_mode) or s.st_uid!=0 or s.st_nlink!=1 or s.st_mode&0o6022:
                raise RuntimeError('Unsafe bound wake source')
            digest=hashlib.sha256(file.read()).hexdigest()
        if digest!=pin:raise RuntimeError('Unapproved bound wake source: '+name)
        refs[name]=pin
    return refs


def capture_legacy(legacy_text, release, debug_config=None):
    """Only the exact existing Amila/debug overrides are admitted as baseline.

    No arbitrary drop-in or ExecStart/hardening change is authorized by capture.
    """
    bundle=files();validate_bundle(bundle)
    if bundle[str(UNIT)]['text']!=legacy_text:raise RuntimeError('Legacy unit differs from renderer')
    target='/opt/firefly/voice-assistant/releases/'+release+'/share/firefly/wake/'
    allowed={str(DROPIN_DIRS[0]/'90-amina-candidate.conf'):
        '[Service]\nBindReadOnlyPaths=/opt/firefly/wake-debug-tools/candidate.tflite:'+target+'amina-v3.tflite\n'
        'BindReadOnlyPaths=/opt/firefly/wake-debug-tools/candidate-worker.py:'+target+'amina_worker.py\n'}
    if debug_config is not None:
        output=Path(debug_config['output'])
        if (not output.is_absolute() or output.resolve()!=output or
                output.parent!=Path('/home/firefly/wake-debug/sessions')):
            raise RuntimeError('Unapproved existing debug output')
        allowed[str(DROPIN_DIRS[1]/'91-wake-debug.conf')]=(
            '[Service]\nEnvironment=FIREFLY_WAKE_DEBUG_CONFIG=/run/firefly-wake-debug/session.json\n'
            'BindReadOnlyPaths=\nBindReadOnlyPaths=/opt/firefly/wake-debug-tools/candidate.tflite:'+target+'amina-v3.tflite\n'
            'BindReadOnlyPaths=/opt/firefly/wake-debug-tools/debug_worker.py:'+target+'amina_worker.py\n'
            'ReadWritePaths='+str(output)+'\n')
    for p,item in bundle.items():
        if p!=str(UNIT) and item['text'].strip()!=allowed.get(p,'').strip():raise RuntimeError('Unapproved existing drop-in: '+p)
    return bundle


def assert_bundle(expected):
    validate_bundle(expected)
    if files()!=expected:raise RuntimeError('Voice unit bundle differs from approved target')


def apply_bundle(wanted, before, after, run, fault=lambda _:None):
    """Recover only combinations of approved bytes; never normalize unknown edits.

    Repeated writes after a crash are safe. Complete loaded admission is still
    mandatory in Production.voice_unit_gate before any service start.
    """
    for bundle in (wanted,before,after):validate_bundle(bundle)
    current=files();known=set(before)|set(after)
    if not set(current)<=known:raise RuntimeError('Unexpected unit file; no repair permitted')
    for p,item in current.items():
        if item not in [b[p] for b in (before,after) if p in b]:
            raise RuntimeError('Unit changed outside approved transition: '+p)
    for name,item in wanted.items():
        path=Path(name)
        if path.exists() and _regular(path)==item:continue
        if not path.parent.exists():path.parent.mkdir(mode=0o755)
        fd,tmp=tempfile.mkstemp(prefix='.firefly-unit-',dir=path.parent)
        try:
            os.fchmod(fd,0o644)
            with os.fdopen(fd,'wb') as file:
                file.write(item['text'].encode());file.flush();os.fsync(file.fileno())
            os.replace(tmp,path);checkpoint.sync_dir(path.parent)
        finally:
            if Path(tmp).exists():Path(tmp).unlink()
        fault('unit_written')
    for name in set(current)-set(wanted):
        path=Path(name)
        if _regular(path)!=current[name]:raise RuntimeError('Drop-in changed during removal')
        path.unlink();checkpoint.sync_dir(path.parent);fault('dropin_removed')
    assert_bundle(wanted)
    run(['systemctl','daemon-reload'],timeout=15)
    fault('unit_reloaded')


def restore_target(root, plan, target, run):
    """Whole directory exchange + literal unit publication, with original timer.

    Caller must have the original operation lock, resource barrier and active
    supervision. Checkpoint transactions preserve every replaced history/tree.
    """
    profile=plan['operational_transition'];side='new' if target==plan['new'] else 'old'
    if target not in (plan['old'],plan['new']):raise RuntimeError('Unknown release target')
    state=profile[side]
    if Path(state['binary']).resolve()!=PREFIX/'releases'/target/'bin/firefly-config':
        raise RuntimeError('Checkpoint authority must be the exact target executable')
    if hashlib.sha256(Path(state['binary']).read_bytes()).hexdigest()!=state['binary_sha256']:
        raise RuntimeError('Checkpoint authority changed')
    # The supervisor has a frozen, explicitly authorized plan. In-process
    # helpers do not inherit Production.run's subprocess environment. Pass
    # admission explicitly; root/stopped/cgroup checks remain mandatory.
    owner_authorized = False
    if STORE == Path('/var/lib/firefly/store'):
        owner_authorized = isinstance(plan.get('authorization'), str) and bool(plan['authorization'])
        if not owner_authorized:raise RuntimeError('Frozen owner authorization required')
    checkpoint.quiescent(STORE, owner_authorized=owner_authorized)
    checkpoint.verify(Path(state['checkpoint']),Path(state['binary']),state['checkpoint_pin'],authority_sha256=state['binary_sha256'])
    # Re-entry after a completed rollback retains its already-proven live tree.
    transaction=Path(state['transaction'])
    if transaction.exists() and (transaction/'VERIFIED.json').exists():
        checkpoint.prove_catalog(STORE,Path(state['binary']),state['binary_sha256'])
    else:
        checkpoint.restore(STORE,Path(state['checkpoint']),Path(state['binary']),
                           state['checkpoint_pin'],transaction,authority_sha256=state['binary_sha256'],
                           owner_authorized=owner_authorized)
    receipt=checkpoint.verify(Path(state['checkpoint']),Path(state['binary']),state['checkpoint_pin'],authority_sha256=state['binary_sha256'])
    stable=lambda tree:{name:{key:value for key,value in item.items() if key!='mtime_ns'} for name,item in tree.items()}
    if stable(checkpoint.inventory(STORE))!=stable(receipt['tree']):
        raise RuntimeError('Restored target store bytes/owners/modes differ')
    if bound_sources(state['unit_bundle'])!=state.get('bound_sources',{}):
        raise RuntimeError('Bound wake sources changed')
    apply_bundle(state['unit_bundle'],profile.get('baseline_unit_bundle',profile['old']['unit_bundle']),profile['new']['unit_bundle'],run)


def prepare(root, prefix, old, new, before, incoming_store, persistent, store_manifest, debug_config=None):
    """Build a root-private, durable, fully executable rollback/forward pair.

    Both binary authorities come exclusively from already-verified immutable
    releases, never from a ConfigStore/HTTP field. No active-store writes.
    """
    import copy
    from . import firefly_release as release,voice_profile
    persistent=Path(persistent)
    checkpoint.require_persistent(persistent.parent)
    if persistent.exists():raise RuntimeError('Fresh persistent transition root required')
    persistent.mkdir(mode=0o700)
    old_release=prefix/'releases'/old;new_release=prefix/'releases'/new
    old_doc=json.loads((old_release/'manifest.json').read_text());new_doc=json.loads((new_release/'manifest.json').read_text())
    for doc in [old_doc,new_doc]:
        if doc.get('operational_voice') is not None and doc['operational_voice']!=voice_profile.metadata():
            raise RuntimeError('Unknown operational profile')
    if new_doc.get('operational_voice') is None:
        raise RuntimeError('This forward profile requires the approved operational target')
    debug=debug_config
    legacy_unit=release.render_unit(str(prefix),old_doc.get('operational_voice') is not None)
    old_bundle=capture_legacy(legacy_unit,old,debug)
    if unit_argv(legacy_unit)!=before['argv']:raise RuntimeError('Legacy argv differs from exact unit')
    before['unit_bundle']=old_bundle
    before['bound_sources']=bound_sources(old_bundle)
    # A recovery must not depend on a /run debug session surviving reboot.
    durable_old={path:item for path,item in old_bundle.items() if not path.startswith('/run/')}
    profile={'baseline_unit_bundle':old_bundle}
    for side,rid,source in [('old',old,STORE),('new',new,Path(incoming_store))]:
        binary=prefix/'releases'/rid/'bin/firefly-config'
        digest=hashlib.sha256(binary.read_bytes()).hexdigest()
        point=persistent/(side+'-checkpoint')
        pin=checkpoint.create(source,point,binary,authority_sha256=digest)
        receipt=checkpoint.verify(point,binary,pin,authority_sha256=digest)
        unit=durable_old if side=='old' else {str(UNIT):dict(text=release.render_unit(str(prefix),True),
            sha256=hashlib.sha256(release.render_unit(str(prefix),True).encode()).hexdigest())}
        expected=copy.deepcopy(before)
        audit=receipt['proof']['current']
        marker=point/'store/last_known_good.json'
        if (not marker.is_file() or json.loads(marker.read_text())!={'revision':audit['revision'],'hash':audit['content_sha256']}):
            raise RuntimeError('Ready source with current LKG required before transition')
        for field in ['active','stored']:
            expected['status'][field]['revision']=audit['revision']
            expected['status'][field]['hash']=audit['content_sha256']
        expected['unit_sha256']=unit[str(UNIT)]['sha256'];expected['unit_bundle']=unit
        expected['bound_sources']=bound_sources(unit)
        expected['argv']=unit_argv(unit[str(UNIT)]['text'])
        expected['store_manifest']=store_manifest(point/'store')
        profile[side]=dict(checkpoint=str(point),checkpoint_pin=pin,binary=str(binary),binary_sha256=digest,
                           transaction=str(STORE.parent/('.'+root.name+'-'+side)),
                           unit_bundle=unit,before=expected,bound_sources=expected['bound_sources'])
    old_state=profile['old']
    old_manifest=json.loads((old_release/'manifest.json').read_text())
    capsule=dict(format=2,authority='immutable-operational-release',release=old,
                 checkpoint_pin=old_state['checkpoint_pin'],config_binary_sha256=old_state['binary_sha256'],
                 binary_sha256=checkpoint.digest(old_release/'bin/firefly-demo'),
                 identity=release.build_identity(old_release/'bin/firefly-demo'),
                 manifest_sha256=checkpoint.digest(old_release/'manifest.json'),
                 artifact_modes={item['path']:(old_release/item['path']).stat().st_mode&0o7777 for item in old_manifest['artifacts']},
                 before=old_state['before'])
    checkpoint.validate_release_capsule(capsule,checkpoint.control(Path(old_state['checkpoint'])/'checkpoint.json'),
                                         old_state['checkpoint_pin'],old_release)
    profile['rollback_capsule']=capsule
    profile['persistent_root']=str(persistent)
    return profile
