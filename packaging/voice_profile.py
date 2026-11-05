"""Pinned F2/wake CLI profile for the existing immutable release pipeline.

No download, model inference, audio, service or ConfigStore mutation.
"""
import hashlib
import os
from pathlib import Path
import stat

FILES = {
    'duration_predictor.int8.onnx': (3700147, 'c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db'),
    'text_encoder.int8.onnx': (36416150, 'c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff'),
    'vector_estimator.int8.onnx': (78400833, '20cd86fa5c6effedfda0e7cffe5b0569ca401c440a0c3a1d72bf39286c0db3fd'),
    'vocoder.int8.onnx': (25991073, 'e923d60f53f95eb1ce235f1dc33ec56d9c057823c96fa6f8acf98f32b0da6152'),
    'tts.json': (8253, '42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09'),
    'unicode_indexer.bin': (262144, '8402ca48e5189a8950138580b0fff64db6f072f24ac07cd54ba8b2fbb9883b30'),
    'voice.bin': (517168, '67d5209b0ee8ce6c74105ffbe12fe6a7628aea3b4ba2fcb308a4a67938a93ce8'),
}
PATHS = {'models/tts/' + name for name in FILES}
ARTIFACT = 'share/firefly/deployment/packaging/voice_profile.py'


def metadata():
    return dict(version=1, wake_stt_confirmation=True, voice='F2', language='ru',
                assets=[dict(path='models/tts/'+name, bytes=size, sha256=digest)
                        for name, (size,digest) in FILES.items()])


def verify_models(directory):
    directory = Path(directory)
    if not directory.is_absolute() or directory.resolve() != directory:
        raise ValueError('F2 directory must be canonical')
    for path in [*reversed(directory.parents), directory]:
        s=path.lstat()
        if (not stat.S_ISDIR(s.st_mode) or s.st_uid not in (0,1000) or
                (s.st_mode & 0o022 and not (path == Path('/tmp') and s.st_mode & stat.S_ISVTX))):
            raise ValueError('Unsafe F2 directory: '+str(path))
    if set(p.name for p in directory.iterdir()) != set(FILES):
        raise ValueError('F2 assets must have exact complete coverage')
    for name,(size,digest) in FILES.items():
        fd=os.open(directory/name,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
        with os.fdopen(fd,'rb') as file:
            before=os.fstat(file.fileno())
            if (not stat.S_ISREG(before.st_mode) or before.st_uid not in (0,1000) or
                    before.st_mode & 0o6022 or before.st_size!=size):
                raise ValueError('Unsafe F2 asset: '+name)
            h=hashlib.sha256()
            for block in iter(lambda:file.read(1024*1024),b''):h.update(block)
            after=os.fstat(file.fileno())
            stable=lambda x:(x.st_dev,x.st_ino,x.st_mode,x.st_uid,x.st_gid,x.st_size,x.st_mtime_ns,x.st_ctime_ns)
            if h.hexdigest()!=digest or stable(after)!=stable(before):
                raise ValueError('F2 asset changed: '+name)


def provision_models(source, destination):
    """Publish a root-owned copy; existing pilot weights are never modified."""
    import tempfile
    from .firefly_release import _publish_directory, _require_space
    verify_models(source)
    destination=Path(destination)
    if destination.exists() or destination.is_symlink():
        verify_models(destination);return
    parent=destination.parent
    if not parent.is_dir() or parent.resolve()!=parent:
        raise ValueError('Canonical existing model parent required')
    _require_space(parent,sum(size for size,_ in FILES.values()))
    with tempfile.TemporaryDirectory(prefix='.f2-',dir=parent) as tmp:
        stage=Path(tmp)/'tts';stage.mkdir(mode=0o755)
        for name,(size,_) in FILES.items():
            fd=os.open(Path(source)/name,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
            with os.fdopen(fd,'rb') as src,(stage/name).open('xb') as dst:
                if not stat.S_ISREG(os.fstat(src.fileno()).st_mode):raise ValueError('Unsafe source')
                remaining=size
                while remaining:
                    block=src.read(min(1024*1024,remaining))
                    if not block:raise ValueError('Truncated F2 source')
                    dst.write(block);remaining-=len(block)
                if src.read(1):raise ValueError('Oversized F2 source')
                dst.flush();os.fsync(dst.fileno())
            (stage/name).chmod(0o644)
        verify_models(stage)
        _publish_directory(stage,destination)
