#!/usr/bin/python3
"""Prepare a fresh recovery bundle; run one separately owner-authorized release."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.request


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def command(root, argv, timeout=60, *, owner_authorized=False):
    """Preserve the response before judging a mandatory preparation command."""
    env = {**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'}
    # Offline checks must not inherit live-root permission from the operator.
    env.pop('FIREFLY_OWNER_ACCEPTED', None)
    if owner_authorized:
        env['FIREFLY_OWNER_ACCEPTED'] = '1'
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                                env=env)
        record = dict(argv=argv, returncode=result.returncode,
                      stdout=result.stdout, stderr=result.stderr)
    except subprocess.TimeoutExpired as error:
        record = dict(argv=argv, timeout=timeout, stdout=repr(error.stdout), stderr=repr(error.stderr))
        with (root / 'preparation-commands.jsonl').open('a') as file:
            file.write(json.dumps(record) + '\n')
        raise
    with (root / 'preparation-commands.jsonl').open('a') as file:
        file.write(json.dumps(record) + '\n')
    if result.returncode:
        raise RuntimeError('Preparation command failed: ' + repr(argv))
    return result.stdout.strip()


def write(path, value):
    partial = path.with_name(path.name + '.partial')
    with partial.open('x') as file:
        file.write(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        file.flush()
        os.fsync(file.fileno())
    partial.chmod(0o600)
    partial.replace(path)


def prepare_files(root, source):
    """Copy only tracked operator tools/tests; never reuse a consumed directory."""
    root.mkdir(mode=0o700)
    (root / 'evidence').mkdir(mode=0o700)
    files = ['scripts/release-runner.py', 'scripts/deployment-launcher.py',
             'scripts/production-rollout.py', 'packaging/__init__.py',
             'packaging/firefly_recovery.py', 'packaging/firefly_checkpoint.py',
             'packaging/operational_transition.py', 'packaging/voice_profile.py',
             'packaging/firefly_release.py', 'packaging/mqtt_release.py', 'tests/DeploymentLauncherTests.py',
             'tests/ReleaseRecoveryTests.py', 'tests/ReleaseRunnerTests.py',
             'tests/DeploymentLauncherSystemdTests.py']
    for name in files:
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        shutil.copyfile(source / name, target)
        target.chmod(0o600)
    for name in ['scripts/stage-release.py', 'packaging/__init__.py', 'packaging/firefly_release.py',
                 'packaging/mqtt_release.py', 'packaging/firefly_checkpoint.py',
                 'packaging/voice_profile.py', 'packaging/operational_transition.py', 'scripts/config-checkpoint.py']:
        target = root / 'tools' / name
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        shutil.copyfile(source / name, target)
        target.chmod(0o600)


def mandatory_fixtures(root):
    # Fixed, reviewed list. No ephemeral NRestarts assertion or extra live gates.
    for name in ['DeploymentLauncherTests', 'ReleaseRecoveryTests', 'ReleaseRunnerTests']:
        command(root, ['/usr/bin/python3', str(root / 'tests' / (name + '.py'))])
    command(root, ['/usr/bin/python3', str(root / 'tests/DeploymentLauncherSystemdTests.py'),
                   str(root / 'mandatory-systemd-fixtures')])


def selected_build(source, build_dir=None):
    if build_dir is None:
        return source / 'build'
    cache = build_dir / 'CMakeCache.txt'
    if (not build_dir.is_absolute() or build_dir.resolve() != build_dir or
            cache.is_symlink() or not cache.is_file() or cache.stat().st_nlink != 1):
        raise RuntimeError('Explicit build directory requires a canonical regular CMake cache')
    match = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$', cache.read_text(), re.M)
    if not match or match[1] != str(source):
        raise RuntimeError('Build directory must belong to the exact approved source')
    return build_dir


def application_checks(root, source, build_dir=None):
    # Existing full offline application checks run as the repository/service user,
    # never as privileged production mutation tools. No dependency downloads.
    # Serial checks avoid competing package-copy memory and 100 ms deadline
    # positive controls on the 8 GB appliance; the tested deadlines stay exact.
    command(root, ['runuser', '-u', 'firefly', '--', 'ctest', '--test-dir', str(selected_build(source, build_dir)),
                   '--output-on-failure', '--no-tests=error', '-j1'], timeout=1200)
    for name in ['typecheck', 'test', 'build']:
        command(root, ['runuser', '-u', 'firefly', '--', 'npm', '--prefix',
                       str(source / 'src/admin/frontend'), 'run', name], timeout=120)


def approved_assets(candidate, previous, approved, built_assets):
    if approved is None:
        if candidate != previous:
            raise RuntimeError('Candidate assets mismatch; an explicit reviewed SHA256 is required')
        return previous
    if (len(approved) != 64 or any(c not in '0123456789abcdef' for c in approved)
            or candidate != approved or built_assets != approved):
        raise RuntimeError('Candidate assets mismatch with approved source/build SHA256')
    return approved


def preservation_contract(old, new, old_release, new_release):
    """Frozen properties/modes; explicit MQTT additions and one pinned wake revision.

    Legacy M19 manifests lack mode metadata. In that case the previously verified
    immutable release is the mode authority, never an inferred filename policy.
    Both trees have already passed ordinary production verification.
    """
    def protected(document):
        result = {}
        for entry in document['artifacts']:
            name = entry['path']
            if name.startswith(('lib/', 'share/firefly/wake/', 'share/firefly/config/',
                                'share/firefly/systemd/')):
                if name in result:
                    raise RuntimeError('Frozen contract changed: duplicate path')
                result[name] = entry
        return result
    before, after = protected(old), protected(new)
    old_wake = old.get('dependencies', {})
    new_wake = new.get('dependencies', {})
    wake_upgrade = {}
    if ((old_wake.get('amina_wake'), old_wake.get('amina_sha256')) ==
            ('v3', 'bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1') and
            (new_wake.get('amina_wake'), new_wake.get('amina_sha256')) ==
            ('v3-amila-20261029', '24000c0e3f9fd3249f56e5f836719aba8f7a3f23c1f4a2c571bdf0bf6cc8a1dd')):
        # Only this exact model/worker/metadata revision may change together.
        # These pins are cross-checked against the ordinary package verifier.
        wake_upgrade = {
            'share/firefly/wake/amina-v3.tflite': (
                ('bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1', 60912),
                ('24000c0e3f9fd3249f56e5f836719aba8f7a3f23c1f4a2c571bdf0bf6cc8a1dd', 61080)),
            'share/firefly/wake/amina_worker.py': (
                ('74ebee6c765c205131086891fda9b47d802959a688337c3045d7acc2c7346b63', 5732),
                ('e9db1780ce3f560b04b4c1b94db6b4e018680d3582d8266ceed76cd14a45ffff', 5732)),
            'share/firefly/wake/manifest.json': (
                ('7c097b8d1b7b6bf9735fd2df11aa4117f1c8916cfe563052330511ace6b416ed', 394),
                ('1873f5e3afd51678f5f81f28aeae67494daed0abd5fda98b0306daf78271bde1', 517)),
        }
        if not wake_upgrade.keys() <= before.keys() or not wake_upgrade.keys() <= after.keys():
            raise RuntimeError('Frozen contract changed: incomplete wake revision')
    additions = set(after) - set(before)
    permitted = {'lib/libmosquitto.so.1', 'lib/libssl.so.3', 'lib/libcrypto.so.3'}
    mqtt_transition = not old.get('mqtt', {}).get('enabled') and new.get('mqtt', {}).get('enabled') is True
    if (set(before) - set(after) or additions != (permitted if mqtt_transition else set())
            or old['shared_models'] != new['shared_models']):
        raise RuntimeError('Voice/libraries/unit/seed/shared-model contract changed')
    voice_upgrade = (old.get("operational_voice") != new.get("operational_voice"))
    if voice_upgrade:
        from packaging.voice_profile import metadata
        if old.get("operational_voice") is not None or new.get("operational_voice") != metadata():
            raise RuntimeError("Unsupported operational profile transition")
    for name, entry in after.items():
        # The ordinary verifier also validates paths, bytes, references and owners.
        # Preserve every legacy property, accepting only additive mode metadata.
        mode = (new_release / name).stat().st_mode & 0o7777
        if mode not in (0o644, 0o755) or ('mode' in entry and
                (type(entry['mode']) is not int or entry['mode'] != mode)):
            raise RuntimeError('Frozen contract changed: unsafe/mismatched mode: ' + name)
        if new.get('mqtt', {}).get('enabled') is True and 'mode' not in entry:
            raise RuntimeError('Frozen contract changed: missing MQTT artifact mode: ' + name)
        if name in additions:
            if mode != 0o755:
                raise RuntimeError('Frozen contract changed: MQTT library mode: ' + name)
            continue
        previous = before[name]
        if voice_upgrade and name == 'share/firefly/systemd/firefly-voice-assistant.service':
            from packaging.firefly_release import render_unit, PRODUCTION_PREFIX
            if ((old_release/name).read_text()!=render_unit(PRODUCTION_PREFIX) or
                    (new_release/name).read_text()!=render_unit(PRODUCTION_PREFIX, True)):
                raise RuntimeError('Unapproved operational unit bytes')
            if ({k:v for k,v in previous.items() if k not in ('mode','sha256','bytes')} !=
                    {k:v for k,v in entry.items() if k not in ('mode','sha256','bytes')}):
                raise RuntimeError('Operational unit properties changed')
            if (old_release/name).stat().st_mode & 0o7777 != mode:
                raise RuntimeError('Operational unit mode changed')
            continue
        previous_mode = (old_release / name).stat().st_mode & 0o7777
        if name in wake_upgrade:
            old_pin, new_pin = wake_upgrade[name]
            for release, item, pin in [(old_release, previous, old_pin), (new_release, entry, new_pin)]:
                path = release / name
                if ((item.get('sha256'), item.get('bytes')) != pin or path.stat().st_size != pin[1]
                        or hashlib.sha256(path.read_bytes()).hexdigest() != pin[0]):
                    raise RuntimeError('Frozen contract changed: unapproved wake bytes: ' + name)
            if (previous_mode != mode or ('mode' in previous and
                    (type(previous['mode']) is not int or previous['mode'] != previous_mode))
                    or {k: v for k, v in previous.items() if k not in ('mode', 'sha256', 'bytes')} !=
                       {k: v for k, v in entry.items() if k not in ('mode', 'sha256', 'bytes')}):
                raise RuntimeError('Frozen contract changed: wake properties: ' + name)
            continue
        if (previous_mode != mode or ('mode' in previous and
                (type(previous['mode']) is not int or previous['mode'] != previous_mode))
                or {k: v for k, v in previous.items() if k != 'mode'} !=
                   {k: v for k, v in entry.items() if k != 'mode'}):
            raise RuntimeError('Voice/libraries/unit/seed/shared-model contract changed: ' + name)


def approved_source(root, source, commit):
    if (root.parent != Path('/run') or root.exists() or source.resolve() != source
            or subprocess.check_output(['runuser', '-u', 'firefly', '--', 'git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() != commit
            or subprocess.check_output(['runuser', '-u', 'firefly', '--', 'git', '-C', str(source), 'status', '--porcelain']).strip()):
        raise RuntimeError('Fresh /run directory and exact clean approved source are required')
    reviewed = subprocess.check_output(['runuser', '-u', 'firefly', '--', 'git', '-C', str(source), 'show', commit + ':scripts/release-runner.py'])
    if Path(__file__).read_bytes() != reviewed:
        raise RuntimeError('Runner is not the tracked approved version')


def prepare(root, source, commit, release, approved_admin_assets_sha256=None,
            m19_checkpoint=None, m19_checkpoint_pin=None, m19_config=None,
            m19_recovery_capsule=None, m19_recovery_capsule_pin=None, build_dir=None, operational_store=None):
    approved_source(root, source, commit)
    build = selected_build(source, build_dir)
    prepare_files(root, source)
    application_checks(root, source, build_dir)
    m = load(root / 'scripts/production-rollout.py')
    package = load(root / 'tools/packaging/firefly_release.py')
    prefix, store = Path('/opt/firefly/voice-assistant'), Path('/var/lib/firefly/store')
    old = Path(os.readlink(prefix / 'current')).name
    for target in [old, release]:
        command(root, ['/usr/bin/python3', str(root / 'tools/scripts/stage-release.py'),
                       'verify', '--destination', str(prefix), '--release-id', target],
                timeout=600, owner_authorized=True)
    identities = {target: package.build_identity(prefix / 'releases' / target / 'bin/firefly-demo')
                  for target in [old, release]}
    if (old == release or identities[release]['head'] != commit or identities[release]['dirty']
            or identities[release]['source_sha256'] != package.source_tree_fingerprint(source)):
        raise RuntimeError('Candidate identity/source/assets mismatch')
    assets_pin = approved_assets(identities[release]['admin_assets_sha256'],
                                 identities[old]['admin_assets_sha256'], approved_admin_assets_sha256,
                                 package.admin_assets_fingerprint(build) if approved_admin_assets_sha256 else None)
    documents = {target: json.loads((prefix / 'releases' / target / 'manifest.json').read_bytes())
                 for target in [old, release]}
    preservation_contract(documents[old], documents[release],
                          prefix / 'releases' / old, prefix / 'releases' / release)
    checkpoint = None
    capsule = None
    create_capsule = False
    if documents[release].get('mqtt', {}).get('enabled') is True:
        if not (m19_checkpoint and m19_checkpoint_pin and m19_config and m19_recovery_capsule):
            raise RuntimeError('MQTT preparation requires a complete verified M19 checkpoint')
        if any(Path('/run') in p.parents for p in (m19_checkpoint, m19_recovery_capsule)):
            raise RuntimeError('M19 checkpoint must persist across reboot, outside /run')
        helper = load(root / 'tools/packaging/firefly_checkpoint.py')
        helper.require_persistent(m19_checkpoint)
        helper.require_persistent(m19_recovery_capsule)
        receipt = helper.verify(m19_checkpoint, m19_config, m19_checkpoint_pin)
        if not documents[old].get('mqtt', {}).get('enabled') and helper.inventory(store) != receipt['tree']:
            raise RuntimeError('M19 store changed since checkpoint; prepare a fresh checkpoint')
        helper.safe_path(m19_recovery_capsule)
        create_capsule = not documents[old].get('mqtt', {}).get('enabled')
        if create_capsule:
            if m19_recovery_capsule.exists():
                raise RuntimeError('A NEW durable M19 readiness capsule is required')
            if m19_config != prefix / 'releases' / old / 'bin/firefly-config':
                raise RuntimeError('Initial checkpoint authority must be the current original M19')
        else:
            capsule = helper.control(m19_recovery_capsule)
            if helper.pin(capsule) != m19_recovery_capsule_pin:
                raise RuntimeError('M19 readiness capsule pin mismatch')
            helper.validate_capsule(capsule, receipt, m19_checkpoint_pin,
                                    prefix / 'releases' / capsule['release'])
            command(root, ['/usr/bin/python3', str(root / 'tools/scripts/stage-release.py'),
                           'verify', '--destination', str(prefix), '--release-id', capsule['release']],
                    timeout=600, owner_authorized=True)
        checkpoint = dict(path=str(m19_checkpoint), pin=m19_checkpoint_pin,
                          m19_config=str(m19_config), binary_sha256=receipt['proof']['binary_sha256'],
                          recovery_capsule=str(m19_recovery_capsule), capsule_pin=m19_recovery_capsule_pin,
                          helper_sha256=m.sha(root / 'tools/packaging/firefly_checkpoint.py'))
    command(root, ['cp', '-a', str(store), str(root / 'backup-store')])
    with urllib.request.urlopen('http://127.0.0.1:8080/api/status', timeout=3) as response:
        status = json.load(response)
    before = dict(status=status, argv=Path('/proc/' + str(status['worker_pid']) + '/cmdline').read_bytes().decode().split('\0'),
                  unit_sha256=m.sha('/etc/systemd/system/firefly-voice-assistant.service'), store_manifest=m.manifest(store))
    if create_capsule:
        if helper.inventory(store) != receipt['tree']:
            raise RuntimeError('M19 store changed since checkpoint')
        capsule = dict(format=1, checkpoint_pin=m19_checkpoint_pin, release=old,
                       identity=identities[old], binary_sha256=m.sha(prefix / 'releases' / old / 'bin/firefly-demo'),
                       artifact_modes={a['path']: (prefix / 'releases' / old / a['path']).stat().st_mode & 0o7777
                                       for a in documents[old]['artifacts']},
                       manifest_sha256=m.sha(prefix / 'releases' / old / 'manifest.json'), before=before)
        helper.validate_capsule(capsule, receipt, m19_checkpoint_pin, prefix / 'releases' / old)
        checkpoint['capsule_pin'] = helper.pin(capsule)
    transition = None
    if operational_store is not None:
        from packaging.operational_transition import prepare as prepare_transition
        debug_path = Path('/run/firefly-wake-debug/session.json')
        debug = json.loads(debug_path.read_text()) if debug_path.exists() else None
        transition = prepare_transition(root, prefix, old, release, before, operational_store,
                       Path('/userdata') / (root.name + '-state'), m.manifest, debug)
    elif documents[release].get('operational_voice') is not None:
        raise RuntimeError('Operational profile requires explicit whole-store migration source')
    write(root / 'production-before.json', before)
    now = time.monotonic()
    plan = dict(old=old, new=release, backup_store=str(root / 'backup-store'), identities=identities,
                m19_checkpoint=checkpoint,
                approved_admin_assets_sha256=assets_pin,
                binary_sha256={target: m.sha(prefix / 'releases' / target / 'bin/firefly-demo') for target in [old, release]},
                worker_unit=root.name + '-forward.service', rollback_unit=root.name + '-rollback.service',
                rollback_timer=root.name + '-rollback.timer', verification_budget_seconds=240, recovery_budget_seconds=360,
                boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                armed_monotonic=now, deadline_monotonic=now + 240,
                controller_sha256=m.sha(root / 'scripts/production-rollout.py'),
                recovery_helper_sha256=m.sha(root / 'packaging/firefly_recovery.py'),
                launcher_sha256=m.sha(root / 'scripts/deployment-launcher.py'),
                runner_sha256=m.sha(root / 'scripts/release-runner.py'))
    if transition is not None:
        plan['operational_transition'] = transition
        names = m.OPERATIONAL_DEPENDENCIES
        plan['operational_tools_sha256'] = {name:m.sha(root/name) for name in names}
    # This provisional, unarmed plan cannot pass supervision_gate. The one real
    # boot-bound deadline is frozen from the actual timer before any forward launch.
    write(root / 'plan.json', plan)
    mandatory_fixtures(root)
    ops = m.Production(root)
    ops.operation_deadline = time.monotonic() + 30  # Read-only preparation, timer not armed.
    ops.record('prepared-gates', ops.gates(old))
    if transition is not None:
        from packaging import firefly_checkpoint as transition_helper
        persistent = Path(transition['persistent_root'])
        transition_helper.durable(persistent/'old-readiness.json', transition['rollback_capsule'])
        transition_helper.durable(persistent/'transition.json', transition)
    if create_capsule:
        if helper.inventory(store) != receipt['tree']:
            raise RuntimeError('M19 store changed before capsule publication')
        helper.durable(m19_recovery_capsule, capsule)
    write(root / 'PREPARED', {'plan_sha256': m.sha(root / 'plan.json'), 'commit': commit})


def prepare_rollback(root, source, commit, checkpoint_path, checkpoint_pin,
                     m19_config, capsule_path, capsule_pin, build_dir=None, operational=False):
    """Fresh supervisor from durable M19 metadata; never reuse an old lease/START."""
    approved_source(root, source, commit)
    selected_build(source, build_dir)
    prepare_files(root, source)
    application_checks(root, source, build_dir)
    m = load(root / 'scripts/production-rollout.py')
    package = load(root / 'tools/packaging/firefly_release.py')
    helper = load(root / 'tools/packaging/firefly_checkpoint.py')
    helper.require_persistent(checkpoint_path)
    helper.require_persistent(capsule_path)
    capsule = helper.control(capsule_path)
    if helper.pin(capsule) != capsule_pin:
        raise RuntimeError('Readiness capsule pin mismatch')
    receipt = (helper.verify(checkpoint_path, m19_config, checkpoint_pin,
                             authority_sha256=capsule['config_binary_sha256']) if operational else
               helper.verify(checkpoint_path, m19_config, checkpoint_pin))
    prefix, store = Path('/opt/firefly/voice-assistant'), Path('/var/lib/firefly/store')
    target = capsule['release']
    validator = helper.validate_release_capsule if operational else helper.validate_capsule
    validator(capsule, receipt, checkpoint_pin, prefix / 'releases' / target)
    if (m19_config != prefix / 'releases' / target / 'bin/firefly-config' or
            helper.inventory(store) != receipt['tree']):
        raise RuntimeError('Restore the complete M19 checkpoint before bootstrap preparation')
    command(root, ['/usr/bin/python3', str(root / 'tools/scripts/stage-release.py'),
                   'verify', '--destination', str(prefix), '--release-id', target],
            timeout=600, owner_authorized=True)
    # The immutable M19 manifest, complete artifact modes, binaries and original
    # readiness baseline are pinned by the owner-retained capsule. A broken M20
    # image must not be needed to verify this exact old target. No files are removed.
    if package.build_identity(prefix / 'releases' / target / 'bin/firefly-demo') != capsule['identity']:
        raise RuntimeError('Original M19 identity changed')
    source_release = package._current_id(prefix)
    if source_release is None:
        raise RuntimeError('M19 bootstrap requires a recorded current release pointer')
    command(root, ['cp', '-a', str(store), str(root / 'backup-store')])
    before = json.loads(json.dumps(capsule['before']))
    # The original M19 bootstrap always starts a new RuntimeGeneration(id=1).
    # Catalog revision/hash and every other old gate remain pinned.
    before['status']['active']['generation_id'] = 1
    write(root / 'production-before.json', before)
    now = time.monotonic()
    checkpoint = dict(path=str(checkpoint_path), pin=checkpoint_pin, m19_config=str(m19_config),
                      binary_sha256=receipt['proof']['binary_sha256'], recovery_capsule=str(capsule_path),
                      capsule_pin=capsule_pin, helper_sha256=m.sha(root / 'tools/packaging/firefly_checkpoint.py'))
    plan = dict(old=target, new=target, recovery_only=True, bootstrap_source_release=source_release,
                backup_store=str(root / 'backup-store'),
                identities={target: capsule['identity']}, m19_checkpoint=checkpoint,
                binary_sha256={target: capsule['binary_sha256']},
                approved_admin_assets_sha256=capsule['identity']['admin_assets_sha256'],
                worker_unit=root.name + '-forward.service', rollback_unit=root.name + '-rollback.service',
                rollback_timer=root.name + '-rollback.timer', verification_budget_seconds=240, recovery_budget_seconds=360,
                boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                armed_monotonic=now, deadline_monotonic=now + 240,
                controller_sha256=m.sha(root / 'scripts/production-rollout.py'),
                recovery_helper_sha256=m.sha(root / 'packaging/firefly_recovery.py'),
                launcher_sha256=m.sha(root / 'scripts/deployment-launcher.py'),
                runner_sha256=m.sha(root / 'scripts/release-runner.py'))
    if operational:
        plan['release_bootstrap'] = True
        plan['m19_checkpoint']['authority_kind'] = 'immutable-operational-release'
        names = m.OPERATIONAL_DEPENDENCIES
        plan['operational_tools_sha256'] = {name:m.sha(root/name) for name in names}
    write(root / 'plan.json', plan)
    mandatory_fixtures(root)
    ops = m.Production(root)
    ops.operation_deadline = time.monotonic() + 60
    ops.bootstrap_preflight(armed=False)
    write(root / 'PREPARED', {'plan_sha256': m.sha(root / 'plan.json'), 'commit': commit})


def diagnostic(ops):
    # Diagnostic failures are recorded, not treated as extra authorization gates.
    try:
        value = {'output': ops.run(['systemctl', 'show',
            ops.plan['worker_unit'], ops.plan['rollback_unit'], ops.plan['rollback_timer']], timeout=5)}
    except Exception as error:
        value = {'error': repr(error)}
    try:
        ops.record('unit-diagnostics', value)
    except Exception as error:
        value['recording_error'] = repr(error)
    return value


def arm(ops, authorization):
    root = ops.root
    m = load(root / 'scripts/production-rollout.py')
    command(root, ['systemd-run', '--unit=' + ops.plan['rollback_unit'], '--on-active=240s',
        '--timer-property=AccuracySec=250ms', '--property=Type=exec', '--property=Restart=on-failure',
        '--property=RestartSec=1s', '--property=RestartPreventExitStatus=78', '--property=StartLimitIntervalSec=0',
        '--property=TimeoutStartSec=10s', '--property=TimeoutStopSec=10s', '--property=RuntimeMaxSec=360s',
        '/usr/bin/python3', str(root / 'scripts/production-rollout.py'), 'rollback', str(root)], timeout=15)
    trigger = m.property_value(ops.systemd_properties(ops.plan['rollback_timer'], 'Timer'), 'NextElapseUSecMonotonic', 't')
    plan = {**ops.plan, 'armed_monotonic': (trigger - 240000000) / 1000000,
            'deadline_monotonic': trigger / 1000000, 'authorization': authorization}
    write(root / 'plan.json', plan)
    ops = m.Production(root)
    write(root / 'PLAN_FROZEN', {'plan_sha256': ops.plan_sha256, 'utc': m.utc()})
    return ops


def run(root, authorization):
    launcher = load(root / 'scripts/deployment-launcher.py')
    m = launcher.controller
    ops = m.Production(root)
    ops.supervisor_files()
    if (Path(__file__).resolve() != root / 'scripts/release-runner.py'
            or m.sha(Path(__file__)) != ops.plan['runner_sha256']
            or json.loads((root / 'PREPARED').read_bytes())['plan_sha256'] != ops.plan_sha256):
        raise RuntimeError('Runner/prepared bundle changed')
    with (root / 'RUNNER_STARTED').open('x') as claim:
        claim.write(m.utc() + '\n')
    plan = ops.plan
    result = None
    try:
        ops.operation_deadline = time.monotonic() + 30  # Only old read-only gates before arm.
        checkpoint = plan.get('m19_checkpoint')
        if checkpoint:
            helper = load(root / 'tools/packaging/firefly_checkpoint.py')
            capsule = helper.control(Path(checkpoint['recovery_capsule']))
            if helper.pin(capsule) != checkpoint['capsule_pin']:
                raise RuntimeError('M19 readiness capsule changed')
            operational = checkpoint.get('authority_kind') == 'immutable-operational-release'
            receipt = (helper.verify(Path(checkpoint['path']), Path(checkpoint['m19_config']), checkpoint['pin'],
                                     authority_sha256=capsule['config_binary_sha256']) if operational else
                       helper.verify(Path(checkpoint['path']), Path(checkpoint['m19_config']), checkpoint['pin']))
            validator = helper.validate_release_capsule if operational else helper.validate_capsule
            validator(capsule, receipt, checkpoint['pin'], ops.prefix / 'releases' / capsule['release'])
        ops.record('runner-before-arm', ops.bootstrap_preflight(armed=False) if plan.get('recovery_only') else ops.gates(plan['old']))
        ops = arm(ops, authorization)
        plan = ops.plan
        if plan.get('recovery_only'):
            # An armed timer alone cannot authorize a stopped-state start.
            # Freeze this permission only AFTER the existing full typed gate.
            ops.supervision_gate()
            write(root / 'BOOTSTRAP_AUTHORIZED', {'plan_sha256': ops.plan_sha256})
        ops.record('runner-preflight', ops.bootstrap_preflight() if plan.get('recovery_only') else ops.gates(plan['old']))
        ops.supervision_gate()
        launcher.start(ops)  # The existing exclusive claim/asynchronous launcher.
        result = launcher.observe(ops)
        if result['state'] == 'DEPLOYED_VERIFIED':
            independent = m.Production(root).gates(plan['new'])
            ops.record('runner-independent-acceptance', independent)
            result = {'result': 'ROLLED_BACK' if plan.get('recovery_only') else 'DEPLOYED_VERIFIED', 'verification': independent}
        elif result['state'] == 'FORWARD_FAILED' and not (root / 'CUTOVER_STARTED').exists():
            result = {'result': 'ABORTED_BEFORE_CUTOVER', 'verification': ops.gates(plan['old'])}
        else:
            raise RuntimeError('Forward operation did not independently verify: ' + repr(result))
    except Exception as error:
        ops.record('runner-error', {'error': repr(error)})
        if not (root / 'FORWARD_LAUNCH_STARTED').exists() and not (plan.get('recovery_only') and (root / 'BOOTSTRAP_AUTHORIZED').exists()):
            try:
                ops.operation_deadline = time.monotonic() + 30  # No forward submitted; read-only old verification.
                result = {'result': 'ABORTED_BEFORE_CUTOVER', 'verification': ops.gates(plan['old'])}
            except Exception as verification_error:
                result = {'result': 'BLOCKED_UNKNOWN', 'error': repr(verification_error), 'watchdog_preserved': True}
        else:
            # Do not stop/disarm recovery or start another worker. The original
            # independent timer/controller owns restoration, including agent loss.
            ops.operation_deadline = ops.recovery_deadline
            while time.monotonic() < ops.recovery_deadline:
                try:
                    if (root / 'evidence/rollback-result.json').exists():
                        result = {'result': 'ROLLED_BACK', 'verification': ops.gates(plan['old'])}
                        break
                except Exception as recovery_error:
                    ops.record('runner-recovery-observation', {'error': repr(recovery_error)})
                time.sleep(min(.5, max(0, ops.recovery_deadline - time.monotonic())))
            if result is None or 'result' not in result:
                result = {'result': 'BLOCKED_UNKNOWN', 'watchdog_preserved': True}
    result['diagnostics'] = diagnostic(ops)
    if plan.get('recovery_only') and result['result'] == 'ROLLED_BACK':
        result['operation'] = 'M19_BOOTSTRAP_RECOVERY'
    if result['result'] in ('DEPLOYED_VERIFIED', 'ROLLED_BACK', 'ABORTED_BEFORE_CUTOVER'):
        # Disarm only after fresh existing runtime/store/ownership gates passed.
        try:
            ops.run(['systemctl', 'stop', plan['rollback_timer']], timeout=10)
            result['watchdog_disarmed_after_verification'] = True
        except Exception as error:
            result['watchdog_disarmed_after_verification'] = False
            result['watchdog_error'] = repr(error)
    ops.record('runner-result', result)
    return result


def main():
    os.umask(0o077)
    if os.geteuid() != 0 or not __debug__:
        raise RuntimeError('Root and Python assertions are required')
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('--source', type=Path, required=True)
    prep.add_argument('--approved-commit', required=True)
    prep.add_argument('--new-release', required=True)
    prep.add_argument('--root', type=Path, required=True)
    prep.add_argument('--approved-admin-assets-sha256')
    prep.add_argument('--build-dir', type=Path)
    prep.add_argument('--operational-store', type=Path)
    prep.add_argument('--m19-checkpoint', type=Path)
    prep.add_argument('--m19-checkpoint-pin')
    prep.add_argument('--m19-config', type=Path)
    prep.add_argument('--m19-recovery-capsule', type=Path)
    prep.add_argument('--m19-recovery-capsule-pin')
    recovery = sub.add_parser('prepare-rollback')
    recovery.add_argument('--source', type=Path, required=True)
    recovery.add_argument('--approved-commit', required=True)
    recovery.add_argument('--root', type=Path, required=True)
    recovery.add_argument('--m19-checkpoint', '--checkpoint', dest='m19_checkpoint', type=Path, required=True)
    recovery.add_argument('--m19-checkpoint-pin', '--checkpoint-pin', dest='m19_checkpoint_pin', required=True)
    recovery.add_argument('--m19-config', '--config', dest='m19_config', type=Path, required=True)
    recovery.add_argument('--m19-recovery-capsule', '--recovery-capsule', dest='m19_recovery_capsule', type=Path, required=True)
    recovery.add_argument('--m19-recovery-capsule-pin', '--recovery-capsule-pin', dest='m19_recovery_capsule_pin', required=True)
    recovery.add_argument('--build-dir', type=Path)
    recovery.add_argument('--operational', action='store_true')
    execute = sub.add_parser('run')
    execute.add_argument('root', type=Path)
    execute.add_argument('--owner-authorization', required=True)
    args = parser.parse_args()
    if args.mode in ('prepare', 'prepare-rollback'):
        try:
            if args.mode == 'prepare':
                prepare(args.root, args.source, args.approved_commit, args.new_release, args.approved_admin_assets_sha256,
                        args.m19_checkpoint, args.m19_checkpoint_pin, args.m19_config,
                        args.m19_recovery_capsule, args.m19_recovery_capsule_pin, args.build_dir, args.operational_store)
            else:
                prepare_rollback(args.root, args.source, args.approved_commit,
                                 args.m19_checkpoint, args.m19_checkpoint_pin, args.m19_config,
                                 args.m19_recovery_capsule, args.m19_recovery_capsule_pin, args.build_dir, args.operational)
        except Exception as error:
            print(json.dumps({'result': 'ABORTED_BEFORE_CUTOVER', 'phase': 'prepare', 'error': repr(error)}))
            return 1
        plan = json.loads((args.root / 'plan.json').read_bytes())
        print(json.dumps({'result': 'PREPARED', 'production_stopped': False,
                          'm19_recovery_capsule_pin': (plan.get('m19_checkpoint') or {}).get('capsule_pin')}))
        return 0
    try:
        result = run(args.root, args.owner_authorization)
    except Exception as error:
        # No retry, ad-hoc recovery or watchdog disarm after an unhandled failure.
        uncertain = (args.root / 'FORWARD_LAUNCH_STARTED').exists() or (args.root / 'CUTOVER_STARTED').exists()
        result = {'result': 'BLOCKED_UNKNOWN' if uncertain else 'ABORTED_BEFORE_CUTOVER',
                  'error': repr(error), 'watchdog_preserved': True}
    print(json.dumps({'result': result['result']}))
    return 0 if (result['result'] == 'DEPLOYED_VERIFIED' or
                 result['result'] == 'ROLLED_BACK' and result.get('operation') == 'M19_BOOTSTRAP_RECOVERY') else 1


if __name__ == '__main__':
    raise SystemExit(main())
