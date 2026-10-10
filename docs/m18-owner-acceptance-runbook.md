# M18.6 owner acceptance runbook

Owner acceptance is **not ready**. Two blockers remain:

- The worktree is dirty, so `--production` refuses to label HEAD as the
  release identity. A commit is a separate approval. This runbook does not
  commit.
- The first complete stage, including the wake packages and application,
  needs about 9.3 GiB free. The documented stage-then-install procedure needs
  room for two copies when both prefixes use the same filesystem. Recheck
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
runbook. Replace `RELEASE_ID` and `SAVED_UNIT` with the recorded values.
Run each privileged command individually and stop at any failure.

This is what automated tests already do. It uses small stand-in models and the
real libraries, Amina model, and wake packages. It does not claim a
committed-production identity.

```sh
python3 scripts/stage-release.py stage \
  --destination /tmp/firefly-m186-fixture \
  --build-dir build \
  --source-root . \
  --runtime-prefix /tmp/firefly-m186-fixture \
  --rkllm-library /home/firefly/demo_Linux_aarch64/lib/librkllmrt.so \
  --sherpa-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libsherpa-onnx-c-api.so \
  --onnx-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libonnxruntime.so \
  --runtime-model /tmp/firefly-m186-fixture-inputs/runtime.rkllm \
  --admin-model /tmp/firefly-m186-fixture-inputs/admin.rkllm \
  --stt-dir /tmp/firefly-m186-fixture-inputs/stt \
  --vad-model /tmp/firefly-m186-fixture-inputs/vad.onnx \
  --wake-site-packages /home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/lib/python3.10/site-packages
```

Confirm `manifest.json` has `"identity": "dirty-worktree"` while this tree is
dirty, and `"production_identity_checked": false`. Confirm `current` was not
created.

## 2. Committed-source production staging

Run this only after a clean `git status --porcelain`, a rebuild of
`firefly-demo` from that same commit, and enough free space for section 3.
`--production` refuses a dirty tree and a binary whose embedded head or source
hash does not match the worktree. Do not stage the real models until that
space exists. The command is:

```sh
python3 scripts/stage-release.py stage \
  --destination /tmp/firefly-m186-review \
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

The command prints a release id. Then:

```sh
python3 scripts/stage-release.py verify \
  --destination /tmp/firefly-m186-review \
  --release-id RELEASE_ID
```

Require `"identity": "committed-production"` and
`"production_identity_checked": true`. A dirty tree must fail before any copy.

## 3. Disk-space prerequisites

```sh
df -B1 /tmp
```

The two RKLLM models are 9,688,240,448 bytes. Speech models add 71,190,073
bytes. Libraries add 46,179,856 bytes. The tool also keeps a 64 MiB margin and
copies the wake tree. The first stage therefore needs about 9.3 GiB free.
A later release reuses shared models whose SHA256 already matches and does not
copy them again. That savings does not apply to the first copy.

Sections 4 and 5 make another complete copy into an installation prefix. If
review and installation share a filesystem, budget about 18.6 GiB for both
bundles, plus additional operating headroom. Freeing only enough
space for staging does not make the first installation possible. Prefer
owner-provided external storage with at least 25 GiB free. The current
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
changes are authorized by this review.

Stop if free space is short. Do not remove production data or the SDK.

## 4. Isolated installation verification

Using the fixture or review prefix as `--source`, install into a second
temporary prefix. This checks the non-destructive installer without `/opt`.

```sh
python3 scripts/stage-release.py install-release \
  --source /tmp/firefly-m186-review \
  --destination /tmp/firefly-m186-install \
  --release-id RELEASE_ID
python3 scripts/stage-release.py verify \
  --destination /tmp/firefly-m186-install \
  --release-id RELEASE_ID
```

Repeat the install of the same id and confirm it fails without changing the
existing release. Confirm `current` is still absent unless you deliberately
activate against a temporary store. A full filesystem during the copy must
leave the destination prefix, its `current` symlink, and its previous releases
unchanged.

## 5. Owner-authorized real cutover

Do not start this section while section 2 or 3 is blocked.

Capture the store and the process first. These commands only read.

```sh
RELEASE_ID='RELEASE_ID'
REVIEW_RELEASE="/tmp/firefly-m186-review/releases/$RELEASE_ID"
RELEASE_TOOL="$REVIEW_RELEASE/share/firefly/deployment/scripts/stage-release.py"
CONFIG_TOOL="$REVIEW_RELEASE/bin/firefly-config"
python3 - << 'PY'
import hashlib, json, pathlib, shutil
store = pathlib.Path("/var/lib/firefly/store")
body = (store / "configuration.json").read_bytes()
pathlib.Path("/tmp/firefly-config-before.json").write_bytes(body)
document = json.loads(body)
print("file_sha256", hashlib.sha256(body).hexdigest())
print("schema_version", document.get("schema_version"))
print("revision", document.get("revision"))
print("retired_ids", document.get("retired_ids"))
PY
"$CONFIG_TOOL" audit --store /var/lib/firefly/store | tee /tmp/firefly-audit-before.json
cp -a /var/lib/firefly/store/backups /tmp/firefly-store-backups-before
cp -a /var/lib/firefly/store/last_known_good.json /tmp/firefly-lkg-before.json
cp -a /var/lib/firefly/store/last_known_good_snapshot.json /tmp/firefly-lkg-snapshot-before.json
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
sudo install -d -o root -g root -m 755 /opt/firefly /opt/firefly/voice-assistant
sudo install -d -o root -g root -m 755 /opt/firefly/voice-assistant/releases /opt/firefly/voice-assistant/models
sudo install -d -o root -g root -m 755 /opt/firefly/voice-assistant/systemd-backup
```

Stop if those fail. The prefix and its parents must be root-owned before any
privileged installation. Also make the approved review bundle root-owned,
so the service user cannot replace code while root verifies it:

```sh
sudo chown -R root:root /tmp/firefly-m186-review
sudo chmod -R go-w /tmp/firefly-m186-review
sudo chmod 755 /tmp/firefly-m186-review
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" install-release \
  --source /tmp/firefly-m186-review \
  --destination /opt/firefly/voice-assistant \
  --release-id RELEASE_ID
```

Stop if that fails. Then make the new release and the shared models root-owned
and not writable by `firefly` before activation:

```sh
sudo chown -R root:root /opt/firefly/voice-assistant/releases/RELEASE_ID /opt/firefly/voice-assistant/models
sudo find /opt/firefly/voice-assistant/releases/RELEASE_ID /opt/firefly/voice-assistant/models -type d -exec chmod 755 {} +
sudo find /opt/firefly/voice-assistant/releases/RELEASE_ID /opt/firefly/voice-assistant/models -type f -exec chmod 644 {} +
sudo find /opt/firefly/voice-assistant/releases/RELEASE_ID -type f \( -name firefly-demo -o -name firefly-config -o -name python -o -name '*.so*' \) -exec chmod 755 {} +
```

Stop if that fails. Back up the checkout unit before replacing it:

```sh
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" install-unit \
  --source "$REVIEW_RELEASE/share/firefly/systemd/firefly-voice-assistant.service" \
  --destination /etc/systemd/system/firefly-voice-assistant.service \
  --backup-dir /opt/firefly/voice-assistant/systemd-backup
```

The printed backup path is the previous checkout unit. Keep it. Stop if this
fails; the previous unit is then still installed. Do not restart.

```sh
sudo FIREFLY_OWNER_ACCEPTED=1 python3 "$RELEASE_TOOL" activate \
  --destination /opt/firefly/voice-assistant \
  --release-id RELEASE_ID \
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

The health gate does not identify the process. Compare identity first.

```sh
PID=$(systemctl show -p MainPID --value firefly-voice-assistant.service)
sudo readlink -f /proc/$PID/exe
sudo sha256sum "$(sudo readlink -f /proc/$PID/exe)"
readlink -f /opt/firefly/voice-assistant/current
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
import hashlib, json, pathlib, filecmp, sys
store = pathlib.Path("/var/lib/firefly/store")
body = (store / "configuration.json").read_bytes()
before = pathlib.Path("/tmp/firefly-config-before.json").read_bytes()
if body != before:
    sys.exit("configuration.json bytes changed")
document = json.loads(body)
audit_before = json.loads(pathlib.Path("/tmp/firefly-audit-before.json").read_text())
print("file_sha256", hashlib.sha256(body).hexdigest())
print("revision", document.get("revision"))
print("retired_ids", document.get("retired_ids"))
print("content_sha256", audit_before.get("content_sha256"))
PY
/opt/firefly/voice-assistant/current/bin/firefly-config audit --store /var/lib/firefly/store | cmp - /tmp/firefly-audit-before.json
cmp /var/lib/firefly/store/last_known_good.json /tmp/firefly-lkg-before.json
cmp /var/lib/firefly/store/last_known_good_snapshot.json /tmp/firefly-lkg-snapshot-before.json
diff -rq /var/lib/firefly/store/backups /tmp/firefly-store-backups-before
```

Any difference in the configuration bytes, revision, content hash,
`retired_ids`, backups, or Last Known Good files stops acceptance.

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
  --backup /opt/firefly/voice-assistant/systemd-backup/SAVED_UNIT \
  --destination /etc/systemd/system/firefly-voice-assistant.service
```

Stop if restore fails. Only then:

```sh
sudo systemctl daemon-reload
sudo systemctl restart firefly-voice-assistant.service
```

This puts the checkout unit back and leaves `/opt/firefly/voice-assistant` on
disk. It does not rewrite the ConfigStore. Repeat the PID, health, and store
diff checks.

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
