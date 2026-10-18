"""Whole-store M19 checkpoints for the existing supervised release procedure.

Never stop a service or switch a release. Restore requires an already quiescent
store and atomically exchanges whole directories, retaining the newer tree.
"""
import contextlib
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import stat
import subprocess
import tempfile


class CheckpointError(RuntimeError):
    pass


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pin(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def safe_path(path, owner=None):
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts:
        raise CheckpointError('Absolute canonical paths required')
    for part in [*reversed(path.parents), path]:
        if part.is_symlink():
            raise CheckpointError('Symlink path rejected: ' + str(part))
        if part.exists():
            info = part.stat()
            if info.st_uid not in (0, os.geteuid(), owner) or (info.st_mode & 0o022 and
                    not (part == Path('/tmp') and info.st_mode & stat.S_ISVTX)):
                raise CheckpointError('Untrusted path: ' + str(part))
    return path


def control(path):
    safe_path(path)
    s = path.lstat()
    if not stat.S_ISREG(s.st_mode) or s.st_nlink != 1 or s.st_uid != os.geteuid() or stat.S_IMODE(s.st_mode) != 0o600:
        raise CheckpointError('Untrusted checkpoint control file')
    return json.loads(path.read_bytes())


def inventory(tree):
    """Exact coverage, bytes, owner, modes and mtime, including lock and metadata."""
    safe_path(tree, owner=tree.lstat().st_uid)
    owner = tree.stat().st_uid
    result = {}
    for path in [tree, *sorted(tree.rglob('*'))]:
        s = path.lstat()
        if (not (stat.S_ISDIR(s.st_mode) or stat.S_ISREG(s.st_mode)) or
                s.st_uid != owner or s.st_mode & 0o7022 or
                (stat.S_ISREG(s.st_mode) and s.st_nlink != 1)):
            raise CheckpointError('Unsafe store entry: ' + str(path))
        e = dict(mode=stat.S_IMODE(s.st_mode), uid=s.st_uid, gid=s.st_gid,
                 mtime_ns=s.st_mtime_ns, kind='dir' if path.is_dir() else 'file')
        if e['kind'] == 'file':
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            try:
                if os.fstat(fd) != s:
                    raise CheckpointError('Store entry changed during inspection')
                with os.fdopen(fd, 'rb', closefd=False) as f:
                    e.update(bytes=s.st_size, sha256=hashlib.sha256(f.read()).hexdigest())
            finally:
                os.close(fd)
        result[str(path.relative_to(tree))] = e
    return result


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def durable(path, value):
    data = (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    sync_dir(path.parent)


def clone(source, destination):
    before = inventory(source)
    def copy_file(src, dst):
        descriptor = os.open(src, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise CheckpointError('Unsafe file while copying checkpoint')
            with os.fdopen(descriptor, 'rb', closefd=False) as reader, open(dst, 'xb') as writer:
                shutil.copyfileobj(reader, writer)
            shutil.copystat(src, dst, follow_symlinks=False)
        finally:
            os.close(descriptor)
        return dst
    shutil.copytree(source, destination, symlinks=True, copy_function=copy_file)
    for name, entry in before.items():
        path = destination / name
        if os.geteuid() == 0:
            os.chown(path, entry['uid'], entry['gid'])
        if entry['kind'] == 'file':
            with path.open('rb') as f:
                os.fsync(f.fileno())
    for path in [*sorted((p for p in destination.rglob('*') if p.is_dir()), reverse=True), destination]:
        sync_dir(path)
    if inventory(source) != before or inventory(destination) != before:
        raise CheckpointError('Store changed while copying')
    return before


@contextlib.contextmanager
def locked(store):
    inventory(store)
    # Never create/repair anything in a source store.
    fd = os.open(store / '.lock', os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def invoke(binary, store, *args):
    owner = store.stat().st_uid
    argv = [str(binary), *map(str, args)]
    if owner != os.geteuid():
        if os.geteuid() != 0:
            raise CheckpointError('Wrong checkpoint owner')
        argv = ['runuser', '-u', pwd.getpwuid(owner).pw_name, '--', *argv]
    env = {k: v for k, v in os.environ.items() if k not in
           ('FIREFLY_OWNER_ACCEPTED', 'LD_PRELOAD', 'LD_LIBRARY_PATH')}
    p = subprocess.run(argv, capture_output=True, text=True, timeout=60, env=env)
    if p.returncode:
        raise CheckpointError('M19 compatibility operation failed: ' + str(args[0]) + ': ' + p.stderr)
    return json.loads(p.stdout)


def prove_m19(tree, binary):
    """Run real old tool on a disposable clone; never mutate checkpoint/source."""
    before = inventory(tree)
    if '.pending.json' in before:
        raise CheckpointError('Pending transaction: reconcile explicitly before checkpoint')
    with tempfile.TemporaryDirectory(prefix='.m19-proof-') as tmp:
        parent = Path(tmp)
        owner = tree.stat()
        if os.geteuid() == 0:
            os.chown(parent, owner.st_uid, owner.st_gid)
        copy = parent / 'store'
        clone(tree, copy)
        call = lambda *a: invoke(binary, copy, *a)
        current = call('audit', '--store', copy)
        call('current', '--store', copy)
        call('reconcile', '--store', copy)
        call('revisions', '--store', copy)
        catalogs = []
        for name in before:
            if name == 'configuration.json' or name == 'last_known_good_snapshot.json' or name.startswith('backups/') and before[name]['kind'] == 'file':
                catalogs.append(call('inspect', '--file', copy / name))
        if 'last_known_good.json' in before:
            marker = json.loads((copy / 'last_known_good.json').read_bytes())
            if (set(marker) != {'revision', 'hash'} or not any(
                    x['revision'] == marker['revision'] and x['content_sha256'] == marker['hash'] for x in catalogs)):
                raise CheckpointError('Unrecoverable LKG marker')
        saved = call('save', '--store', copy, '--file', copy / 'configuration.json',
                     '--expected-revision', current['revision'], '--expected-hash', current['content_sha256'])
        call('revisions', '--store', copy)
        call('restore', '--store', copy, '--revision', current['revision'],
             '--expected-revision', saved['revision'], '--expected-hash', saved['content_sha256'])
        call('audit', '--store', copy)
        call('revisions', '--store', copy)
    if inventory(tree) != before:
        raise CheckpointError('Proof changed source checkpoint')
    return dict(binary_sha256=digest(binary), current=current, history_cas_restore=True, lkg_verified=True)


def verify(checkpoint, binary, expected_pin):
    safe_path(checkpoint)
    if checkpoint.stat().st_mode & 0o077:
        raise CheckpointError('Checkpoint must be private')
    if {p.name for p in checkpoint.iterdir()} != {'store', 'checkpoint.json'}:
        raise CheckpointError('Incomplete/unexpected checkpoint entries')
    receipt = control(checkpoint / 'checkpoint.json')
    if (pin(receipt) != expected_pin or receipt.get('format') != 1 or
            receipt['proof']['binary_sha256'] != digest(binary) or
            inventory(checkpoint / 'store') != receipt['tree']):
        raise CheckpointError('Checkpoint identity/coverage/metadata mismatch')
    prove_m19(checkpoint / 'store', binary)
    return receipt


def create(store, destination, binary):
    inventory(store); safe_path(destination); safe_path(binary)
    if destination.exists() or destination.is_relative_to(store) or store.is_relative_to(destination):
        raise CheckpointError('Checkpoint must be NEW and separate from store')
    with locked(store), tempfile.TemporaryDirectory(prefix='.checkpoint-', dir=destination.parent) as tmp:
        stage = Path(tmp) / 'checkpoint'
        stage.mkdir(mode=0o700)
        tree = clone(store, stage / 'store')
        proof = prove_m19(stage / 'store', binary)
        receipt = dict(format=1, tree=tree, proof=proof)
        durable(stage / 'checkpoint.json', receipt)
        sync_dir(stage)
        if inventory(store) != tree:
            raise CheckpointError('Store changed before checkpoint publication')
        os.rename(stage, destination)
        sync_dir(destination.parent)
    return pin(receipt)


def quiescent(store):
    """No implicit service actions. Production needs explicit owner AND stopped unit."""
    live = Path('/var/lib/firefly/store')
    if store == live or store.is_relative_to(Path('/var/lib/firefly')):
        if store != live or os.geteuid() != 0 or os.environ.get('FIREFLY_OWNER_ACCEPTED') != '1':
            raise CheckpointError('Production restore requires owner authorization and root')
        p = subprocess.run(['systemctl', 'show', 'firefly-voice-assistant.service',
                            '-p', 'ActiveState', '-p', 'MainPID', '-p', 'ControlGroup'],
                           check=True, capture_output=True, text=True, timeout=10)
        fields = dict(line.split('=', 1) for line in p.stdout.splitlines())
        if fields.get('ActiveState') != 'inactive' or fields.get('MainPID') != '0':
            raise CheckpointError('Voice service must already be inactive')
        group = fields.get('ControlGroup')
        if group:
            cgroup = Path('/sys/fs/cgroup') / group.lstrip('/')
            for path in cgroup.rglob('cgroup.procs'):
                if path.read_text().strip():
                    raise CheckpointError('Voice cgroup is not empty')
    inventory(store)


def restore(store, checkpoint, binary, expected_pin, transaction, fault=lambda _: None):
    """Durable READY journal + Linux RENAME_EXCHANGE; interrupted retry is idempotent.

    No individual store file is overwritten/deleted. The exchanged newer tree
    stays in transaction/preserved-store. Do not start service before VERIFIED.
    """
    safe_path(transaction); quiescent(store)
    if transaction.parent != store.parent or transaction == store:
        raise CheckpointError('Transaction must be a NEW sibling on the same filesystem')
    receipt = verify(checkpoint, binary, expected_pin)
    with locked(store):
        if not transaction.exists():
            transaction.mkdir(mode=0o700)
            sync_dir(transaction.parent)
            plan = dict(format=1, store=str(store), checkpoint=str(checkpoint), checkpoint_pin=expected_pin,
                        previous=inventory(store), restored=receipt['tree'])
            durable(transaction / 'plan.json', plan)
        else:
            if transaction.stat().st_mode & 0o077:
                raise CheckpointError('Transaction must remain private')
            plan = control(transaction / 'plan.json')
            if (plan['store'] != str(store) or plan['checkpoint'] != str(checkpoint) or
                    plan['checkpoint_pin'] != expected_pin or plan['restored'] != receipt['tree']):
                raise CheckpointError('Transaction identity mismatch')
        ready = transaction / 'restored-store'
        archived = transaction / 'preserved-store'
        if (transaction / 'VERIFIED.json').exists():
            if (control(transaction / 'VERIFIED.json') != dict(checkpoint_pin=expected_pin, previous_pin=pin(plan['previous']))
                    or inventory(archived) != plan['previous']):
                raise CheckpointError('Preserved newer store changed')
            prove_m19(store, binary)
            return dict(result='RESTORED_VERIFIED', preserved=str(archived), retry=True)
        now = inventory(store)
        if now == plan['previous']:
            if archived.exists():
                raise CheckpointError('Ambiguous transaction; no exchange allowed')
            if not ready.exists():
                clone(checkpoint / 'store', ready)
                sync_dir(transaction)
            if inventory(ready) != plan['restored']:
                raise CheckpointError('Incomplete restore staging; retain it and use a NEW transaction')
            prove_m19(ready, binary)
            quiescent(store)
            fault('before_exchange')
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.renameat2(-100, os.fsencode(store), -100, os.fsencode(ready), 2) != 0:
                raise OSError(ctypes.get_errno(), 'Atomic ConfigStore exchange failed; no fallback')
            fault('after_exchange')
            sync_dir(store.parent); sync_dir(transaction)
            fault('after_sync')
        elif now != plan['restored']:
            raise CheckpointError('Store changed outside transaction; no exchange allowed')
        # Also required on a retry after a crash immediately after exchange.
        sync_dir(store.parent); sync_dir(transaction)
        if not archived.exists():
            if inventory(ready) != plan['previous']:
                raise CheckpointError('Preserved newer tree mismatch')
            os.rename(ready, archived)
            sync_dir(transaction)
        if inventory(archived) != plan['previous'] or inventory(store) != plan['restored']:
            raise CheckpointError('Exchanged tree verification failed; keep service stopped')
        prove_m19(store, binary)
        fault('before_receipt')
        durable(transaction / 'VERIFIED.json', dict(checkpoint_pin=expected_pin, previous_pin=pin(plan['previous'])))
        return dict(result='RESTORED_VERIFIED', preserved=str(archived), retry=False)
