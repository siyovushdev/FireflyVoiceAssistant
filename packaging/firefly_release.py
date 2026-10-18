"""Relocatable Firefly release staging, verification, activation and rollback.

Staging never installs a systemd unit. Explicit installation and activation
commands require FIREFLY_OWNER_ACCEPTED=1 at the fixed production paths.
No command restarts a service or writes a ConfigStore.
"""
from __future__ import annotations

import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
import pwd
import re
import select
import shlex
import shutil
import stat
import subprocess
import sys
import time
import tempfile
import urllib.parse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


PRODUCTION_PREFIX = "/opt/firefly/voice-assistant"
STORE_DIR = "/var/lib/firefly/store"
LIVE_STORE = Path("/var/lib/firefly/store")
LIVE_UNIT = Path("/etc/systemd/system/firefly-voice-assistant.service")
DISK_MARGIN_BYTES = 64 * 1024 * 1024
WAKE_READY_TIMEOUT_SECONDS = 10.0
# First production model copy. Reuse avoids a second copy; it does not shrink this.
PRODUCTION_MODEL_BYTES = 4840064228 + 4848176220 + 68187180 + 2093079 + 259572 + 6388 + 643854
PRODUCTION_REPO = Path("/home/firefly/FireflyVoiceAssistantLinux")
VENDOR_SDK = Path("/home/firefly/demo_Linux_aarch64")
READABLE_SCHEMAS = (1, 2)
WRITES_SCHEMA = 2
RELEASE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,80}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{7,64}$")

RUNTIME_MODEL = "models/runtime/Qwen3-4B_W8A8_RK3588_3core.rkllm"
ADMIN_MODEL = "models/admin/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm"
STT_DIR = "models/stt"
VAD_MODEL = "models/vad/silero_vad.onnx"
STT_FILES = ("encoder.int8.onnx", "decoder.onnx", "joiner.int8.onnx", "tokens.txt")
ALLOWED_SHARED_MODELS = (
    RUNTIME_MODEL, ADMIN_MODEL, VAD_MODEL, *(f"{STT_DIR}/{name}" for name in STT_FILES))

# Identity of the on-device artifacts. Production mode rejects any other bytes.
PRODUCTION_SHA256 = {
    RUNTIME_MODEL: "d806976a3d77f2252a6abe40a5fd7d9b1006a90f671992923ac2c1542456ba8b",
    ADMIN_MODEL: "58e8e74d8f129ea6f68b8e2101aae65d512668f46bb847a89e88a6a6d6094e5b",
    VAD_MODEL: "9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6",
    f"{STT_DIR}/encoder.int8.onnx": "69a347ddf7e5d39452d17f059dfedcd8c9471ca1953c320cb04ca19626149fdb",
    f"{STT_DIR}/decoder.onnx": "3977101b53640e50dff4274988ef754cd7ea539ae480baed0d18fc4966ad6917",
    f"{STT_DIR}/joiner.int8.onnx": "a6f550b56375326ee4a67dcac2cac027035e2dc137c08fff3557f4ffdc5cbc3e",
    f"{STT_DIR}/tokens.txt": "93bbbc0bae6b78c0bbb743d4aa9fded3bb5ff3aac5f0200e3a769a5a05e0fdf6",
    "lib/librkllmrt.so": "6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6",
    "lib/libsherpa-onnx-c-api.so": "5997c35aa68c8843b2ddfa6208b6961a07f171fa636633dd8941270e1aaacf5f",
    "lib/libonnxruntime.so": "d860f5968f5a1ed63533e9ed198aa747ca9fe289028129877f428556089f6874",
}
ADMIN_MODEL_BYTES = 4848176220
AMINA_SHA256 = "bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1"
AMINA_BYTES = 60912
WAKE_MODEL = "share/firefly/wake/amina-v3.tflite"
PINNED_PYTHON_PACKAGES = {
    "numpy": "2.2.6",
    "ai-edge-litert": "1.2.0",
    "pymicro-features": "2.0.2",
}
SAME_DEVICE_PATHS = (
    "lib/librkllmrt.so",
    RUNTIME_MODEL,
    ADMIN_MODEL,
)
SECRET_NAMES = {
    "configuration.json",
    ".pending.json",
    "last_known_good.json",
    "last_known_good_snapshot.json",
    ".env",
    "id_rsa",
    "credentials.json",
}


class ReleaseError(Exception):
    """The release was not published, or activation/rollback made no change."""


@dataclass
class StageRequest:
    destination: Path
    build_dir: Path
    source_root: Path
    rkllm_library: Path
    sherpa_library: Path
    onnx_library: Path
    runtime_model: Path
    admin_model: Path
    stt_dir: Path
    vad_model: Path
    wake_site_packages: Path
    release_id: str = ""
    commit: str = ""
    runtime_prefix: str = ""
    production: bool = False
    dirty: bool = False
    build_head: str = ""
    build_source: str = ""
    build_time: str = ""
    build_matches: bool = False
    reuse_installed_models: bool = False
    wake_reference_release: str = ""
    offline_candidate: bool = False
    mqtt_root: Optional[Path] = None
    tls_root: Optional[Path] = None
    mqtt_provenance: Optional[Path] = None


def _within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def assert_real_path(path: Path) -> Path:
    """Reject symlinks in every existing component before resolving a path."""
    absolute = Path(os.path.abspath(path))
    for component in (*reversed(absolute.parents), absolute):
        if component.is_symlink():
            raise ReleaseError(f"refusing symlink in path: {component}")
    return absolute


def _publish_directory(source: Path, target: Path) -> None:
    """Linux atomic rename with no replacement, including an empty directory."""
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(source), -100, os.fsencode(target), 1) != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(target))


def _lock_prefix(prefix: Path) -> int:
    assert_real_path(prefix)
    info = prefix.stat()
    if info.st_uid != os.geteuid() or info.st_mode & 0o022:
        raise ReleaseError("release prefix must be owned by the invoking user and not group/other writable")
    path = prefix / ".release.lock"
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ReleaseError("unsafe release lock")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd
    except Exception:
        os.close(fd)
        raise


def _owner_accepted() -> bool:
    return os.environ.get("FIREFLY_OWNER_ACCEPTED") == "1"


def assert_stage_destination(destination: Path, source_root: Path) -> Path:
    """Refuse live install roots and the source tree. Does not create anything."""
    resolved = assert_real_path(destination)
    source = source_root.resolve()
    blocked = (
        Path("/opt"), Path("/etc"), Path("/usr"), Path("/lib"), Path("/bin"), Path("/var"),
        PRODUCTION_REPO, VENDOR_SDK, LIVE_STORE,
    )
    for root in blocked:
        if resolved == root or _within(resolved, root):
            raise ReleaseError(f"refusing to stage into {resolved}")
    if resolved == source or _within(resolved, source):
        raise ReleaseError("stage destination must be outside the source tree")
    return resolved


def assert_activation_destination(destination: Path) -> Path:
    resolved = assert_real_path(destination)
    blocked = (
        Path("/etc"), Path("/usr"), Path("/lib"), Path("/bin"), Path("/var"),
        PRODUCTION_REPO, VENDOR_SDK, LIVE_STORE,
    )
    for root in blocked:
        if resolved == root or _within(resolved, root):
            raise ReleaseError(f"refusing to activate or roll back under {resolved}")
    under_opt = resolved == Path("/opt") or _within(resolved, Path("/opt"))
    if under_opt:
        live = resolved == Path(PRODUCTION_PREFIX)
        if not (live and _owner_accepted()):
            raise ReleaseError(
                "refusing to change /opt without FIREFLY_OWNER_ACCEPTED=1; "
                "staging and tests use a private prefix")
    return resolved


def assert_store_readable(store: Optional[Path]) -> None:
    if store is None:
        return
    resolved = store.resolve()
    if (resolved == LIVE_STORE or _within(resolved, LIVE_STORE)) and not _owner_accepted():
        raise ReleaseError("refusing to read the live production ConfigStore")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise ReleaseError(f"{path} is not a regular file")
        while True:
            block = os.read(fd, 1024 * 1024)
            if not block:
                break
            digest.update(block)
    finally:
        os.close(fd)
    return digest.hexdigest()


def require_regular_file(path: Path, label: str) -> None:
    if not path.exists() and not path.is_symlink():
        raise ReleaseError(f"missing {label}: {path}")
    if path.is_symlink() or not path.is_file():
        raise ReleaseError(f"{label} must be a regular file, not a symlink: {path}")
    if path.stat().st_size <= 0:
        raise ReleaseError(f"{label} is empty: {path}")


def render_unit(runtime_prefix: str) -> str:
    prefix = runtime_prefix.rstrip("/")
    if (not re.fullmatch(r"/[A-Za-z0-9/_+.-]+", prefix) or
            any(part in ("", ".", "..") for part in prefix.split("/")[1:])):
        raise ReleaseError("runtime prefix must be an absolute safe path without traversal or whitespace")
    current = prefix + "/current"
    return f"""# Firefly single-process appliance — M18.6 relocatable production unit.
# Not installed by the staging tool. Owner activation: docs/m18-owner-acceptance-runbook.md
# Argument contract is shared with scripts/firefly-dialogue.sh via FIREFLY_PREFIX.
# Libraries resolve from the binary RUNPATH ($ORIGIN/../lib).

[Unit]
Description=Firefly Voice Assistant
Documentation=file://{current}/share/firefly/doc/m18-relocatable-production-packaging.md
After=network-online.target sound.target
Wants=network-online.target
# Avoid rapid crash loops from bad config / missing model
StartLimitIntervalSec=120
StartLimitBurst=5

[Service]
Type=simple
User=firefly
Group=firefly
WorkingDirectory={current}
ExecStart={current}/bin/firefly-demo \\
  --store {STORE_DIR} \\
  --init-config {current}/share/firefly/config/empty-home.json \\
  --model {prefix}/models/runtime/Qwen3-4B_W8A8_RK3588_3core.rkllm \\
  --admin-model {prefix}/models/admin/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm \\
  --admin-bind 127.0.0.1 \\
  --admin-port 8080 \\
  --voice \\
  --stt-model-dir {prefix}/models/stt \\
  --vad-model {prefix}/models/vad/silero_vad.onnx \\
  --audio-device plughw:CARD=Array,DEV=0 \\
  --wake-enabled true \\
  --wake-python {current}/share/firefly/wake/python/bin/python \\
  --wake-model {current}/share/firefly/wake/amina-v3.tflite \\
  --wake-cutoff 0.99 --wake-window 7 \\
  --playback-device plughw:CARD=rockchipes8388,DEV=0
Restart=on-failure
RestartSec=5
TimeoutStopSec=45
KillMode=mixed
KillSignal=SIGTERM
StandardOutput=journal
StandardError=journal
SyslogIdentifier=firefly-voice-assistant
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths={STORE_DIR}
ProtectHome=read-only

[Install]
WantedBy=multi-user.target
"""


def unit_arguments(text: str) -> dict:
    folded = re.sub(r"\\\n\s*", " ", text)
    command = shlex.split(_service_value(folded, "ExecStart"))
    result = {}
    tokens = iter(command[1:])
    for flag in tokens:
        if not flag.startswith("--") or flag in result:
            raise ReleaseError(f"invalid unit flag {flag}")
        result[flag] = True if flag == "--voice" else next(tokens)
    result["__binary__"] = command[0]
    return result


def _service_value(text: str, key: str) -> str:
    for line in text.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1]
    raise ReleaseError(f"unit is missing {key}")


def source_identity(source_root: Path) -> dict:
    """HEAD plus whether the worktree matches that commit."""
    commit = git_commit(source_root)
    status = subprocess.check_output(
        ["git", "-C", str(source_root), "status", "--porcelain", "--untracked-files=normal"],
        text=True)
    dirty = bool(status.strip())
    return {
        "commit": commit,
        "dirty": dirty,
        "identity": "dirty-worktree" if dirty else "committed",
    }


def git_commit(source_root: Path) -> str:
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError) as error:
        raise ReleaseError("cannot read the source commit; pass --commit") from error
    if not COMMIT_RE.fullmatch(commit):
        raise ReleaseError("source commit is not a hex sha")
    return commit


copied_destinations: list[str] = []


def _copy_regular(source: Path, dest: Path) -> str:
    require_regular_file(source, source.name)
    assert_real_path(dest)
    copied_destinations.append(str(dest))
    dest.parent.mkdir(parents=True, exist_ok=True)
    output_fd, partial_name = tempfile.mkstemp(prefix=f".{dest.name}.partial-", dir=dest.parent)
    partial = Path(partial_name)
    digest = hashlib.sha256()
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    fd = None
    try:
        fd = os.open(source, flags)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
            raise ReleaseError(f"refusing to copy {source}")
        with os.fdopen(output_fd, "wb") as out:
            output_fd = None
            os.fchmod(out.fileno(), 0o644)
            while True:
                block = os.read(fd, 1024 * 1024)
                if not block:
                    break
                digest.update(block)
                out.write(block)
            out.flush()
            os.fsync(out.fileno())
        os.replace(partial, dest)
    finally:
        if fd is not None:
            os.close(fd)
        if output_fd is not None:
            os.close(output_fd)
        if partial.exists():
            partial.unlink()
    return digest.hexdigest()


def _ignore_site(_directory: str, names: Iterable[str]) -> set:
    ignored = set()
    for name in names:
        if name in {"pip", "setuptools", "pkg_resources", "_distutils_hack", "__pycache__"}:
            ignored.add(name)
        elif name.startswith(("pip-", "setuptools-")) or name.endswith(".pyc") or name == "distutils-precedence.pth":
            ignored.add(name)
    return ignored


def _elf(path: Path) -> tuple[list[str], list[str]]:
    text = subprocess.check_output(["readelf", "-d", str(path)], text=True)
    needed = re.findall(r"Shared library: \[([^\]]+)\]", text)
    runpaths = re.findall(r"Library (?:runpath|rpath): \[([^\]]+)\]", text)
    return needed, runpaths


def check_elf(binary: Path) -> None:
    needed, runpaths = _elf(binary)
    for library in ("librkllmrt.so", "libsherpa-onnx-c-api.so", "libasound.so.2"):
        if library not in needed:
            raise ReleaseError(f"{binary.name} is missing NEEDED {library}")
    if runpaths != ["$ORIGIN/../lib"]:
        raise ReleaseError(f"{binary.name} RUNPATH is {runpaths}, expected ['$ORIGIN/../lib']")
    if b"/home/firefly" in binary.read_bytes():
        raise ReleaseError("staged firefly-demo still contains a /home/firefly runtime path")
    env = {key: value for key, value in os.environ.items() if key != "LD_LIBRARY_PATH"}
    listing = subprocess.check_output(["ldd", str(binary)], env=env, text=True)
    if "not found" in listing:
        raise ReleaseError(f"ldd could not resolve staged libraries:\n{listing}")
    lib_dir = (binary.parent.parent / "lib").resolve()
    for name in ("librkllmrt.so", "libsherpa-onnx-c-api.so", "libonnxruntime.so"):
        match = re.search(rf"{re.escape(name)} => (\S+)", listing)
        if not match:
            raise ReleaseError(f"ldd did not resolve {name}")
        resolved = Path(match.group(1)).resolve()
        if resolved.parent != lib_dir:
            raise ReleaseError(f"{name} resolved to {resolved}, not {lib_dir}")
        if "/home/firefly" in str(resolved):
            raise ReleaseError(f"{name} still resolves under /home/firefly")
    for library, origin in (("libsherpa-onnx-c-api.so", "$ORIGIN"), ("libonnxruntime.so", "$ORIGIN")):
        _needed, paths = _elf(lib_dir / library)
        if origin not in paths and "$ORIGIN" not in ":".join(paths):
            raise ReleaseError(f"{library} lost its $ORIGIN RPATH: {paths}")


def read_ready_byte(process: subprocess.Popen, timeout_seconds: float) -> bytes:
    """Read one stdout byte or raise when the deadline passes. Does not reap the process."""
    if process.stdout is None:
        raise ReleaseError("wake worker has no stdout")
    stream = process.stdout
    descriptor = stream.fileno()
    os.set_blocking(descriptor, False)
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        readable, _, _ = select.select([descriptor], [], [], min(0.2, remaining))
        if readable:
            chunk = stream.read(1)
            if chunk:
                return chunk
        if process.poll() is not None:
            break
    raise ReleaseError(f"wake worker did not send the ready byte within {timeout_seconds:.1f}s")


def _stop_process(process: subprocess.Popen) -> None:
    for stream in (process.stdin,):
        if stream:
            try:
                stream.close()
            except Exception:
                pass
    if process.poll() is None:
        process.kill()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    for stream in (process.stdout, process.stderr):
        if stream:
            try:
                stream.close()
            except Exception:
                pass


def check_wake(release: Path) -> str:
    """Import pinned packages and read the worker ready byte. Returns Python version."""
    python = release / "share/firefly/wake/python/bin/python"
    worker = release / "share/firefly/wake/amina_worker.py"
    model = release / "share/firefly/wake/amina-v3.tflite"
    if not os.access(python, os.X_OK):
        raise ReleaseError(f"wake interpreter is not executable: {python}")
    probe = (
        "import sys\n"
        "from importlib.metadata import version\n"
        "import numpy\n"
        "from ai_edge_litert.interpreter import Interpreter\n"
        "from pymicro_features import MicroFrontend\n"
        "MicroFrontend()\n"
        "print(sys.version.split()[0])\n"
        "print(numpy.__version__)\n"
        "print(version('ai-edge-litert'))\n"
        "print(version('pymicro-features'))\n"
        "bad=[p for p in sys.path if '/home/firefly' in p or 'FireflyVoiceAssistantLinux' in p]\n"
        "print('BAD' if bad else 'PATH-OK')\n"
    )
    env = {key: value for key, value in os.environ.items() if key != "LD_LIBRARY_PATH"}
    env["HOME"] = "/tmp"
    env["PYTHONNOUSERSITE"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [str(python), "-c", probe], env=env, text=True, capture_output=True, timeout=30, cwd="/tmp")
    if result.returncode != 0:
        raise ReleaseError(f"staged wake imports failed:\n{result.stderr}")
    lines = [line for line in result.stdout.splitlines() if line]
    if len(lines) != 5 or lines[4] != "PATH-OK":
        raise ReleaseError(f"unexpected wake probe output:\n{result.stdout}")
    version, numpy_version, litert, micro = lines[:4]
    if not version.startswith("3.10."):
        raise ReleaseError(f"wake interpreter is Python {version}, expected 3.10")
    actual = {"numpy": numpy_version, "ai-edge-litert": litert, "pymicro-features": micro}
    if actual != PINNED_PYTHON_PACKAGES:
        raise ReleaseError(f"wake package versions {actual} != {PINNED_PYTHON_PACKAGES}")
    process = subprocess.Popen(
        [str(python), str(worker), "--model", str(model), "--cutoff", "0.99", "--window", "7"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, cwd="/tmp")
    try:
        ready = read_ready_byte(process, WAKE_READY_TIMEOUT_SECONDS)
        if ready != b"R":
            error = b""
            if process.stderr:
                os.set_blocking(process.stderr.fileno(), False)
                error = process.stderr.read() or b""
            raise ReleaseError(f"wake worker did not become ready ({ready!r}): {error.decode('utf-8', 'replace')}")
    finally:
        _stop_process(process)
    return version


def _purge_pycache(root: Path) -> None:
    for cache in root.rglob("__pycache__"):
        shutil.rmtree(cache)


def _chmod_tree(root: Path) -> None:
    for directory, _dirs, files in os.walk(root):
        os.chmod(directory, 0o755)
        for name in files:
            path = Path(directory) / name
            if path.is_symlink():
                wake_reference(root, path)  # Never chmod the protected model target.
                continue
            executable = ".so" in path.name or name in {"firefly-demo", "firefly-config", "python"} or os.access(path, os.X_OK)
            os.chmod(path, 0o755 if executable else 0o644)


def _rel_files(root: Path) -> list[str]:
    assert_real_path(root)
    files = []
    for directory, dirs, names in os.walk(root, followlinks=False):
        for name in dirs + names:
            path = Path(directory) / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                wake_reference(root, path)
                files.append(str(path.relative_to(root)))
                continue
            if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
                raise ReleaseError(f"unexpected filesystem entry: {path}")
            if stat.S_ISREG(info.st_mode):
                if info.st_nlink != 1:
                    raise ReleaseError(f"refusing hardlinked artifact: {path}")
                files.append(str(path.relative_to(root)))
    return sorted(files)


def require_root_owned(path: Path, root: Path) -> None:
    for entry in [path, *path.parents]:
        info = entry.lstat()
        if info.st_uid != 0 or info.st_mode & 0o022:
            raise ReleaseError(f"reference must be root-owned and read-only to service user: {entry}")
        if entry == root:
            break


def installed_model_paths() -> dict:
    """Only the fixed, root-owned on-device model set; never arbitrary aliases."""
    root = assert_real_path(Path(PRODUCTION_PREFIX))
    result = {}
    for relative in ALLOWED_SHARED_MODELS:
        path = model_path(root, relative)
        require_regular_file(path, relative)
        require_root_owned(path, root)
        if path.stat().st_nlink != 1:
            raise ReleaseError(f"refusing hardlinked installed model: {path}")
        _expect(path, PRODUCTION_SHA256[relative], ADMIN_MODEL_BYTES if relative == ADMIN_MODEL else None)
        result[relative] = path
    return result


def wake_reference(root: Path, path: Path) -> Path:
    """The sole permitted release symlink is a pinned immutable Amina model."""
    if str(path.relative_to(root)) != WAKE_MODEL or not path.is_symlink():
        raise ReleaseError(f"unexpected release symlink: {path}")
    target = Path(os.readlink(path))
    releases = Path(PRODUCTION_PREFIX) / "releases"
    try:
        relative = target.relative_to(releases)
    except ValueError as error:
        raise ReleaseError("wake model reference must use the installed releases") from error
    if (len(relative.parts) != 5 or not RELEASE_ID_RE.fullmatch(relative.parts[0])
            or relative.parts[1:] != Path(WAKE_MODEL).parts or target.resolve() != target):
        raise ReleaseError("wake reference must name an exact immutable release model")
    require_regular_file(target, "referenced Amina model")
    require_root_owned(target, Path(PRODUCTION_PREFIX))
    if target.stat().st_nlink != 1:
        raise ReleaseError("refusing hardlinked wake reference target")
    _expect(target, AMINA_SHA256, AMINA_BYTES)
    return target


def artifact_path(release: Path, relative: str) -> Path:
    path = release / relative
    return wake_reference(release, path) if path.is_symlink() else path


def manifest_model_paths(document: dict, prefix: Path) -> dict:
    marker = document.get("installed_model_reference")
    if marker is None:
        if document.get("wake_model_reference") is not None:
            raise ReleaseError("wake reference requires the installed model contract")
        return {name: model_path(prefix, name) for name in ALLOWED_SHARED_MODELS}
    if (marker != {"prefix": PRODUCTION_PREFIX, "mode": "installed-read-only"}
            or document.get("runtime_prefix") != PRODUCTION_PREFIX
            or document.get("production_identity_checked") is not True):
        raise ReleaseError("invalid installed model reference contract")
    return installed_model_paths()


def _manifest_document(request: StageRequest, release: Path, model_paths: dict, python_version: str) -> dict:
    binary_identity = build_identity(release / "bin/firefly-demo")
    artifacts = []
    for relative in _rel_files(release):
        if relative in {"manifest.json", "manifest.sha256"}:
            continue
        path = artifact_path(release, relative)
        if path.name in SECRET_NAMES:
            raise ReleaseError(f"refusing to package {relative}")
        artifacts.append({
            "path": relative,
            "sha256": sha256_file(path),
            "bytes": path.stat().st_size,
            **({"mode": path.stat().st_mode & 0o7777} if mqtt_enabled(request.build_dir) else {}),
        })
    models = []
    for relative in (RUNTIME_MODEL, ADMIN_MODEL, VAD_MODEL, *(f"{STT_DIR}/{name}" for name in STT_FILES)):
        if request.offline_candidate:
            continue
        path = model_paths[relative]
        models.append({
            "path": relative,
            "sha256": sha256_file(path),
            "bytes": path.stat().st_size,
            "shared": True,
            "distribution": "same-device-only" if relative in (RUNTIME_MODEL, ADMIN_MODEL) else "device-local",
        })
    document = {
        "format": 1,
        "milestone": "M18.6",
        "release_id": request.release_id,
        "source_commit": request.commit,
        "source": {
            "commit": request.commit,
            "dirty": request.dirty,
            "identity": (
                "dirty-worktree" if request.dirty
                else "committed-production" if request.production
                else "committed-fixture"
            ),
        },
        "architecture": "aarch64",
        "operating_system": "linux",
        "runtime_prefix": request.runtime_prefix,
        "production_identity_checked": bool(request.production and request.build_matches),
        "build": {
            "head": request.build_head,
            "source_sha256": request.build_source,
            "built_utc": request.build_time,
            "matches_worktree": request.build_matches,
            "dirty": binary_identity.get("dirty"),
            "admin_assets_sha256": binary_identity.get("admin_assets_sha256"),
        },
        "config_schema": {"readable": list(READABLE_SCHEMAS), "writes": WRITES_SCHEMA},
        "dependencies": {
            "rkllm": "1.3.0",
            "sherpa_onnx": "1.13.7",
            "onnxruntime": "bundled-with-sherpa-onnx-1.13.7",
            "python": python_version,
            "numpy": PINNED_PYTHON_PACKAGES["numpy"],
            "ai_edge_litert": PINNED_PYTHON_PACKAGES["ai-edge-litert"],
            "pymicro_features": PINNED_PYTHON_PACKAGES["pymicro-features"],
            "amina_wake": "v3",
            "amina_sha256": AMINA_SHA256,
        },
        "distribution": {
            "same_device_only": list(SAME_DEVICE_PATHS),
            "note": (
                "librkllmrt.so and the RKLLM models are staged for this device only. "
                "The local SDK has no redistribution license. sherpa-onnx is Apache-2.0 "
                "and ONNX Runtime is MIT, but this bundle does not include their license files; "
                "do not ship those libraries off-device without the upstream notices."
            ),
        },
        "ownership": {
            "staged_as_invoking_user": True,
            "service_user": "firefly",
            "note": "Owner must chown the installed tree to root and keep it read-only for the service user.",
        },
        "artifacts": artifacts,
        "shared_models": models,
        "systemd": {
            "unit": "share/firefly/systemd/firefly-voice-assistant.service",
            "installed": False,
        },
        "activation": {"creates_current_symlink": False},
    }
    if request.reuse_installed_models:
        document["installed_model_reference"] = {"prefix": PRODUCTION_PREFIX, "mode": "installed-read-only"}
        document["wake_model_reference"] = str(wake_reference(release, release / WAKE_MODEL))
    if mqtt_enabled(request.build_dir):
        from . import mqtt_release
        document["mqtt"] = mqtt_release.metadata()
    if request.offline_candidate:
        from . import mqtt_release
        document["source"]["identity"] = "committed-offline-candidate"
        document["shared_models"] = mqtt_release.model_references()
        document["offline_candidate"] = {"state": "OWNER_ACTIVATION_REQUIRED", "models": "frozen-target-references"}
    return document


def _write_manifest(release: Path, document: dict) -> None:
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    manifest = release / "manifest.json"
    manifest.write_text(text)
    (release / "manifest.sha256").write_text(hashlib.sha256(text.encode()).hexdigest() + "\n")
    os.chmod(manifest, 0o644)
    os.chmod(release / "manifest.sha256", 0o644)


def load_manifest(release: Path) -> dict:
    assert_real_path(release)
    manifest = release / "manifest.json"
    require_regular_file(manifest, "manifest.json")
    require_regular_file(release / "manifest.sha256", "manifest.sha256")
    body = manifest.read_bytes()
    recorded = (release / "manifest.sha256").read_text().strip()
    actual = hashlib.sha256(body).hexdigest()
    if recorded != actual:
        raise ReleaseError("manifest.sha256 does not match manifest.json")
    document = json.loads(body)
    if document.get("format") != 1 or document.get("release_id") != release.name:
        raise ReleaseError("manifest does not describe this release directory")
    return document


def assert_shared_model_manifest(entries) -> list:
    """Reject unsafe model paths before any filesystem join."""
    if not isinstance(entries, list):
        raise ReleaseError("shared_models must be a list")
    seen = []
    for item in entries:
        if not isinstance(item, dict):
            raise ReleaseError("shared model entry is malformed")
        path = item.get("path")
        if not isinstance(path, str) or path != path.strip() or "\x00" in path or "\\" in path:
            raise ReleaseError("shared model path is malformed")
        if path.startswith("/") or path.startswith("~"):
            raise ReleaseError(f"absolute model path rejected: {path}")
        parts = path.split("/")
        if any(part in ("", ".", "..") for part in parts):
            raise ReleaseError(f"model path traversal rejected: {path}")
        if path not in ALLOWED_SHARED_MODELS:
            raise ReleaseError(f"unexpected model path rejected: {path}")
        if path in seen:
            raise ReleaseError(f"duplicate model path rejected: {path}")
        seen.append(path)
    if seen != list(ALLOWED_SHARED_MODELS) and set(seen) != set(ALLOWED_SHARED_MODELS):
        raise ReleaseError("shared_models do not match the production model set")
    if len(seen) != len(ALLOWED_SHARED_MODELS):
        raise ReleaseError("shared_models do not match the production model set")
    return seen


ARTIFACT_FILES = {
    "bin/firefly-demo", "bin/firefly-config", "lib/librkllmrt.so", "lib/libsherpa-onnx-c-api.so", "lib/libonnxruntime.so",
    "share/firefly/config/empty-home.json", "share/firefly/wake/amina-v3.tflite",
    "share/firefly/wake/amina_worker.py", "share/firefly/wake/manifest.json",
    "share/firefly/wake/requirements.txt", "share/firefly/wake/python/bin/python",
    "share/firefly/systemd/firefly-voice-assistant.service",
    "share/firefly/doc/m18-owner-acceptance-runbook.md",
    "share/firefly/doc/m18-relocatable-production-packaging.md",
    "share/firefly/deployment/scripts/stage-release.py",
    "share/firefly/deployment/packaging/__init__.py",
    "share/firefly/deployment/packaging/firefly_release.py",
}
SITE_PREFIX = "share/firefly/wake/python/lib/python3.10/site-packages/"


def assert_artifact_manifest(entries, *, mqtt: bool = False) -> dict:
    if not isinstance(entries, list):
        raise ReleaseError("artifacts must be a list")
    expected = {}
    required = ARTIFACT_FILES
    if mqtt:
        from .mqtt_release import EXTRA_FILES
        required = ARTIFACT_FILES | EXTRA_FILES
    for item in entries:
        if not isinstance(item, dict):
            raise ReleaseError("artifact entry is malformed")
        path = item.get("path")
        if (not isinstance(path, str) or "\x00" in path or "\\" in path or
                any(part in ("", ".", "..") for part in path.split("/")) or
                (path not in required and not path.startswith(SITE_PREFIX)) or
                path in expected):
            raise ReleaseError(f"unsafe or unexpected artifact path: {path!r}")
        digest, size = item.get("sha256"), item.get("bytes")
        if (not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest) or
                type(size) is not int or size < 0):
            raise ReleaseError(f"malformed artifact metadata: {path}")
        expected[path] = item
    if not required.issubset(expected):
        raise ReleaseError("manifest is missing required release artifacts")
    return expected


def model_path(root: Path, relative: str) -> Path:
    """Resolve one allowlisted model path and refuse symlinks or escape."""
    if relative not in ALLOWED_SHARED_MODELS:
        raise ReleaseError(f"unexpected model path rejected: {relative}")
    base = assert_real_path(root)
    current = base
    for part in relative.split("/"):
        if current.is_symlink():
            raise ReleaseError(f"refusing symlink in model path: {current}")
        current = current / part
        if current.is_symlink():
            raise ReleaseError(f"refusing symlink in model path: {current}")
    parent = current.parent
    if parent.exists():
        resolved_parent = parent.resolve()
        if resolved_parent != base and not _within(resolved_parent, base):
            raise ReleaseError(f"model path escapes the prefix: {relative}")
    return current


def assert_model_tree(prefix: Path) -> None:
    root = prefix / "models"
    assert_real_path(root)
    if not root.exists():
        return
    present = {"models/" + path for path in _rel_files(root)}
    if not present.issubset(ALLOWED_SHARED_MODELS):
        raise ReleaseError("unexpected file in shared model tree")
    for directory, dirs, _files in os.walk(root):
        for name in dirs:
            relative = str((Path(directory) / name).relative_to(root))
            if relative not in {"runtime", "admin", "stt", "vad"}:
                raise ReleaseError(f"unexpected shared model directory: {relative}")


def assert_production_ownership(prefix: Path) -> None:
    for root in (prefix, prefix / "releases", prefix / "models"):
        info = root.stat()
        if info.st_uid != 0 or info.st_mode & 0o022:
            raise ReleaseError(f"production directory must be root-owned without group/other write: {root}")
    for root in (prefix / "releases", prefix / "models"):
        for directory, dirs, files in os.walk(root, followlinks=False):
            for name in dirs + files:
                path = Path(directory) / name
                info = path.lstat()
                if stat.S_ISLNK(info.st_mode):
                    parts = path.relative_to(prefix / "releases").parts if root == prefix / "releases" else ()
                    if not parts:
                        raise ReleaseError("shared model symlinks are forbidden")
                    release = prefix / "releases" / parts[0]
                    document = load_manifest(release)
                    if (document.get("installed_model_reference") != {"prefix": PRODUCTION_PREFIX, "mode": "installed-read-only"}
                            or document.get("wake_model_reference") != str(wake_reference(release, path))):
                        raise ReleaseError("production symlink requires the pinned wake contract")
                if info.st_uid != 0 or (not stat.S_ISLNK(info.st_mode) and info.st_mode & 0o022):
                    raise ReleaseError("production artifacts must be root-owned without group/other write")


def assert_production_runtime_prefix(runtime_prefix: str, destination: Path) -> None:
    destination_text = str(destination).rstrip("/")
    if destination_text == PRODUCTION_PREFIX or _within(Path(destination_text), Path(PRODUCTION_PREFIX)):
        if str(runtime_prefix).rstrip("/") != PRODUCTION_PREFIX:
            raise ReleaseError(
                "installing into /opt/firefly/voice-assistant requires "
                "--runtime-prefix /opt/firefly/voice-assistant")


def require_production_identity(document: dict, release: Path) -> None:
    source, build = document.get("source", {}), document.get("build", {})
    identity = build_identity(release / "bin/firefly-demo")
    if (document.get("production_identity_checked") is not True or
            source.get("dirty") is not False or source.get("identity") != "committed-production" or
            build.get("matches_worktree") is not True or identity.get("dirty") is not False or
            build.get("dirty") is not False or
            identity.get("admin_assets_sha256") != build.get("admin_assets_sha256") or
            identity.get("head") != source.get("commit") or identity.get("head") != build.get("head") or
            identity.get("source_sha256") != build.get("source_sha256")):
        raise ReleaseError("production destination requires a verified clean committed-production release")
    entries = {item["path"]: item for item in document["artifacts"] + document["shared_models"]}
    for relative, digest in PRODUCTION_SHA256.items():
        if entries.get(relative, {}).get("sha256") != digest:
            raise ReleaseError(f"production artifact identity mismatch: {relative}")


def is_production_prefix(prefix: Path) -> bool:
    return prefix == Path(PRODUCTION_PREFIX) or _within(prefix, Path(PRODUCTION_PREFIX))


def require_production_store(store: Path, *, fresh: bool = False) -> None:
    if assert_real_path(store) != LIVE_STORE:
        raise ReleaseError("production activation/rollback must audit /var/lib/firefly/store used by the unit")
    if fresh:
        raise ReleaseError("production cutover requires an existing validated ConfigStore; --fresh is fixture-only")
    try:
        service_uid = pwd.getpwnam("firefly").pw_uid
    except KeyError as error:
        raise ReleaseError("production service user firefly is missing") from error
    if store.stat().st_uid != service_uid:
        raise ReleaseError("production ConfigStore must be owned by the service user firefly")


def source_tree_fingerprint(source_root: Path) -> str:
    """Match the CMake source manifest hashed into firefly-demo."""
    root = source_root.resolve()
    files = []

    def add_tree(directory: Path) -> None:
        if not directory.is_dir() or directory.is_symlink():
            return
        for dirpath, dirnames, filenames in os.walk(directory):
            dirnames[:] = sorted(name for name in dirnames if not name.startswith("."))
            for name in sorted(filenames):
                if name.startswith("."):
                    continue
                files.append(Path(dirpath) / name)

    source = root / "src"
    for child in sorted(source.iterdir(), key=lambda item: item.name):
        if child.name.startswith("."):
            continue
        if child.name == "admin":
            continue
        if child.is_dir() and not child.is_symlink():
            add_tree(child)
        elif child.is_file() and not child.is_symlink():
            files.append(child)
    admin = source / "admin"
    for pattern in ("*.cpp", "*.h", "*.in", "*.txt"):
        files.extend(path for path in admin.glob(pattern) if path.is_file() and not path.is_symlink())
    frontend = admin / "frontend"
    for pattern in ("*.json", "*.ts", "*.html"):
        files.extend(path for path in frontend.glob(pattern) if path.is_file() and not path.is_symlink())
    add_tree(root / "include")
    add_tree(root / "config")
    add_tree(root / "third_party")
    add_tree(frontend / "src")
    files.extend((root / "CMakeLists.txt", root / "scripts/package-admin-v2.py"))
    unique = sorted({path.resolve() for path in files})
    manifest = []
    for path in unique:
        digest = sha256_file(path)
        manifest.append(f"{path}:{digest}\n")
    return hashlib.sha256("".join(manifest).encode()).hexdigest()


def admin_assets_fingerprint(build_dir: Path) -> str:
    cache = (build_dir / "CMakeCache.txt").read_text()
    match = re.search(r"^FIREFLY_ADMIN_DIST_DIR:PATH=(.+)$", cache, re.MULTILINE)
    if not match:
        raise ReleaseError("build cache is missing FIREFLY_ADMIN_DIST_DIR")
    root = Path(match.group(1))
    files = sorted(path for path in root.rglob("*") if path.is_file())
    return hashlib.sha256("".join(
        f"{path.relative_to(root)}:{sha256_file(path)}\n" for path in files).encode()).hexdigest()


def build_identity(binary: Path) -> dict:
    result = subprocess.run(
        [str(binary), "--build-identity"], text=True, capture_output=True, timeout=15)
    if result.returncode != 0:
        raise ReleaseError(f"firefly-demo --build-identity failed: {result.stderr.strip()}")
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ReleaseError(f"firefly-demo build identity is malformed: {error}") from error
    for key in ("head", "source_sha256", "built_utc"):
        if not isinstance(document.get(key), str) or not document[key]:
            raise ReleaseError(f"firefly-demo build identity is missing {key}")
    return document


def verify_tree(prefix: Path, release_id: str, *, check_python: bool = True,
                model_paths: Optional[dict] = None, candidate_only: bool = False) -> dict:
    assert_real_path(prefix)
    if not RELEASE_ID_RE.fullmatch(release_id):
        raise ReleaseError("invalid release id")
    release = prefix / "releases" / release_id
    assert_model_tree(prefix)
    if is_production_prefix(prefix):
        assert_production_ownership(prefix)
    document = load_manifest(release)
    if candidate_only:
        from .mqtt_release import assert_candidate
        assert_candidate(document)
        resolved_models = {}
    else:
        if document.get("offline_candidate") is not None:
            raise ReleaseError("offline candidate requires owner finalization with actual target model verification")
        resolved_models = manifest_model_paths(document, prefix)
    wake = release / WAKE_MODEL
    if wake.is_symlink():
        if (document.get("installed_model_reference") is None
                or document.get("wake_model_reference") != str(wake_reference(release, wake))):
            raise ReleaseError("unrecorded or mismatched wake reference")
    elif document.get("wake_model_reference") is not None:
        raise ReleaseError("wake reference manifest requires its exact symlink")
    assert_shared_model_manifest(document.get("shared_models"))
    expected = assert_artifact_manifest(document.get("artifacts"), mqtt=document.get("mqtt", {}).get("enabled") is True)
    present = set(_rel_files(release))
    present.discard("manifest.json")
    present.discard("manifest.sha256")
    if present != set(expected):
        missing = sorted(set(expected) - present)
        extra = sorted(present - set(expected))
        raise ReleaseError(f"release tree does not match the manifest (missing={missing}, extra={extra})")
    allowed_dirs = {str(parent) for name in present | {"manifest.json", "manifest.sha256"}
                    for parent in Path(name).parents if str(parent) != "."}
    for directory, dirs, _names in os.walk(release):
        for name in dirs:
            relative = str((Path(directory) / name).relative_to(release))
            if relative not in allowed_dirs:
                raise ReleaseError(f"unexpected release directory: {relative}")
    for relative, item in expected.items():
        path = artifact_path(release, relative)
        if path.name in SECRET_NAMES:
            raise ReleaseError(f"release contains {relative}")
        if sha256_file(path) != item["sha256"] or path.stat().st_size != item["bytes"]:
            raise ReleaseError(f"checksum mismatch: {relative}")
        if "mode" in item and (type(item["mode"]) is not int or path.stat().st_mode & 0o7777 != item["mode"]):
            raise ReleaseError(f"artifact mode mismatch: {relative}")
    for item in document["shared_models"]:
        if candidate_only:
            continue  # assert_candidate checked the exact frozen references, never fictitious model bytes.
        relative = item["path"]
        path = resolved_models[relative] if document.get("installed_model_reference") else (model_paths or {}).get(relative, resolved_models[relative])
        require_regular_file(path, item["path"])
        if sha256_file(path) != item["sha256"] or path.stat().st_size != item["bytes"]:
            raise ReleaseError(f"shared model checksum mismatch: {item['path']}")
    binary = release / "bin/firefly-demo"
    check_elf(binary)
    identity = build_identity(binary)
    if build_identity(release / "bin/firefly-config") != identity:
        raise ReleaseError("release firefly-config and firefly-demo build identities differ")
    build = document.get("build", {})
    for identity_key, manifest_key in (("head", "head"), ("source_sha256", "source_sha256"),
                                       ("built_utc", "built_utc"), ("dirty", "dirty"),
                                       ("admin_assets_sha256", "admin_assets_sha256")):
        if identity.get(identity_key) != build.get(manifest_key):
            raise ReleaseError(f"manifest build identity mismatch: {identity_key}")
    unit = (release / "share/firefly/systemd/firefly-voice-assistant.service").read_text()
    if unit != render_unit(document["runtime_prefix"]):
        raise ReleaseError("staged systemd unit does not match the packaging contract")
    if "/home/firefly" in unit or "LD_LIBRARY_PATH" in unit:
        raise ReleaseError("staged unit still depends on a developer path or LD_LIBRARY_PATH")
    seed = json.loads((release / "share/firefly/config/empty-home.json").read_text())
    if seed.get("schema_version") != 2:
        raise ReleaseError("empty init-config is not schema 2")
    for name in ("rooms", "devices", "actions", "bindings", "connections", "observations"):
        if seed.get(name, []) != []:
            raise ReleaseError(f"empty init-config contains {name}")
    amina = artifact_path(release, WAKE_MODEL)
    if amina.stat().st_size != AMINA_BYTES or sha256_file(amina) != AMINA_SHA256:
        raise ReleaseError("Amina model bytes do not match the frozen artifact")
    if check_python:
        before = set(_rel_files(release))
        version = check_wake(release)
        if set(_rel_files(release)) != before:
            raise ReleaseError("wake probe created files in the release")
        if version != document["dependencies"]["python"]:
            raise ReleaseError("manifest Python version does not match the staged interpreter")
    if is_production_prefix(prefix):
        assert_production_runtime_prefix(document.get("runtime_prefix", ""), prefix)
        require_production_identity(document, release)
    if document.get("mqtt", {}).get("enabled") is True:
        from .mqtt_release import metadata, verify
        if document["mqtt"] != metadata():
            raise ReleaseError("invalid MQTT package contract")
        verify(release)
    return document


def assert_schema_compatible(version: Optional[int], readable: Iterable[int]) -> None:
    allowed = list(readable)
    if version not in allowed:
        raise ReleaseError(
            f"ConfigStore schema_version {version} is not readable by this release {allowed}. "
            "The store was not modified.")


def _secure_directory(store: Path) -> None:
    if store.is_symlink() or not store.is_dir():
        raise ReleaseError("ConfigStore must be a real directory")
    info = store.stat()
    if info.st_uid != os.geteuid() and not (os.geteuid() == 0 and info.st_uid != 0):
        raise ReleaseError("ConfigStore must be owned by the invoking user")
    if info.st_mode & 0o022:
        raise ReleaseError("ConfigStore must not be group or other writable")
    pending = store / ".pending.json"
    if pending.exists() or pending.is_symlink():
        raise ReleaseError("Interrupted transaction; reconcile before activation. The store was not modified.")


def require_store(store: Path, readable: Iterable[int], *, fresh: bool = False, config_tool: Path) -> Optional[int]:
    """Validate with firefly-config audit. Never writes the store."""
    if not isinstance(store, Path) or not isinstance(config_tool, Path):
        raise ReleaseError("ConfigStore path and firefly-config are required")
    assert_store_readable(store)
    _secure_directory(store)
    config = store / "configuration.json"
    if fresh:
        if config.exists() or config.is_symlink():
            raise ReleaseError("fresh initialization requires configuration.json to be absent")
        return None
    if not config_tool.is_file():
        raise ReleaseError(f"firefly-config is missing: {config_tool}")
    result = subprocess.run(
        [str(config_tool), "audit", "--store", str(store)],
        text=True, capture_output=True, timeout=30)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "audit failed").strip()
        raise ReleaseError(f"ConfigStore rejected by firefly-config: {detail}")
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ReleaseError(f"firefly-config audit output is malformed: {error}") from error
    version = document.get("schema_version")
    if isinstance(version, bool) or not isinstance(version, int):
        raise ReleaseError("firefly-config did not report an integer schema_version")
    assert_schema_compatible(version, readable)
    return version


# Test hooks.
publish_after = None
install_after_models = None


def _claim_new_file(source: Path, target: Path, created: list) -> None:
    """Publish one regular file without replacing an existing target inode."""
    if target.is_symlink():
        raise ReleaseError(f"refusing to overwrite symlink {target}")
    if target.exists():
        if target.is_file() and sha256_file(target) == sha256_file(source):
            return
        raise ReleaseError(f"refusing to overwrite existing {target}")
    target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    incoming = target.with_name(f".{target.name}.incoming-{os.getpid()}-{len(created)}")
    os.rename(source, incoming)
    created.append(incoming)
    try:
        os.link(incoming, target)
    except FileExistsError:
        incoming_hash = sha256_file(incoming)
        incoming.unlink()
        created.remove(incoming)
        if target.is_file() and not target.is_symlink() and sha256_file(target) == incoming_hash:
            return
        raise ReleaseError(f"refusing to overwrite existing {target}")
    created.append(target)
    incoming.unlink()
    created.remove(incoming)


def publish_shared_models(work_prefix: Path, destination: Path) -> list:
    """Move new shared models into destination. Returns only files this call created."""
    created: list = []
    try:
        for relative in ALLOWED_SHARED_MODELS:
            source = model_path(work_prefix, relative)
            target = model_path(destination, relative)
            if not source.exists():
                if target.is_symlink() or not target.is_file():
                    raise ReleaseError(f"reused model is missing: {target}")
                continue
            _claim_new_file(source, target, created)
            if publish_after is not None:
                publish_after(relative)
        return [path for path in created if path.exists()]
    except Exception:
        for path in reversed(created):
            try:
                if path.is_symlink() or path.exists():
                    path.unlink()
            except OSError:
                pass
        raise


def _model_inputs(request: StageRequest) -> dict:
    return {
        RUNTIME_MODEL: request.runtime_model,
        ADMIN_MODEL: request.admin_model,
        VAD_MODEL: request.vad_model,
        **{f"{STT_DIR}/{name}": request.stt_dir / name for name in STT_FILES},
    }


def _path_bytes(path: Path, *, release_root: Optional[Path] = None) -> int:
    if path.is_symlink():
        if release_root is not None:
            wake_reference(release_root, path)
            return 0
        raise ReleaseError(f"refusing to size a symlink: {path}")
    if path.is_file():
        return path.stat().st_size
    if path.is_dir():
        total = 0
        for item in path.rglob("*"):
            if item.is_symlink():
                if release_root is not None:
                    wake_reference(release_root, item)
                    continue
                raise ReleaseError(f"refusing to size a symlink: {item}")
            if item.is_file():
                total += item.stat().st_size
        return total
    raise ReleaseError(f"cannot size {path}")


def _require_space(anchor: Path, needed: int) -> None:
    probe = anchor if anchor.exists() else anchor.parent
    if not probe.exists():
        raise ReleaseError(f"cannot check free space for {anchor}")
    free = shutil.disk_usage(probe).free
    if free < needed + DISK_MARGIN_BYTES:
        raise ReleaseError(
            f"need {needed + DISK_MARGIN_BYTES} bytes free at {probe}, have {free}. "
            "Refusing to start a copy that can fill the disk.")


def _reused_models(request: StageRequest, destination: Path) -> dict:
    """Models already stored with the same bytes. They are not copied again."""
    reused = {}
    if not destination.exists():
        return reused
    for relative, source in _model_inputs(request).items():
        target = model_path(destination, relative)
        if not target.exists() and not target.is_symlink():
            continue
        if target.is_symlink() or not target.is_file() or sha256_file(target) != sha256_file(source):
            raise ReleaseError(f"refusing to overwrite existing {target}")
        reused[relative] = target
    return reused


def stage_release(request: StageRequest) -> dict:
    destination = assert_stage_destination(request.destination, request.source_root)
    request.destination = destination
    identity = source_identity(request.source_root)
    if request.commit and request.commit != identity["commit"]:
        raise ReleaseError("commit does not match HEAD")
    request.commit = identity["commit"]
    request.dirty = identity["dirty"]
    if request.production and request.dirty:
        raise ReleaseError(
            "production staging requires a clean committed source tree; "
            f"HEAD {request.commit} does not describe this uncommitted worktree")
    identity_binary = build_identity(request.build_dir / "firefly-demo")
    if build_identity(request.build_dir / "firefly-config") != identity_binary:
        raise ReleaseError("firefly-config must be rebuilt with the same build identity as firefly-demo")
    source_hash = source_tree_fingerprint(request.source_root)
    request.build_head = identity_binary["head"]
    request.build_source = identity_binary["source_sha256"]
    request.build_time = identity_binary["built_utc"]
    request.build_matches = (
        not request.dirty and identity_binary.get("dirty") is False and
        request.build_head == request.commit and request.build_source == source_hash and
        identity_binary.get("admin_assets_sha256") == admin_assets_fingerprint(request.build_dir))
    if request.offline_candidate:
        if (request.production or request.reuse_installed_models or request.wake_reference_release
                or request.runtime_prefix != PRODUCTION_PREFIX or not request.build_matches
                or not mqtt_enabled(request.build_dir)):
            raise ReleaseError("offline candidate requires clean matching MQTT build and fixed runtime prefix; no production/reuse flags")
    if request.production and not request.build_matches:
        raise ReleaseError(
            "production staging requires a fresh firefly-demo built from this clean commit; "
            f"binary head {request.build_head} source {request.build_source} "
            f"does not match worktree {request.commit} {source_hash}")
    if not COMMIT_RE.fullmatch(request.commit):
        raise ReleaseError("commit must be a hex sha")
    request.release_id = request.release_id or f"m18.6-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-{request.commit[:12]}"
    if not RELEASE_ID_RE.fullmatch(request.release_id):
        raise ReleaseError("release id must be a single safe path segment")
    request.runtime_prefix = request.runtime_prefix or str(destination)
    if not request.runtime_prefix.startswith("/"):
        raise ReleaseError("runtime prefix must be absolute")
    if os.uname().machine not in {"aarch64", "arm64"}:
        raise ReleaseError("staging requires aarch64")
    _preflight(request)
    assert_model_tree(destination)
    if request.offline_candidate:
        reused = {}
    elif request.reuse_installed_models:
        if (not request.production or request.runtime_prefix != PRODUCTION_PREFIX
                or not RELEASE_ID_RE.fullmatch(request.wake_reference_release)):
            raise ReleaseError("installed model reuse requires production prefix and an immutable wake release")
        reused = installed_model_paths()
        if _model_inputs(request) != reused:
            raise ReleaseError("reuse inputs must be the exact installed model paths")
    else:
        if request.wake_reference_release:
            raise ReleaseError("wake reference requires --reuse-installed-models")
        reused = _reused_models(request, destination)
    target = assert_real_path(destination / "releases" / request.release_id)
    if target.exists():
        raise ReleaseError(f"release {request.release_id} already exists and will not be modified")
    needed = _path_bytes(request.build_dir / "firefly-demo")
    needed += _path_bytes(request.build_dir / "firefly-config")
    needed += _path_bytes(request.rkllm_library) + _path_bytes(request.sherpa_library)
    needed += _path_bytes(request.onnx_library) + _path_bytes(request.wake_site_packages)
    for relative, source in _model_inputs(request).items():
        if not request.offline_candidate and relative not in reused:
            needed += _path_bytes(source)
    _require_space(destination if destination.exists() else destination.parent, needed)
    destination.mkdir(mode=0o755, parents=True, exist_ok=True)
    current_before = os.readlink(destination / "current") if (destination / "current").is_symlink() else None
    if (destination / "current").exists() and not (destination / "current").is_symlink():
        raise ReleaseError("existing current entry must stay a relative symlink")
    lock = _lock_prefix(destination)
    work = None
    published_release: Optional[Path] = None
    created_models: list = []
    try:
        work = Path(tempfile.mkdtemp(prefix=f".staging-{request.release_id}-", dir=destination))
        prefix = work / "prefix"
        release = prefix / "releases" / request.release_id
        release.mkdir(parents=True)
        subprocess.check_call(
            ["cmake", "--install", str(request.build_dir), "--prefix", str(release), "--component", "firefly-runtime"])
        wake = release / WAKE_MODEL
        if request.reuse_installed_models:
            # The build must also avoid copying Amina; do not silently remove a copy.
            if wake.exists() or wake.is_symlink():
                raise ReleaseError("configure FIREFLY_WAKE_MODEL_REFERENCE before no-copy production staging")
            reference = Path(PRODUCTION_PREFIX) / "releases" / request.wake_reference_release / WAKE_MODEL
            wake.symlink_to(reference)
            wake_reference(release, wake)
        elif not wake.exists():
            # Legacy/fixture staging from a reference-configured build remains relocatable.
            _copy_regular(request.source_root / "assets/wake/amina-v3.tflite", wake)
        binary = release / "bin/firefly-demo"
        build_binary = request.build_dir / "firefly-demo"
        if not binary.is_file():
            raise ReleaseError("cmake --install did not install bin/firefly-demo")
        if build_binary.exists() and os.path.samefile(binary, build_binary):
            raise ReleaseError("refusing to strip the build-tree firefly-demo")
        subprocess.check_call(["strip", "--strip-unneeded", str(binary)])
        lib = release / "lib"
        lib.mkdir()
        for name, source in (
            ("librkllmrt.so", request.rkllm_library),
            ("libsherpa-onnx-c-api.so", request.sherpa_library),
            ("libonnxruntime.so", request.onnx_library),
        ):
            _copy_regular(source, lib / name)
        if mqtt_enabled(request.build_dir):
            from .mqtt_release import stage
            stage(request, release)
        model_paths = {}
        for relative, source in _model_inputs(request).items():
            if request.offline_candidate:
                continue
            if relative in reused:
                model_paths[relative] = reused[relative]
                continue
            destination_path = prefix / relative
            _copy_regular(source, destination_path)
            model_paths[relative] = destination_path
        site = release / "share/firefly/wake/python/lib/python3.10/site-packages"
        site.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(request.wake_site_packages, site, symlinks=False, ignore=_ignore_site)
        for required in ("numpy", "ai_edge_litert", "pymicro_features", "micro_features_cpp.abi3.so", "numpy.libs"):
            if not (site / required).exists():
                raise ReleaseError(f"wake site-packages is missing {required}")
        doc = release / "share/firefly/doc"
        doc.mkdir(parents=True)
        for name in ("m18-relocatable-production-packaging.md", "m18-owner-acceptance-runbook.md"):
            source = request.source_root / "docs" / name
            require_regular_file(source, name)
            shutil.copyfile(source, doc / name)
        unit = release / "share/firefly/systemd/firefly-voice-assistant.service"
        unit.parent.mkdir(parents=True)
        unit.write_text(render_unit(request.runtime_prefix))
        _purge_pycache(release)
        for directory, _dirs, _files in os.walk(release, topdown=False):
            path = Path(directory)
            if path != release and not any(path.iterdir()):
                path.rmdir()
        _chmod_tree(release)
        if (prefix / "models").exists():
            _chmod_tree(prefix / "models")
        check_elf(binary)
        python_version = check_wake(release)
        _purge_pycache(release)
        document = _manifest_document(request, release, model_paths, python_version)
        _write_manifest(release, document)
        verify_tree(prefix, request.release_id, check_python=True, model_paths=model_paths,
                    candidate_only=request.offline_candidate)
        if request.production:
            require_production_identity(document, release)
            final_source = source_identity(request.source_root)
            if (final_source["dirty"] or final_source["commit"] != request.commit or
                    source_tree_fingerprint(request.source_root) != request.build_source or
                    admin_assets_fingerprint(request.build_dir) != document["build"]["admin_assets_sha256"]):
                raise ReleaseError("production build inputs changed during staging; no release was published")
        if not request.reuse_installed_models and not request.offline_candidate:
            created_models = publish_shared_models(prefix, destination)
        target = destination / "releases" / request.release_id
        if target.exists():
            raise ReleaseError(f"release {request.release_id} appeared during staging")
        target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        _publish_directory(release, target)
        published_release = target
        verify_tree(destination, request.release_id, check_python=False, candidate_only=request.offline_candidate)
        current_after = os.readlink(destination / "current") if (destination / "current").is_symlink() else None
        if current_after != current_before or ((destination / "current").exists() and not (destination / "current").is_symlink()):
            raise ReleaseError("staging changed the current symlink")
        published_release = None
        created_models = []
        return load_manifest(target)
    except Exception as error:
        for path in created_models:
            try:
                if path.is_file() and not path.is_symlink():
                    path.unlink()
            except OSError:
                pass
        if published_release is not None and published_release.exists():
            shutil.rmtree(published_release)
        if isinstance(error, ReleaseError):
            raise
        if isinstance(error, OSError) and error.errno == errno.ENOSPC:
            raise ReleaseError("copy stopped because the filesystem is full; partial files were removed") from error
        raise ReleaseError(str(error)) from error
    finally:
        if work is not None and work.exists():
            shutil.rmtree(work)
        if lock is not None:
            os.close(lock)


def mqtt_enabled(build_dir: Path) -> bool:
    return bool(re.search(r"^FIREFLY_ENABLE_MQTT:BOOL=ON$", (build_dir / "CMakeCache.txt").read_text(), re.MULTILINE))


def _preflight(request: StageRequest) -> None:
    build_binary = request.build_dir / "firefly-demo"
    cache = request.build_dir / "CMakeCache.txt"
    require_regular_file(build_binary, "build firefly-demo")
    require_regular_file(cache, "CMakeCache.txt")
    for label, path in (
        ("rkllm library", request.rkllm_library),
        ("sherpa library", request.sherpa_library),
        ("onnx runtime", request.onnx_library),
    ):
        require_regular_file(path, label)
    if not request.offline_candidate:
        for path in (request.runtime_model, request.admin_model, request.vad_model):
            require_regular_file(path, "model input")
        if request.stt_dir.is_symlink() or not request.stt_dir.is_dir():
            raise ReleaseError("STT model directory must be a real directory")
        for name in STT_FILES:
            require_regular_file(request.stt_dir / name, f"STT {name}")
    if request.wake_site_packages.is_symlink() or not request.wake_site_packages.is_dir():
        raise ReleaseError("wake site-packages must be a real directory")
    for name in ("docs/m18-relocatable-production-packaging.md", "docs/m18-owner-acceptance-runbook.md"):
        require_regular_file(request.source_root / name, name)
    if request.production or request.offline_candidate:
        _expect(request.rkllm_library, PRODUCTION_SHA256["lib/librkllmrt.so"], None)
        _expect(request.sherpa_library, PRODUCTION_SHA256["lib/libsherpa-onnx-c-api.so"], None)
        _expect(request.onnx_library, PRODUCTION_SHA256["lib/libonnxruntime.so"], None)
    if request.production:
        _expect(request.runtime_model, PRODUCTION_SHA256[RUNTIME_MODEL], None)
        _expect(request.admin_model, PRODUCTION_SHA256[ADMIN_MODEL], ADMIN_MODEL_BYTES)
        _expect(request.vad_model, PRODUCTION_SHA256[VAD_MODEL], None)
        for name in STT_FILES:
            _expect(request.stt_dir / name, PRODUCTION_SHA256[f"{STT_DIR}/{name}"], None)
    elif not request.offline_candidate and request.admin_model.stat().st_size == ADMIN_MODEL_BYTES:
        # Fixture mode must not silently accept the real multi-gigabyte model without production checks.
        raise ReleaseError("admin model has the production size; rerun with --production")


def _expect(path: Path, digest: str, size: Optional[int]) -> None:
    if size is not None and path.stat().st_size != size:
        raise ReleaseError(f"{path.name} size {path.stat().st_size} != {size}")
    actual = sha256_file(path)
    if actual != digest:
        raise ReleaseError(f"{path.name} sha256 {actual} != {digest}")


def _current_id(prefix: Path) -> Optional[str]:
    link = prefix / "current"
    if not link.exists() and not link.is_symlink():
        return None
    if not link.is_symlink():
        raise ReleaseError("current must be a relative symlink to releases/<id>")
    target = Path(os.readlink(link))
    if target.is_absolute() or target.parent != Path("releases") or not RELEASE_ID_RE.fullmatch(target.name):
        raise ReleaseError(f"current symlink target is not releases/<id>: {target}")
    return target.name


def _switch_current(prefix: Path, release_id: str) -> None:
    with tempfile.TemporaryDirectory(prefix=".activation-", dir=prefix) as temp:
        temporary = Path(temp) / "current"
        temporary.symlink_to(Path("releases") / release_id)
        os.replace(temporary, prefix / "current")


def _write_previous(prefix: Path, release_id: str) -> None:
    target = prefix / "previous-release"
    fd, name = tempfile.mkstemp(prefix=".previous-", dir=prefix)
    partial = Path(name)
    try:
        with os.fdopen(fd, "w") as output:
            output.write(release_id + "\n")
            output.flush()
            os.fsync(output.fileno())
            os.fchmod(output.fileno(), 0o644)
        os.replace(partial, target)
    finally:
        if partial.exists():
            partial.unlink()


def _restore_current(prefix: Path, previous_id: Optional[str]) -> None:
    link = prefix / "current"
    if previous_id is None:
        if link.is_symlink() or link.exists():
            link.unlink()
        return
    _switch_current(prefix, previous_id)


def activate(destination: Path, release_id: str, store: Path, *, fresh: bool = False, config_tool: Optional[Path] = None) -> str:
    prefix = assert_activation_destination(destination)
    lock = _lock_prefix(prefix)
    try:
        return _activate_locked(prefix, release_id, store, fresh=fresh, config_tool=config_tool)
    finally:
        os.close(lock)


def _activate_locked(prefix: Path, release_id: str, store: Path, *, fresh: bool, config_tool: Optional[Path]) -> str:
    if not RELEASE_ID_RE.fullmatch(release_id):
        raise ReleaseError("invalid release id")
    if is_production_prefix(prefix):
        require_production_store(store, fresh=fresh)
    document = verify_tree(prefix, release_id, check_python=True)
    shipped_tool = prefix / "releases" / release_id / "bin/firefly-config"
    if is_production_prefix(prefix) and config_tool is not None and config_tool.resolve() != shipped_tool.resolve():
        raise ReleaseError("production activation must use the target release's firefly-config")
    config_tool = config_tool or shipped_tool
    require_store(store, document["config_schema"]["readable"], fresh=fresh, config_tool=config_tool)
    previous = _current_id(prefix)
    if previous == release_id:
        return (
            f"activated {release_id}\n"
            "previous=unchanged\n"
            "service restart was not run\n"
            "ConfigStore was not modified\n"
            "systemd unit was not installed\n"
        )
    _switch_current(prefix, release_id)
    try:
        if previous:
            _write_previous(prefix, previous)
    except Exception as error:
        try:
            _restore_current(prefix, previous)
        except Exception as revert_error:
            raise ReleaseError(
                f"activation switched current to {release_id} and could not restore {previous}: {revert_error}"
            ) from error
        raise ReleaseError("activation marker update failed; current was restored") from error
    return (
        f"activated {release_id}\n"
        f"previous={previous or 'none'}\n"
        "service restart was not run\n"
        "ConfigStore was not modified\n"
        "systemd unit was not installed\n"
    )


def rollback(destination: Path, store: Path, *, config_tool: Optional[Path] = None) -> str:
    prefix = assert_activation_destination(destination)
    lock = _lock_prefix(prefix)
    try:
        return _rollback_locked(prefix, store, config_tool=config_tool)
    finally:
        os.close(lock)


def _rollback_locked(prefix: Path, store: Path, *, config_tool: Optional[Path]) -> str:
    if is_production_prefix(prefix):
        require_production_store(store)
    marker = prefix / "previous-release"
    if not marker.is_file() or marker.is_symlink():
        raise ReleaseError("no previous release is recorded; current was left unchanged")
    try:
        previous = marker.read_text().strip()
    except OSError as error:
        raise ReleaseError(f"previous-release is unreadable; current was left unchanged: {error.strerror}") from error
    if not RELEASE_ID_RE.fullmatch(previous):
        raise ReleaseError("previous-release is invalid; current was left unchanged")
    current = _current_id(prefix)
    if current is None:
        raise ReleaseError("current symlink is missing; rollback made no change")
    document = verify_tree(prefix, previous, check_python=True)
    shipped_tool = prefix / "releases" / previous / "bin/firefly-config"
    if is_production_prefix(prefix) and config_tool is not None and config_tool.resolve() != shipped_tool.resolve():
        raise ReleaseError("production rollback must use the target release's firefly-config")
    config_tool = config_tool or shipped_tool
    require_store(store, document["config_schema"]["readable"], fresh=False, config_tool=config_tool)
    if current == previous:
        return (
            f"rolled back to {previous}\n"
            "service restart was not run\n"
            "ConfigStore was not modified\n"
        )
    _switch_current(prefix, previous)
    try:
        _write_previous(prefix, current)
    except Exception as error:
        try:
            _switch_current(prefix, current)
        except Exception as revert_error:
            raise ReleaseError(
                f"rollback switched current to {previous} and could not restore {current}: {revert_error}"
            ) from error
        raise ReleaseError("rollback marker update failed; current was restored") from error
    return (
        f"rolled back to {previous}\n"
        f"left={current}\n"
        "service restart was not run\n"
        "ConfigStore was not modified\n"
    )


def assess_post_start(status: dict, url: str) -> list[str]:
    """Judge a captured loopback /api/status document.

    This does not connect, and it does not prove which executable or PID produced
    the document. The owner checks process identity separately.
    """
    errors = []
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "http" or parsed.hostname != "127.0.0.1" or parsed.port != 8080 or parsed.path != "/api/status":
        errors.append("health URL must be http://127.0.0.1:8080/api/status")
    if status.get("ok") is not True:
        errors.append("status.ok is not true")
    if status.get("runtime_ready") is not True:
        errors.append("runtime_ready is not true")
    if status.get("stopping") is not False:
        errors.append("status.stopping is not false")
    if status.get("stt_ready") is not True:
        errors.append("stt_ready is not true")
    if status.get("microphone") != "connected":
        errors.append("microphone is not connected")
    if status.get("voice") != "WAITING_WAKE":
        errors.append("voice is not WAITING_WAKE")
    health = status.get("health")
    if not isinstance(health, dict):
        return errors + ["missing health object"]
    if health.get("overall") != "ready":
        errors.append(f"health.overall is {health.get('overall')}, expected ready")
    if health.get("stopping") is not False:
        errors.append("health.stopping is not false")
    subsystems = health.get("subsystems") if isinstance(health.get("subsystems"), dict) else {}
    for name in ("admin", "configuration", "runtime", "llm", "stt", "wake"):
        entry = subsystems.get(name)
        if not isinstance(entry, dict) or entry.get("status") != "ready":
            errors.append(f"{name} is not ready")
    return errors


def classify_systemd_analyze(output: str) -> dict:
    """Separate a missing install path from unit defects and foreign-unit noise."""
    expected_absent = []
    foreign = []
    defects = []
    for line in output.splitlines():
        if not line.strip():
            continue
        if ("firefly-voice-assistant.service: Command " in line and
                "is not executable: No such file or directory" in line):
            expected_absent.append(line)
        elif "firefly-voice-assistant.service" in line:
            defects.append(line)
        elif "netplan-ovs-cleanup.service" in line or "rkaiq_3A.service" in line:
            foreign.append(line)
        else:
            defects.append(line)
    return {"expected_absent_install": expected_absent, "foreign": foreign, "defects": defects}


def _assert_not_live_unit(destination: Path) -> None:
    _assert_unit_destination(destination)


def _assert_unit_destination(destination: Path, *, backup: bool = False) -> None:
    resolved = assert_real_path(destination)
    approved = Path(PRODUCTION_PREFIX) / "systemd-backup" if backup else LIVE_UNIT
    if resolved == approved:
        if not _owner_accepted():
            raise ReleaseError("refusing production unit or backup changes without FIREFLY_OWNER_ACCEPTED=1")
        return
    for root in (Path("/opt"), Path("/etc"), Path("/usr"), Path("/lib"), Path("/bin"), Path("/var"),
                 PRODUCTION_REPO, VENDOR_SDK):
        if resolved == root or _within(resolved, root):
            raise ReleaseError(f"refusing unit or backup writes under {resolved}")


def install_release(source_prefix: Path, destination: Path, release_id: str) -> None:
    """Copy one verified release into an existing prefix. Never deletes that prefix or current."""
    source_prefix = assert_real_path(source_prefix)
    destination = assert_activation_destination(destination)
    if destination == source_prefix:
        raise ReleaseError("install destination must be a different prefix")
    if not RELEASE_ID_RE.fullmatch(release_id):
        raise ReleaseError("invalid release id")
    verify_tree(source_prefix, release_id, check_python=True)
    target = assert_real_path(destination / "releases" / release_id)
    if target.exists() or target.is_symlink():
        raise ReleaseError(f"refusing to overwrite release {release_id}")
    current_before = os.readlink(destination / "current") if (destination / "current").is_symlink() else None
    if (destination / "current").exists() and not (destination / "current").is_symlink():
        raise ReleaseError("existing current entry must stay a relative symlink")
    marker = destination / "previous-release"
    marker_before = marker.read_text() if marker.is_file() and not marker.is_symlink() else None
    document = load_manifest(source_prefix / "releases" / release_id)
    assert_production_runtime_prefix(document.get("runtime_prefix", ""), destination)
    if is_production_prefix(destination):
        require_production_identity(document, source_prefix / "releases" / release_id)
    model_names = assert_shared_model_manifest(document.get("shared_models"))
    sources = manifest_model_paths(document, source_prefix)
    external = document.get("installed_model_reference") is not None
    if external and destination != Path(PRODUCTION_PREFIX):
        raise ReleaseError("installed model reference can only be installed into the fixed production prefix")
    assert_model_tree(destination)
    needed = _path_bytes(source_prefix / "releases" / release_id,
                         release_root=source_prefix / "releases" / release_id if external else None)
    for relative in model_names:
        existing = model_path(destination, relative)
        source = sources[relative]
        if existing.exists() or existing.is_symlink():
            if existing.is_symlink() or not existing.is_file() or sha256_file(existing) != sha256_file(source):
                raise ReleaseError(f"refusing to overwrite existing {existing}")
            continue
        if external:
            raise ReleaseError("installed shared model disappeared; refusing a replacement copy")
        needed += source.stat().st_size
    anchor = destination if destination.exists() else destination.parent
    _require_space(anchor, needed)
    destination.mkdir(mode=0o755, parents=True, exist_ok=True)
    lock = _lock_prefix(destination)
    incoming = None
    published_release = None
    created_models: list = []
    try:
        incoming = Path(tempfile.mkdtemp(prefix=f".incoming-{release_id}-", dir=destination))
        release_copy = incoming / "releases" / release_id
        shutil.copytree(source_prefix / "releases" / release_id, release_copy, symlinks=external)
        for relative in model_names:
            existing = model_path(destination, relative)
            if existing.is_file() and not existing.is_symlink():
                continue
            if external:
                raise ReleaseError("installed shared model disappeared during install")
            _copy_regular(sources[relative], model_path(incoming, relative))
        (destination / "releases").mkdir(mode=0o755, parents=True, exist_ok=True)
        _publish_directory(release_copy, target)
        published_release = target
        if not external:
            created_models = publish_shared_models(incoming, destination)
        if install_after_models is not None:
            install_after_models(created_models)
        verify_tree(destination, release_id, check_python=False)
        current_after = os.readlink(destination / "current") if (destination / "current").is_symlink() else None
        marker_after = marker.read_text() if marker.is_file() and not marker.is_symlink() else None
        if current_after != current_before or marker_after != marker_before:
            raise ReleaseError("install changed current or previous-release")
        published_release = None
        created_models = []
    except Exception as error:
        for path in created_models:
            try:
                if path.is_file() and not path.is_symlink():
                    path.unlink()
            except OSError:
                pass
        if published_release is not None and published_release.exists():
            shutil.rmtree(published_release)
        if isinstance(error, ReleaseError):
            raise
        if isinstance(error, OSError) and error.errno == errno.ENOSPC:
            raise ReleaseError("install copy stopped because the filesystem is full; the prefix was left unchanged") from error
        raise ReleaseError(str(error)) from error
    finally:
        if incoming is not None and incoming.exists():
            shutil.rmtree(incoming)
        if lock is not None:
            os.close(lock)


def install_systemd_unit(source: Path, destination: Path, backup_dir: Path) -> Optional[Path]:
    """Back up an existing unit, then atomically replace it. Does not restart systemd."""
    _assert_not_live_unit(destination)
    assert_real_path(destination)
    _assert_unit_destination(backup_dir, backup=True)
    require_regular_file(source, "unit source")
    if destination.resolve() == source.resolve():
        raise ReleaseError("unit destination must differ from the source")
    backup_dir.mkdir(parents=True, exist_ok=True)
    info = backup_dir.stat()
    if info.st_uid != os.geteuid() or info.st_mode & 0o022:
        raise ReleaseError("unit backup directory must be owned by the invoking user without group/other write")
    destination.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_file():
            raise ReleaseError("existing unit must be a regular file")
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        backup = backup_dir / f"firefly-voice-assistant.service.{stamp}"
        suffix = 0
        while backup.exists():
            suffix += 1
            backup = backup_dir / f"firefly-voice-assistant.service.{stamp}.{suffix}"
        with tempfile.TemporaryDirectory(prefix=".unit-backup-", dir=backup_dir) as temporary:
            snapshot = Path(temporary) / "unit"
            _copy_regular(destination, snapshot)
            # A simultaneous installer must not replace another rollback archive.
            os.link(snapshot, backup)
    _copy_regular(source, destination)
    return backup


def restore_systemd_unit(backup: Path, destination: Path) -> None:
    """Atomically put a saved unit back. Does not delete the install prefix or restart systemd."""
    _assert_not_live_unit(destination)
    assert_real_path(destination)
    require_regular_file(backup, "unit backup")
    destination.parent.mkdir(parents=True, exist_ok=True)
    _copy_regular(backup, destination)


def unit_paths_exist(prefix: Path, release_id: str) -> None:
    release = prefix / "releases" / release_id
    text = (release / "share/firefly/systemd/firefly-voice-assistant.service").read_text()
    arguments = unit_arguments(text)
    binary = Path(arguments.pop("__binary__"))
    if not binary.is_file():
        raise ReleaseError(f"unit binary does not exist: {binary}")
    for flag, value in arguments.items():
        if flag == "--voice" or not isinstance(value, str) or not value.startswith("/"):
            continue
        if flag in {"--audio-device", "--playback-device", "--store"}:
            continue
        path = Path(value)
        if flag == "--stt-model-dir":
            if not path.is_dir():
                raise ReleaseError(f"{flag} does not exist: {path}")
        elif not path.is_file():
            raise ReleaseError(f"{flag} does not exist: {path}")


def _request_from_args(args: argparse.Namespace) -> StageRequest:
    return StageRequest(
        destination=Path(args.destination),
        build_dir=Path(args.build_dir),
        source_root=Path(args.source_root),
        rkllm_library=Path(args.rkllm_library),
        sherpa_library=Path(args.sherpa_library),
        onnx_library=Path(args.onnx_library),
        runtime_model=Path(args.runtime_model),
        admin_model=Path(args.admin_model),
        stt_dir=Path(args.stt_dir),
        vad_model=Path(args.vad_model),
        wake_site_packages=Path(args.wake_site_packages),
        release_id=args.release_id or "",
        commit=args.commit or "",
        runtime_prefix=args.runtime_prefix or "",
        production=args.production,
        reuse_installed_models=args.reuse_installed_models,
        wake_reference_release=args.wake_reference_release,
        offline_candidate=args.offline_candidate,
        mqtt_root=Path(args.mqtt_root) if args.mqtt_root else None,
        tls_root=Path(args.tls_root) if args.tls_root else None,
        mqtt_provenance=Path(args.mqtt_provenance) if args.mqtt_provenance else None,
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Stage a relocatable Firefly release without activating it")
    sub = parser.add_subparsers(dest="command", required=True)
    stage = sub.add_parser("stage")
    for name in (
        "destination", "build-dir", "source-root", "rkllm-library", "sherpa-library", "onnx-library",
        "wake-site-packages",
    ):
        stage.add_argument("--" + name, required=True)
    for name in ("runtime-model", "admin-model", "stt-dir", "vad-model"):
        stage.add_argument("--" + name, default="")
    stage.add_argument("--release-id", default="")
    stage.add_argument("--commit", default="")
    stage.add_argument("--runtime-prefix", default="")
    stage.add_argument("--production", action="store_true")
    stage.add_argument("--reuse-installed-models", action="store_true")
    stage.add_argument("--wake-reference-release", default="")
    stage.add_argument("--offline-candidate", action="store_true")
    for name in ("mqtt-root", "tls-root", "mqtt-provenance"):
        stage.add_argument("--" + name, default="")
    verify = sub.add_parser("verify")
    verify.add_argument("--destination", required=True)
    verify.add_argument("--release-id", required=True)
    candidate = sub.add_parser("verify-candidate")
    candidate.add_argument("--destination", required=True)
    candidate.add_argument("--release-id", required=True)
    final = sub.add_parser("finalize-candidate")
    final.add_argument("--source", required=True)
    final.add_argument("--destination", required=True)
    final.add_argument("--release-id", required=True)
    activate_parser = sub.add_parser("activate")
    activate_parser.add_argument("--destination", required=True)
    activate_parser.add_argument("--release-id", required=True)
    activate_parser.add_argument("--store", required=True)
    activate_parser.add_argument("--config-tool")
    activate_parser.add_argument("--fresh", action="store_true")
    rollback_parser = sub.add_parser("rollback")
    rollback_parser.add_argument("--destination", required=True)
    rollback_parser.add_argument("--store", required=True)
    rollback_parser.add_argument("--config-tool")
    install = sub.add_parser("install-release")
    install.add_argument("--source", required=True)
    install.add_argument("--destination", required=True)
    install.add_argument("--release-id", required=True)
    install_unit = sub.add_parser("install-unit")
    install_unit.add_argument("--source", required=True)
    install_unit.add_argument("--destination", required=True)
    install_unit.add_argument("--backup-dir", required=True)
    restore_unit = sub.add_parser("restore-unit")
    restore_unit.add_argument("--backup", required=True)
    restore_unit.add_argument("--destination", required=True)
    unit = sub.add_parser("render-unit")
    unit.add_argument("--runtime-prefix", default=PRODUCTION_PREFIX)
    health = sub.add_parser("health-gate")
    health.add_argument("--url", required=True)
    health.add_argument("--status-file", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "stage":
            document = stage_release(_request_from_args(args))
            print(document["release_id"])
        elif args.command == "verify":
            verify_tree(Path(args.destination), args.release_id, check_python=True)
            print("verified")
        elif args.command == "verify-candidate":
            verify_tree(Path(args.destination), args.release_id, candidate_only=True)
            print("candidate verified; OWNER_ACTIVATION_REQUIRED")
        elif args.command == "finalize-candidate":
            from .mqtt_release import finalize_candidate
            finalize_candidate(Path(args.source), Path(args.destination), args.release_id)
            print("private production stage prepared; NOT installed or activated")
        elif args.command == "activate":
            print(activate(Path(args.destination), args.release_id, Path(args.store),
                           fresh=args.fresh, config_tool=Path(args.config_tool) if args.config_tool else None), end="")
        elif args.command == "rollback":
            print(rollback(Path(args.destination), Path(args.store),
                           config_tool=Path(args.config_tool) if args.config_tool else None), end="")
        elif args.command == "install-release":
            install_release(Path(args.source), Path(args.destination), args.release_id)
            print("installed")
        elif args.command == "install-unit":
            backup = install_systemd_unit(Path(args.source), Path(args.destination), Path(args.backup_dir))
            print(backup or "no-previous-unit")
        elif args.command == "restore-unit":
            restore_systemd_unit(Path(args.backup), Path(args.destination))
            print("restored")
        elif args.command == "render-unit":
            sys.stdout.write(render_unit(args.runtime_prefix))
        elif args.command == "health-gate":
            status = json.loads(Path(args.status_file).read_text())
            errors = assess_post_start(status, args.url)
            if errors:
                print("\n".join(errors), file=sys.stderr)
                return 1
            print("health-gate-pass")
    except ReleaseError as error:
        print(f"firefly-release: {error}", file=sys.stderr)
        return 1
    return 0
