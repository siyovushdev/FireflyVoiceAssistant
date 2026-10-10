# M18.6 owner acceptance runbook

Owner acceptance is **not ready**. Two blockers remain:

- The implementation source is committed at review commit
  `3b96eea1264795c411798eba32d51fed555e0102`; this runbook has a documentation
  follow-up. Owner review must select the accepted commit. A clean configure
  and rebuild of both `firefly-demo` and `firefly-config` from that accepted
  commit is still REQUIRED. The existing binary was built before the review
  commit and reports the earlier base with `dirty=true`; it has no
  committed-production provenance. Committing source does not update a binary.
- The first complete stage, including the wake packages and application,
  needs about 9.3 GiB free. The documented stage-then-install procedure needs
  room for every retained copy on the selected filesystem (see section 3). Recheck
  available space; the takeover measurement is recorded in the engineering report.
  Do not delete the SDK, the live checkout, or
  `/var/lib/firefly/store` to make room.

The running service, its unit, and the ConfigStore stay as they are until
those blockers are gone and the owner explicitly approves the cutover. Nothing
below restarts systemd by itself.

Symlink changes use a temporary symlink and `rename`. Do not use `ln -sfn`.
Do not delete `/opt/firefly/voice-assistant`.

## 1. Fixture staging verification

Use Bash with `set -euo pipefail` and `umask 077` for the commands in this
runbook. Initialize `RELEASE_ID` once from the stage output in section 2;
use the variable thereafter. Record the unit backup path as `SAVED_UNIT`
after section 5 prints it. Run each privileged command individually and stop
at any failure.

Select an owner-approved, already mounted filesystem with sufficient space
before any fixture or production staging. The example below uses external
storage; replace `STORAGE_MOUNT` with the approved mount point. The owner must
prepare `STAGING_ROOT` as a real, writable directory owned by the staging
user without group/other write access. Review and isolated installation
prefixes must be new or have only previously verified matching artifacts.
Do not use `/tmp` for model staging unless its capacity has been approved.

```sh
set -euo pipefail
umask 077
STORAGE_MOUNT='/mnt/OWNER_APPROVED_STORAGE'
STAGING_ROOT="$STORAGE_MOUNT/firefly-m186-owner-review"
FIXTURE_PREFIX="$STAGING_ROOT/fixture"
FIXTURE_INPUTS="$STAGING_ROOT/fixture-inputs"
REVIEW_PREFIX="$STAGING_ROOT/review"
ISOLATED_PREFIX="$STAGING_ROOT/install-check"
PRODUCTION_PREFIX='/opt/firefly/voice-assistant'
mountpoint -q "$STORAGE_MOUNT"
test -d "$STAGING_ROOT"
df -B1 "$STAGING_ROOT"
findmnt --target "$STAGING_ROOT" --output SOURCE,TARGET,FSTYPE,OPTIONS
```

Stop if the mount is absent or its identity/capacity differs from the approved
filesystem. The fixture inputs below are prepared test stand-ins, not copies
of production models.

This is what automated tests already do. It uses small stand-in models and the
real libraries, Amina model, and wake packages. It does not claim a
committed-production identity.

```sh
python3 scripts/stage-release.py stage \
  --destination "$FIXTURE_PREFIX" \
  --build-dir build \
  --source-root . \
  --runtime-prefix "$FIXTURE_PREFIX" \
  --rkllm-library /home/firefly/demo_Linux_aarch64/lib/librkllmrt.so \
  --sherpa-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libsherpa-onnx-c-api.so \
  --onnx-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libonnxruntime.so \
  --runtime-model "$FIXTURE_INPUTS/runtime.rkllm" \
  --admin-model "$FIXTURE_INPUTS/admin.rkllm" \
  --stt-dir "$FIXTURE_INPUTS/stt" \
  --vad-model "$FIXTURE_INPUTS/vad.onnx" \
  --wake-site-packages /home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/lib/python3.10/site-packages
```

For a fixture staged from clean committed source, confirm `manifest.json`
has `"identity": "committed-fixture"` and
`"production_identity_checked": false`. If the source is dirty it must say
`"dirty-worktree"`. Neither fixture identity is a production release.
Confirm `current` was not created.

## 2. Committed-source production staging

Run this only after owner review accepts a specific commit, a clean
`git status --porcelain`, clean configure/build of both shipped binaries from
that same accepted commit, and enough free space for section 3. Do not reuse
the pre-commit build or infer its provenance from the now-committed source.
`--production` refuses a dirty tree and a binary whose embedded head or source
hash does not match the worktree. Do not stage the real models until that
space exists. The command is:

```sh
python3 scripts/stage-release.py stage \
  --destination "$REVIEW_PREFIX" \
  --build-dir build \
  --source-root . \
  --runtime-prefix /opt/firefly/voice-assistant \
  --rkllm-library /home/firefly/demo_Linux_aarch64/lib/librkllmrt.so \
  --sherpa-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libsherpa-onnx-c-api.so \
  --onnx-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libonnxruntime.so \
  --runtime-model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm \
  --stt-dir /home/firefly/FireflyVoiceAssistantLinux/build/stt/model \
  --vad-model /home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab/silero_vad.onnx \
  --wake-site-packages /home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/lib/python3.10/site-packages \
  --production
```

The command prints a release id. Set it once to that exact recorded value,
then verify:

```sh
RELEASE_ID='RELEASE_ID_FROM_STAGE_OUTPUT'
export RELEASE_ID
python3 scripts/stage-release.py verify \
  --destination "$REVIEW_PREFIX" \
  --release-id "$RELEASE_ID"
```

Require `"identity": "committed-production"` and
`"production_identity_checked": true`. A dirty tree must fail before any copy.

## 3. Disk-space prerequisites

```sh
df -B1 "$STAGING_ROOT"
if test -d "$PRODUCTION_PREFIX"; then
    df -B1 "$PRODUCTION_PREFIX"
fi
```

The two RKLLM models are 9,688,240,448 bytes. Speech models add 71,190,073
bytes. Libraries add 46,179,856 bytes. The tool also keeps a 64 MiB margin and
copies the wake tree. The first stage therefore needs about 9.3 GiB free.
A later release reuses shared models whose SHA256 already matches and does not
copy them again. That savings does not apply to the first copy.

Sections 4 and 5 each make another complete copy into an installation prefix.
If review and production installation share a filesystem, budget about
18.6 GiB for those two bundles, plus additional operating headroom. If the
real-model isolated installation from section 4 is also retained there during
cutover, budget about 27.9 GiB for all three bundles plus headroom; prefer
at least 35 GiB free for that sequence. Freeing only enough
space for staging does not make the first installation possible. Prefer
owner-provided external storage with at least 25 GiB free for two bundles,
or 35 GiB for the three-copy sequence. The current
installer creates its incoming model copies inside the destination prefix
and publishes them with a same-filesystem rename. Mounting only an empty
`models/` directory on another filesystem does not solve the first install:
the prefix still needs copy space, and cross-filesystem publication fails.
Prefer staging and the entire installation prefix on the approved external
filesystem, with a separately approved persistent mount or bind mount at
`/opt/firefly/voice-assistant`. An alternative is separately approved
prepopulation of verified external models followed by model reuse; that
procedure has not been performed or accepted here. Model symlinks are refused. Existing
SDK models must not be hardlinked, moved, or removed. No storage or mount
changes are performed by this documentation task.

Before external-storage cutover, the owner must separately approve and verify:

- Persistent mount-at-boot configuration using a stable filesystem identity
  (for example, UUID), including any bind mount of the entire prefix at
  `/opt/firefly/voice-assistant`. The runtime, models, releases, and rollback
  data must remain available after reboot; a manually mounted disk is insufficient.
- Systemd mount dependencies and ordering that require the external filesystem
  and any bind mount before Firefly starts. This requires an owner-approved
  `RequiresMountsFor=/opt/firefly/voice-assistant` dependency, or equivalent
  explicit `Requires=`/`After=` mount dependencies, with verified failure
  behavior when storage is unavailable. The packaged service is unchanged by
  this documentation correction; configuring these dependencies is a separate
  owner-authorized operation and a prerequisite, not an optional later step.
- Root ownership and safe permissions of the mounted production prefix and
  its parents, and confirmation that installation scratch space and shared
  model publication are on the same filesystem.

Do not begin section 5 until those requirements are satisfied. If the
production prefix does not yet exist, inspect the approved backing filesystem
with `df` first; repeat the production-prefix capacity check after the owner
prepares its mount. The cutover commands below assume the recommended
whole-prefix external mount. Do not accidentally install into an unmounted
directory on the root disk.

Stop if free space is short. Do not remove production data or the SDK.

## 4. Isolated installation verification

Using the fixture or review prefix as `--source`, install into a second
temporary prefix. This checks the non-destructive installer without `/opt`.

```sh
python3 scripts/stage-release.py install-release \
  --source "$REVIEW_PREFIX" \
  --destination "$ISOLATED_PREFIX" \
  --release-id "$RELEASE_ID"
python3 scripts/stage-release.py verify \
  --destination "$ISOLATED_PREFIX" \
  --release-id "$RELEASE_ID"
```

Repeat the install of the same id and confirm it fails without changing the
existing release. Confirm `current` is still absent unless you deliberately
activate against a temporary store. A full filesystem during the copy must
leave the destination prefix, its `current` symlink, and its previous releases
unchanged.

## 5. Owner-authorized real cutover

Do not start this section while section 2 or 3 is blocked.

Capture the store and the process first. These commands only read production
paths and write a new private evidence directory. After `initialize()` at
revision 1, `backups/` or either Last Known Good entry may legitimately be
absent. Record exact presence, entry types, directory contents (including
empty directories), and file bytes. An absent entry stays absent; these checks
never create a missing production entry. Keep the evidence private because
it contains configuration bytes; do not commit it.

```sh
REVIEW_RELEASE="$REVIEW_PREFIX/releases/$RELEASE_ID"
INSTALLED_RELEASE="$PRODUCTION_PREFIX/releases/$RELEASE_ID"
RELEASE_TOOL="$REVIEW_RELEASE/share/firefly/deployment/scripts/stage-release.py"
CONFIG_TOOL="$REVIEW_RELEASE/bin/firefly-config"
ACCEPTANCE_EVIDENCE=$(mktemp -d "${TMPDIR:-/tmp}/firefly-m186-acceptance.XXXXXX")
export ACCEPTANCE_EVIDENCE
"$CONFIG_TOOL" audit --store /var/lib/firefly/store | tee "$ACCEPTANCE_EVIDENCE/audit-before.json"
python3 - << 'PY'
import base64, hashlib, json, os, pathlib, stat
store = pathlib.Path("/var/lib/firefly/store")
evidence = pathlib.Path(os.environ["ACCEPTANCE_EVIDENCE"])
def read_entry(path):
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return {"present": False}
    if stat.S_ISREG(mode):
        return {"present": True, "type": "file",
                "content_base64": base64.b64encode(path.read_bytes()).decode("ascii")}
    if stat.S_ISDIR(mode):
        return {"present": True, "type": "directory",
                "entries": {child.name: read_entry(child)
                            for child in sorted(path.iterdir())}}
    raise SystemExit(f"unsafe store entry (symlink or special file): {path}")

names = ("configuration.json", "backups", "last_known_good.json",
         "last_known_good_snapshot.json")
snapshot = {name: read_entry(store / name) for name in names}
if snapshot["configuration.json"].get("type") != "file":
    raise SystemExit("configuration.json must be present and regular")
for name, expected_type in (("backups", "directory"), ("last_known_good.json", "file"),
                            ("last_known_good_snapshot.json", "file")):
    if snapshot[name]["present"] and snapshot[name]["type"] != expected_type:
        raise SystemExit(f"unexpected store entry type: {name}")
(evidence / "store-before.json").write_text(json.dumps(snapshot, sort_keys=True) + "\n")
body = base64.b64decode(snapshot["configuration.json"]["content_base64"])
document = json.loads(body)
print("file_sha256", hashlib.sha256(body).hexdigest())
print("schema_version", document.get("schema_version"))
print("revision", document.get("revision"))
print("retired_ids", document.get("retired_ids"))
print("entry_presence", {name: entry["present"] for name, entry in snapshot.items()})
PY
systemctl show firefly-voice-assistant.service -p MainPID -p ActiveState -p FragmentPath
PID=$(systemctl show -p MainPID --value firefly-voice-assistant.service)
sudo readlink -f /proc/$PID/exe
sudo sha256sum /proc/$PID/exe
tr '\0' '\n' < /proc/$PID/cmdline
sudo grep -E 'librkllmrt|libsherpa|libonnxruntime' /proc/$PID/maps
```

Review the unit before installing it. `systemd-analyze` exits non-zero until
`/opt/firefly/voice-assistant/current/bin/firefly-demo` exists. That missing
path is an expected pre-install diagnostic. Any other
`firefly-voice-assistant.service:` line is a unit defect and stops the
procedure. Warnings for `netplan-ovs-cleanup.service` and `rkaiq_3A.service`
are foreign units and are not fixed here.

```sh
if systemd-analyze verify packaging/firefly-voice-assistant.service > /tmp/firefly-unit-verify.txt 2>&1; then
    UNIT_VERIFY_RC=0
else
    UNIT_VERIFY_RC=$?
fi
export UNIT_VERIFY_RC
python3 - << 'PY'
import os, pathlib, sys
sys.path.insert(0, ".")
from packaging.firefly_release import classify_systemd_analyze
text = pathlib.Path("/tmp/firefly-unit-verify.txt").read_text()
report = classify_systemd_analyze(text)
print(report)
if report["defects"]:
    raise SystemExit("unit defect")
if int(os.environ["UNIT_VERIFY_RC"]) != 0 and not (report["expected_absent_install"] or report["foreign"]):
    raise SystemExit("unclassified systemd-analyze failure")
PY
```

After approval, run the following one at a time. If a command fails, stop.
Do not reload or restart the service after a failed install, ownership change,
unit backup, or activation. `sudo VAR=value` puts `FIREFLY_OWNER_ACCEPTED` in
the elevated process even when sudo resets the environment. Activation uses
that variable so it may update `/opt`, and `firefly-config audit` allows root
to read the private firefly-owned store without writing it.

```sh
mountpoint -q "$PRODUCTION_PREFIX"
sudo install -d -o root -g root -m 755 /opt/firefly "$PRODUCTION_PREFIX"
sudo install -d -o root -g root -m 755 /opt/firefly/voice-assistant/releases /opt/firefly/voice-assistant/models
sudo install -d -o root -g root -m 755 /opt/firefly/voice-assistant/systemd-backup
```

Stop if those fail. The prefix and its parents must be root-owned before any
privileged installation. Also make the approved review bundle and its staging
parent root-owned before invoking its code as root. Ancestors above
`STAGING_ROOT` up to the approved mount must already be root-owned without
group/other write access; otherwise
the service user could rename a root-owned bundle through a writable parent.
Inspect those ancestors and stop if that contract is not met:

```sh
namei -l "$REVIEW_PREFIX"
sudo chown root:root "$STAGING_ROOT"
sudo chmod 755 "$STAGING_ROOT"
sudo chown -R root:root "$REVIEW_PREFIX"
sudo chmod -R go-w "$REVIEW_PREFIX"
sudo chmod 755 "$REVIEW_PREFIX"
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" install-release \
  --source "$REVIEW_PREFIX" \
  --destination /opt/firefly/voice-assistant \
  --release-id "$RELEASE_ID"
```

Stop if that fails. Then make the new release and the shared models root-owned
and not writable by `firefly` before activation:

```sh
sudo chown -R root:root "$INSTALLED_RELEASE" /opt/firefly/voice-assistant/models
sudo find "$INSTALLED_RELEASE" /opt/firefly/voice-assistant/models -type d -exec chmod 755 {} +
sudo find "$INSTALLED_RELEASE" /opt/firefly/voice-assistant/models -type f -exec chmod 644 {} +
sudo find "$INSTALLED_RELEASE" -type f \( -name firefly-demo -o -name firefly-config -o -name python -o -name '*.so*' \) -exec chmod 755 {} +
```

Stop if that fails. Capture the approved binary digest from the verified,
root-owned review bundle before activation. The post-start check compares
both the installed binary and `/proc/PID/exe` to this digest, not to a digest
obtained from the running process itself.

```sh
APPROVED_BINARY_SHA256=$(sha256sum "$REVIEW_RELEASE/bin/firefly-demo")
APPROVED_BINARY_SHA256=${APPROVED_BINARY_SHA256%% *}
```

Back up the checkout unit before replacing it:

```sh
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" install-unit \
  --source "$REVIEW_RELEASE/share/firefly/systemd/firefly-voice-assistant.service" \
  --destination /etc/systemd/system/firefly-voice-assistant.service \
  --backup-dir /opt/firefly/voice-assistant/systemd-backup
```

The printed backup path is the previous checkout unit. Keep it and set
`SAVED_UNIT` to that full path for section 7. Stop if this fails; the previous
unit is then still installed. Do not restart.

```sh
SAVED_UNIT='FULL_BACKUP_PATH_FROM_INSTALL_UNIT_OUTPUT'
```

```sh
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" activate \
  --destination /opt/firefly/voice-assistant \
  --release-id "$RELEASE_ID" \
  --store /var/lib/firefly/store
```

Stop if activation fails. Do not restart. Only after every command above
exits 0:

```sh
sudo systemctl daemon-reload
sudo systemctl restart firefly-voice-assistant.service
```

`--fresh` is limited to fixture activation and is refused in production.
Production activation and rollback must audit the exact private
`/var/lib/firefly/store`, owned by `firefly`, through the target release's
shipped C++ loader. Its configuration file must have the same owner as the
store directory, including when root runs the audit. A release staged with any runtime prefix
other than `/opt/firefly/voice-assistant` is refused at this destination.

## 6. Post-start checks

The health gate does not identify the process. Automatically assert identity
first, using the release ID from section 2 and the approved review binary
digest captured before restart. Any mismatch fails acceptance before health
or voice smoke. Run this only for the new release; checkout rollback uses the
captured checkout identity instead.

```sh
PID=$(systemctl show -p MainPID --value firefly-voice-assistant.service)
sudo python3 - "$PRODUCTION_PREFIX" "$RELEASE_ID" "$APPROVED_BINARY_SHA256" "$PID" << 'PY'
import hashlib, os, pathlib, subprocess, sys
prefix = pathlib.Path(sys.argv[1])
release_id, approved_sha256, pid_text = sys.argv[2:]
pid = int(pid_text)
if pid <= 0:
    raise SystemExit("service has no running main PID")
current = prefix / "current"
expected_release = (prefix / "releases" / release_id).resolve(strict=True)
if os.readlink(current) != f"releases/{release_id}" or current.resolve(strict=True) != expected_release:
    raise SystemExit("current does not match the approved release ID")
approved_binary = (expected_release / "bin/firefly-demo").resolve(strict=True)
if (current / "bin/firefly-demo").resolve(strict=True) != approved_binary:
    raise SystemExit("installed current binary differs from the approved release")
proc_exe = pathlib.Path(f"/proc/{pid}/exe")
if pathlib.Path(os.readlink(proc_exe)).resolve(strict=True) != approved_binary:
    raise SystemExit("running executable differs from approved current/bin/firefly-demo")
def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
if sha256(approved_binary) != approved_sha256 or sha256(proc_exe) != approved_sha256:
    raise SystemExit("installed or running executable SHA256 differs from approved binary")
observed_pid = subprocess.check_output(
    ["systemctl", "show", "-p", "MainPID", "--value", "firefly-voice-assistant.service"],
    text=True).strip()
if observed_pid != pid_text or current.resolve(strict=True) != expected_release:
    raise SystemExit("service PID or current release changed during identity checks")
print("PASS approved release, running executable, and SHA256", release_id, pid)
PY
sudo grep -E 'librkllmrt|libsherpa|libonnxruntime' /proc/$PID/maps
curl -fsS http://127.0.0.1:8080/api/status | tee /tmp/firefly-status-after.json >/dev/null
python3 /opt/firefly/voice-assistant/current/share/firefly/deployment/scripts/stage-release.py health-gate \
  --url http://127.0.0.1:8080/api/status \
  --status-file /tmp/firefly-status-after.json
```

The gate passes only for loopback port 8080 with runtime, STT, microphone,
`voice=WAITING_WAKE`, and ready admin, configuration, runtime, llm, stt, and
wake. A LAN request to port 8080 must fail.

Voice smoke: «Амина», then «Какая погода?». The expected result is the existing
unsupported command, with no device action.

```sh
python3 - << 'PY'
import base64, hashlib, json, os, pathlib, stat
store = pathlib.Path("/var/lib/firefly/store")
evidence = pathlib.Path(os.environ["ACCEPTANCE_EVIDENCE"])
def read_entry(path):
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return {"present": False}
    if stat.S_ISREG(mode):
        return {"present": True, "type": "file",
                "content_base64": base64.b64encode(path.read_bytes()).decode("ascii")}
    if stat.S_ISDIR(mode):
        return {"present": True, "type": "directory",
                "entries": {child.name: read_entry(child)
                            for child in sorted(path.iterdir())}}
    raise SystemExit(f"unsafe store entry (symlink or special file): {path}")

before = json.loads((evidence / "store-before.json").read_text())
after = {name: read_entry(store / name) for name in before}
if after != before:
    changed = [name for name in before if after[name] != before[name]]
    raise SystemExit(f"store entry presence or content changed: {changed}")
body = base64.b64decode(after["configuration.json"]["content_base64"])
document = json.loads(body)
audit_before = json.loads((evidence / "audit-before.json").read_text())
print("file_sha256", hashlib.sha256(body).hexdigest())
print("revision", document.get("revision"))
print("retired_ids", document.get("retired_ids"))
print("content_sha256", audit_before.get("content_sha256"))
print("PASS exact store entry presence and bytes unchanged")
PY
/opt/firefly/voice-assistant/current/bin/firefly-config audit --store /var/lib/firefly/store | cmp - "$ACCEPTANCE_EVIDENCE/audit-before.json"
```

Any difference in the configuration bytes, revision, content hash,
`retired_ids`, entry presence (including previously absent backups/LKG),
backup directory contents, or Last Known Good bytes stops acceptance. The
comparison only reads production paths and never initializes missing entries.

## 7. First-cutover rollback

Use this when the new process is unhealthy and the previous deployment is the
saved checkout unit, not an older directory under `releases/`.

First validate the unchanged store. Stop if this audit fails; do not restore
or restart against a missing, corrupt, or incompatible configuration.

```sh
sudo "$CONFIG_TOOL" audit --store /var/lib/firefly/store
```

Only after the audit exits 0:

```sh
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" restore-unit \
  --backup "$SAVED_UNIT" \
  --destination /etc/systemd/system/firefly-voice-assistant.service
```

Stop if restore fails. Only then:

```sh
sudo systemctl daemon-reload
sudo systemctl restart firefly-voice-assistant.service
```

This puts the checkout unit back and leaves `/opt/firefly/voice-assistant` on
disk. It does not rewrite the ConfigStore. Repeat the PID, executable/hash,
health, and exact store presence/content checks against the captured checkout
baseline. Do not apply the new-release identity assertion to the restored
checkout executable.

## 8. Later release-to-release rollback

Use this only when `previous-release` names another release in this prefix and
that release's manifest can read the store schema.

```sh
sudo FIREFLY_OWNER_ACCEPTED=1 python3 /opt/firefly/voice-assistant/current/share/firefly/deployment/scripts/stage-release.py rollback \
  --destination /opt/firefly/voice-assistant \
  --store /var/lib/firefly/store
```

Stop if rollback fails. The store is not written. Only then:

```sh
sudo systemctl restart firefly-voice-assistant.service
```

The command rechecks the target release, including the wake worker, and reads
the store schema before it moves `current`. A missing, corrupt, or incompatible
store leaves `current` where it is. The owner restart is a separate command.
