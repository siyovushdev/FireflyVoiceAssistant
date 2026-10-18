"""MQTT component of the existing immutable release pipeline.

No downloader, installer, service operation or device command belongs here.
Offline candidates pin absent shared models and cannot pass activation gates.
"""
from pathlib import Path
import json
import ctypes
import os
import re
import shutil
import subprocess
import tempfile

from . import firefly_release as r

PROGRAMS = ("firefly-admin", "firefly-admin-runtime", "firefly-mqtt-probe-worker", "firefly-mqtt-publish-worker")
LIBRARIES = ("libmosquitto.so.1", "libssl.so.3", "libcrypto.so.3")
BASE = "share/firefly/mqtt/"
IMAGE = BASE + "image/"
UNITS = ("firefly-mqtt-broker.service", "firefly-mqtt-control.service", "firefly-mqtt-control.socket")
EXTRA_FILES = {
    *("bin/" + name for name in PROGRAMS), *("lib/" + name for name in LIBRARIES),
    "share/firefly/deployment/packaging/mqtt_release.py",
    "share/firefly/doc/m20-arm64-release-runbook.md", BASE + "provenance.json",
    BASE + "licenses/LICENSE-EDL-1.0.txt", BASE + "licenses/LICENSE-EPL-2.0.txt",
    BASE + "licenses/LICENSE.txt", BASE + "licenses/NOTICE.md",
    BASE + "licenses/openssl-copyright.txt", IMAGE + "PROVISIONING.txt",
    IMAGE + "usr/sbin/mosquitto", IMAGE + "usr/bin/mosquitto_passwd",
    IMAGE + "usr/libexec/firefly-mqtt-control-helper",
    *(IMAGE + "usr/lib/firefly-mqtt/" + name for name in LIBRARIES[1:]),
    *(IMAGE + "etc/systemd/system/" + name for name in UNITS),
    IMAGE + "etc/firefly/mqtt/mosquitto.conf.template",
}
MODEL_SIZES = dict(zip(r.ALLOWED_SHARED_MODELS,
    (4840064228, 4848176220, 643854, 68187180, 2093079, 259572, 6388)))
SYSTEM_LIBS = {"libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libpthread.so.0",
               "libdl.so.2", "librt.so.1", "libasound.so.2", "libgomp.so.1", "ld-linux-aarch64.so.1"}


def enabled(build_dir):
    return bool(re.search(r"^FIREFLY_ENABLE_MQTT:BOOL=ON$", (build_dir / "CMakeCache.txt").read_text(), re.M))


def model_references():
    return [{"path": name, "sha256": r.PRODUCTION_SHA256[name], "bytes": MODEL_SIZES[name],
             "shared": True, "distribution": "same-device-only" if name in r.SAME_DEVICE_PATHS else "device-local"}
            for name in r.ALLOWED_SHARED_MODELS]


def assert_candidate(document):
    if (document.get("offline_candidate") != {"state": "OWNER_ACTIVATION_REQUIRED", "models": "frozen-target-references"}
            or document.get("production_identity_checked") is not False
            or document.get("runtime_prefix") != r.PRODUCTION_PREFIX
            or document.get("source", {}).get("identity") != "committed-offline-candidate"
            or document.get("source", {}).get("dirty") is not False
            or document.get("build", {}).get("matches_worktree") is not True
            or document.get("build", {}).get("dirty") is not False
            or document.get("source", {}).get("commit") != document.get("build", {}).get("head")
            or not re.fullmatch(r"[0-9a-f]{40}", document.get("build", {}).get("head", ""))
            or document.get("shared_models") != model_references()
            or document.get("installed_model_reference") is not None
            or document.get("wake_model_reference") is not None
            or document.get("mqtt", {}).get("enabled") is not True):
        raise r.ReleaseError("invalid offline candidate; frozen target model verification is still required")


def stage(request, release):
    if not (request.mqtt_root and request.tls_root and request.mqtt_provenance):
        raise r.ReleaseError("MQTT packaging requires offline --mqtt-root, --tls-root and --mqtt-provenance")
    mqtt, tls = request.mqtt_root, request.tls_root
    provenance = json.loads(request.mqtt_provenance.read_text())
    if provenance.get("mosquitto_version") != "2.0.18" or provenance.get("tls_enabled") is not True:
        raise r.ReleaseError("expected pinned TLS-enabled Mosquitto2.0.18 provenance")
    sources = {"lib/libmosquitto.so.1": mqtt / "lib/libmosquitto.so.2.0.18",
               "lib/libssl.so.3": tls / "usr/lib/aarch64-linux-gnu/libssl.so.3",
               "lib/libcrypto.so.3": tls / "usr/lib/aarch64-linux-gnu/libcrypto.so.3"}
    for name, source in list(sources.items()):
        if name.endswith(tuple(LIBRARIES[1:])):
            sources[IMAGE + "usr/lib/firefly-mqtt/" + source.name] = source
    sources.update({IMAGE + "usr/sbin/mosquitto": mqtt / "sbin/mosquitto",
                    IMAGE + "usr/bin/mosquitto_passwd": mqtt / "bin/mosquitto_passwd",
                    IMAGE + "usr/libexec/firefly-mqtt-control-helper": request.build_dir / "src/admin/firefly-mqtt-control-helper",
                    BASE + "provenance.json": request.mqtt_provenance,
                    BASE + "licenses/openssl-copyright.txt": tls / "usr/share/doc/libssl3/copyright",
                    "share/firefly/doc/m20-arm64-release-runbook.md": request.source_root / "docs/m20-arm64-release-runbook.md"})
    for name in UNITS:
        sources[IMAGE + "etc/systemd/system/" + name] = request.source_root / "packaging/mqtt" / name
    sources[IMAGE + "etc/firefly/mqtt/mosquitto.conf.template"] = request.source_root / "packaging/mqtt/mosquitto.conf.template"
    for name in ("LICENSE-EDL-1.0.txt", "LICENSE-EPL-2.0.txt", "LICENSE.txt", "NOTICE.md"):
        sources[BASE + "licenses/" + name] = mqtt / "share/mosquitto" / name
    # Copy inputs as regular independent files; no SDK symlink escapes.
    for name, source in sources.items():
        r._copy_regular(source, release / name)
        if name in {IMAGE + "usr/sbin/mosquitto", IMAGE + "usr/bin/mosquitto_passwd",
                    IMAGE + "usr/libexec/firefly-mqtt-control-helper"}:
            (release / name).chmod(0o755)
    (release / IMAGE / "PROVISIONING.txt").write_text(
        "OFFLINE IMAGE INPUTS ONLY. No enabled links, daemon start or accounts are created.\n"
        "Owner provisioning must verify hashes/ELF/permissions, install root-owned files,\n"
        "retain disabled/stopped broker and control socket, render the explicit RFC1918\n"
        "LAN address with stage-local-mqtt.py, and privately provision separate Firefly/IoT\n"
        "accounts. No default passwords; allow_anonymous=false. Never copy this template\n"
        "as an active configuration. Activation is a separately authorized operation.\n")


def metadata():
    return {"enabled": True, "libmosquitto": "2.0.18", "tls": "OpenSSL3/verified-CA-and-hostname",
            "broker_default": "disabled_and_stopped", "credentials": "NOT_PACKAGED",
            "installed": False}


def verify(release):
    """Verify every package ELF and resolve the complete closure in target userspace."""
    for base, directories, files in os.walk(release):
        for path in [Path(base), *(Path(base) / n for n in files)]:
            st = path.lstat()
            if st.st_uid not in (0, os.geteuid()) or st.st_mode & 0o6022:
                raise r.ReleaseError("unsafe MQTT release mode/owner: " + str(path))
    for name in PROGRAMS:
        p = release / "bin" / name
        if p.stat().st_mode & 0o7777 != 0o755 or p.stat().st_uid not in (0, os.geteuid()):
            raise r.ReleaseError("unsafe MQTT executable mode/owner: " + name)
        if r._elf(p)[1] != ["$ORIGIN/../lib"]:
            raise r.ReleaseError("invalid MQTT sibling RUNPATH: " + name)
    for name in ("firefly-demo", "firefly-admin", "firefly-mqtt-probe-worker", "firefly-mqtt-publish-worker"):
        if "libmosquitto.so.1" not in r._elf(release / "bin" / name)[0]:
            raise r.ReleaseError("MQTT executable is missing libmosquitto linkage: " + name)
    template = (release / IMAGE / "etc/firefly/mqtt/mosquitto.conf.template").read_text()
    if "allow_anonymous false" not in template or "@LAN_IPV4@" not in template:
        raise r.ReleaseError("unsafe broker provisioning template")
    result = []
    for name in r._rel_files(release):
        path = release / name
        with path.open("rb") as f:
            magic = f.read(20)
        if magic[:4] != b"\x7fELF":
            continue
        if magic[4:6] != b"\x02\x01" or int.from_bytes(magic[18:20], "little") != 183:
            raise r.ReleaseError("package contains non-AArch64 ELF: " + name)
        if path.stat().st_mode & 0o6022:
            raise r.ReleaseError("unsafe ELF permissions: " + name)
        needed, paths = r._elf(path)
        for value in paths:
            if any(not x.startswith("$ORIGIN") or "/home/" in x for x in value.split(":")):
                raise r.ReleaseError("non-relocatable ELF RUNPATH: " + name)
        env = {k: v for k, v in os.environ.items() if k not in {"LD_LIBRARY_PATH", "LD_PRELOAD"}}
        p = subprocess.run(["ldd", str(path)], env=env, text=True, capture_output=True, timeout=30)
        text = p.stdout + p.stderr
        if "not found" in text or (needed and p.returncode):
            raise r.ReleaseError("unresolved package ELF " + name + ": " + text)
        resolved = dict(re.findall(r"(\S+) => (\S+) \(", text))
        for library in needed:
            if library in SYSTEM_LIBS:
                continue
            where = resolved.get(library)
            if not where or not r._within(Path(where).resolve(), release.resolve()):
                raise r.ReleaseError("dependency escapes immutable release: " + name + " -> " + library)
        result.append({"path": name, "needed": needed, "runpath": paths})
    # Metadata query only: no client creation, DNS, CONNECT or device execution.
    library = ctypes.CDLL(str(release / "lib/libmosquitto.so.1"))
    version = [ctypes.c_int(), ctypes.c_int(), ctypes.c_int()]
    library.mosquitto_lib_version(*[ctypes.byref(v) for v in version])
    if [v.value for v in version] != [2, 0, 18]:
        raise r.ReleaseError("packaged libmosquitto version is not2.0.18")
    return result


def finalize_candidate(source, destination, release_id):
    """Owner preparation only: validate actual installed models, publish a PRIVATE stage.

    Never installs a release or switches pointers. Existing install/runner gates
    still follow this command, with their original owner/supervision protections.
    """
    if not r._owner_accepted() or os.geteuid() != 0:
        raise r.ReleaseError("finalize-candidate requires owner authorization and root")
    destination = r.assert_stage_destination(destination, source)
    if destination.exists():
        raise r.ReleaseError("candidate finalization destination must be NEW")
    document = r.verify_tree(source, release_id, candidate_only=True)
    models = r.installed_model_paths()  # Full SHA256, regular path, root owner, no aliases.
    for item in document["shared_models"]:
        r._expect(models[item["path"]], item["sha256"], item["bytes"])
    # Publish atomically only after the ordinary production verifier passes.
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".candidate-", dir=destination.parent) as temporary:
        prefix = Path(temporary) / "prefix"
        release = prefix / "releases" / release_id
        shutil.copytree(source / "releases" / release_id, release)
        document["candidate_origin_sha256"] = r.sha256_file(source / "releases" / release_id / "manifest.json")
        document.pop("offline_candidate")
        document["source"]["identity"] = "committed-production"
        document["production_identity_checked"] = True
        document["installed_model_reference"] = {"prefix": r.PRODUCTION_PREFIX, "mode": "installed-read-only"}
        r._write_manifest(release, document)
        r.verify_tree(prefix, release_id)
        r.require_production_identity(document, release)
        r._publish_directory(prefix, destination)
