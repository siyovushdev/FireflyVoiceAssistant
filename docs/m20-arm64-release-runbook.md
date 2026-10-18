# M20 ARM64 offline candidate and owner activation checkpoints

This is an operator procedure, not permission to execute it. The candidate gate
ends at `M20_ARM64_RELEASE_CANDIDATE_READY_FOR_INDEPENDENT_REVIEW` and
`OWNER_ACTIVATION_REQUIRED`. It neither installs a release nor enables a service.
Use the existing [release procedure](release-procedure.md), release runner,
controller and independent recovery helper. Do not substitute a shell cutover.

## Offline preparation

Pin one clean full product HEAD descended from the independently accepted G3
`6fa47a6ca247e1eec6cec138b2ba2d49f8f05a67`. Provision downloads in a separate,
explicitly authorized SDK preparation step. Keep signed Ubuntu22.04 ARM64 base
and package indexes, downloaded package/source SHA256, the Mosquitto2.0.18 source
signature, compiler versions, frozen library hashes and exact build commands.
Subsequent build/staging must have no network and must not run an installer.
The provisioned build userspace includes native readelf/ldd/strip/patchelf. Stage
sets the two copied libssl RUNPATHs to `$ORIGIN` so their crypto dependency cannot
fall back to a system library; frozen RKLLM/Sherpa/ONNX files are never patched.

Build real Linux/aarch64 with MQTT=ON, assistant/voice/Admin/config tools ON,
the frozen RKLLM1.3.0 and Sherpa1.13.7/ONNX libraries, the verified same-source
Vue dist and `SOURCE_DATE_EPOCH`. Canonical source path is `/src/firefly` so
the existing source fingerprint can be reproduced during later preparation.
Do not call an x86 host-test build a release. Keep the SDK outside the package.

Run CMake's `preinstall` target where its cross RPATH relink is required, then
use `scripts/stage-release.py stage --offline-candidate` with the fixed runtime
prefix `/opt/firefly/voice-assistant`, explicit offline MQTT/TLS roots and their
provenance. This extends the existing stage/install component. It includes:

- demo/config/Admin/runtime and both fixed sibling probe/publish workers;
- regular protected libmosquitto1/OpenSSL3 files, full target ELF closure,
  embedded Vue assets, frozen M19 libraries, wake worker/Python packages/Amina;
- disabled local-broker/controller image inputs and upstream notices;
- exact frozen shared-model references, without fabricated model files.

All executables must be AArch64 and mode0755, without writable group/other bits,
SUID/SGID or unexpected symlinks. Package/archive installation ownership is
root:root. Runtime siblings use `$ORIGIN/../lib`; no SDK/developer RUNPATH or
LD_LIBRARY_PATH is needed. Verify every ELF, including NumPy/LiteRT/OpenBLAS,
not only the main binary. Pin all checksums, modes, dependencies and assets.
The unchanged M19 wake/NumPy/libgfortran closure retains the Ubuntu22.04
`zlib1g` (`libz.so.1`) system prerequisite, alongside glibc, libstdc++, ALSA
and libgomp. Verify these target OS dependencies with native target `ldd`;
the MQTT/OpenSSL dependencies must resolve inside the immutable release.

`verify-candidate` verifies this payload and its exact frozen model references.
It does not claim the absent multi-GiB models have been read. Ordinary `verify`,
`install-release`, activation, rollback and the release runner must reject an
unfinalized candidate. Never remove the candidate marker by hand.

Execute isolated target userspace smoke with the packaged workers and real
broker: auth/TLS, slow/never DNS deadline, next command, no late publish/replay,
no pre-authorized command, non-root/NoNewPrivileges, protected release directory,
resources/children/FD cleanup, private ConfigStore compatibility/recovery and
old M19 fallback negatives. Retain the applicable CPU/frontend/recovery tests.
QEMU userspace evidence is not physical RK3588/NPU/audio acceptance.

## Independent review and owner-only target preparation

Before any target work, obtain independent packaging/security/recovery PASS
and a new explicit owner authorization naming the exact product HEAD, archive
SHA256, release ID, manifest, build/source fingerprint and Admin assets pin.
Read-only recheck the actual installed M19/current/rollback identities; the dated
baseline is not a live pointer. Preserve the original Amina grounding anchor.

Only within that authorization, root may verify the copied candidate and prepare
a NEW private production stage:

```sh
python3 "$SOURCE/scripts/stage-release.py" verify-candidate \
  --destination "$CANDIDATE_PREFIX" --release-id "$NEW_RELEASE"
env FIREFLY_OWNER_ACCEPTED=1 python3 "$SOURCE/scripts/stage-release.py" finalize-candidate \
  --source "$CANDIDATE_PREFIX" --destination "$NEW_PRIVATE_STAGE" --release-id "$NEW_RELEASE"
```

Finalization reads only the fixed root-owned regular installed model paths,
checking every frozen SHA256/size and rejecting missing, aliased/hardlinked or
changed models. It does not copy models, write production, change pointers,
install units or arm a timer. It records the reviewed candidate manifest SHA256
and publishes a new private stage only after the ordinary production verifier
passes. Any failure leaves no published stage. Never bypass that verification.

Use existing `install-release` only under the separately authorized preparation
scope. It creates a new inactive immutable release, retains current/previous
and all models, and refuses replacements. Recheck root ownership, full hashes,
unit equality, frozen references and the old/new release identities afterward.
Do not run install-unit or modify the permanent voice unit as incidental cleanup.
Keep the exact clean source at its canonical path, full applicable test build
and matching frontend dependencies for the existing release-runner preparation.

## Local Mosquitto provisioning is a separate opt-in

`share/firefly/mqtt/image` is inert image input, never an install script. No
enabled links, passwords, accounts, daemon start or socket enable are packaged.
Owner-approved image provisioning may copy the root-owned broker/helper/libraries
and unit files, then must leave BOTH broker and controller socket disabled and
stopped. No package or Admin Apply may auto-enable them.

Render `mosquitto.conf.template` using the existing `stage-local-mqtt.py` with
an explicitly approved RFC1918 LAN address; never use a wildcard/WAN listener.
Keep `allow_anonymous false`. Privately create separate Firefly/IoT accounts
with interactive `mosquitto_passwd`, never a default password or command-line
real secret. Password/config access is root/mosquitto only; service runtime
credentials follow the existing protected credential-store contract. Verify
UID/GID, socket group, read permissions and hardening before explicit Enable.
Actual systemd/reboot/account provisioning is not proven by an inert image.

## Activation and recovery window

The release runner still owns code activation, its independent watchdog and
240-second verification plus360-second recovery budgets. It never rewrites a
ConfigStore. Config migration recovery is a separate checked **offline** operation
below; never ask the old binary to serve an MQTT-bearing history. Keep the old
M19 release/config binary and the original Amina anchor indefinitely.

### Checkpoint → prepare → activation eligibility

Before the first MQTT mapping is saved/applied, create a persistent root-private
checkpoint outside `/run` and outside the store. This copies the **whole** tree:
current, `.lock`, every backup, LKG marker and snapshot, and other metadata,
preserving paths, bytes, SHA256, ownership, modes and mtime. ConfigStore's existing
exclusive lock prevents interleaved Save/Apply/LKG writes during copying. Pending
transactions, symlinks, hardlinks, unsafe permissions or any unstable copy reject
publication. No input file is created, reconciled or overwritten.

In a separately authorized owner preparation window (commands are instructions,
not actions performed by this candidate gate):

```sh
# Persist both the checkpoint directory AND the printed pin in protected evidence.
# M19_CONFIG is the original c7fa1490 M19 bin/firefly-config, pinned by the tool:
# SHA256 95aad868b99c09e646adf3234347f1c0a643c50b371d775ec65c5dcacd5afd74
python3 "$SOURCE/scripts/config-checkpoint.py" create \
  --store /var/lib/firefly/store --checkpoint "$NEW_PERSISTENT_CHECKPOINT" \
  --m19-config "$M19_CONFIG"
python3 "$SOURCE/scripts/config-checkpoint.py" verify \
  --checkpoint "$NEW_PERSISTENT_CHECKPOINT" --checkpoint-pin "$CHECKPOINT_PIN" \
  --m19-config "$M19_CONFIG"
python3 "$SOURCE/scripts/release-runner.py" prepare \
  --source "$SOURCE" --approved-commit "$APPROVED_HEAD" --new-release "$NEW_RELEASE" \
  --root "$NEW_RUN_BUNDLE" --build-dir "$VERIFIED_BUILD_DIR" --approved-admin-assets-sha256 "$APPROVED_ASSETS" \
  --m19-checkpoint "$NEW_PERSISTENT_CHECKPOINT" --m19-checkpoint-pin "$CHECKPOINT_PIN" \
  --m19-config "$M19_CONFIG" \
  --m19-recovery-capsule "$NEW_PERSISTENT_M19_CAPSULE"
```

Persist the returned `m19_recovery_capsule_pin` separately with the checkpoint
pin. `--build-dir` explicitly selects the full applicable build (cross pipeline:
`/build/firefly`); its regular CMake cache must name the exact canonical source.
The default remains `SOURCE/build`. No symlink/copy of a test build is needed.
Checkpoint and capsule must use persistent ext4/XFS/Btrfs, outside `/run`,
`/tmp` and `/dev/shm`; prepare checks the actual mounted filesystem/device.
The capsule pins the original M19 main/config executables, immutable manifest,
all artifact modes, unit/argv, whole-store metadata and verified old readiness.
It contains no old timer, boot ID, deadline or execution claim. Keep the original
M19 release and this capsule. Later M20 preparations use this existing capsule
with `--m19-recovery-capsule-pin`; never replace it with an M20 readiness snapshot.

Verification runs the **actual pinned M19** on a disposable cloned store:
`audit`, `current`, `reconcile`, `revisions`, inspect every history/LKG document,
validate LKG revision/hash, CAS Save, history read and CAS Restore, then audit and
history again. Original snapshot bytes/metadata remain unchanged. A successful
`audit` alone is insufficient. Before initial M19→M20 preparation the current
whole-store inventory must still equal the checkpoint; otherwise create a NEW
checkpoint and repeat preparation. The pin and M19 executable SHA are frozen in
the runner plan. Preparation uses the complete private packaging module bundle,
ordinary production manifest verification and the frozen-artifact preservation
gate. Missing/corrupt modules or checkpoint errors abort before PREPARED/cutover.

Code activation requires its own explicit owner window and existing
`run --owner-authorization URL`; no consumed/boot-bound plan is reusable. Do not
save/Apply MQTT until independent package acceptance, owner activation gates and
the durable verified checkpoint exist. Preserve supervision, empty old cgroup,
exclusive NPU/microphone ownership, model identities, unit and flags.

### Whole-store rollback after MQTT Save/Apply

A current-only `restore --revision` is **not** rollback: MQTT documents remain in
backups and make old M19 revisions/CAS fail. In a separately owner-authorized
recovery window:

1. Keep commands closed; use existing supervised shutdown/resource-quiescence
   procedure. Confirm voice unit inactive, MainPID0, empty complete cgroup and no
   other store writers. Do not switch to/start M19 over a running M20 store.
2. Verify the preserved checkpoint with its external pin and exact original M19
   executable. Preserve the newer store; never delete MQTT history or edit schema.
3. Choose a NEW root-private transaction **sibling** of `/var/lib/firefly/store`,
   on the same filesystem. Run the checked helper below. It prepares and verifies
   the complete old tree, writes/fsyncs a journal, atomically exchanges directories
   with Linux `RENAME_EXCHANGE` (no non-atomic fallback), fsyncs both parents, and
   retains the entire newer M20 store as `TRANSACTION/preserved-store`. It does not
   stop/start any service or switch any release pointer.

```sh
env FIREFLY_OWNER_ACCEPTED=1 python3 "$SOURCE/scripts/config-checkpoint.py" restore \
  --store /var/lib/firefly/store --checkpoint "$NEW_PERSISTENT_CHECKPOINT" \
  --checkpoint-pin "$CHECKPOINT_PIN" --m19-config "$M19_CONFIG" \
  --transaction "$NEW_SIBLING_TRANSACTION"
```

4. Require `RESTORED_VERIFIED` and `VERIFIED.json`. The real M19 full history/CAS/
   restore proof must pass; exact checkpoint inventory and preserved M20 inventory
   must match their journal hashes/metadata. Retain checkpoint, transaction,
   newer-store history and external pins indefinitely for analysis/return to M20.
   The checkpoint's revision intentionally resumes its original CAS epoch; no
   client or runtime generation may remain alive across this offline rollback.
5. With the service still stopped, prepare the explicit **fresh** M19 bootstrap
   mode of the existing runner. Use reviewed current release-control source and
   its commit, not an old source checkout or an old consumed `/run` bundle:

```sh
python3 "$SOURCE/scripts/release-runner.py" prepare-rollback \
  --source "$SOURCE" --approved-commit "$APPROVED_RELEASE_CONTROL_HEAD" \
  --root "$NEW_RECOVERY_RUN_BUNDLE" --build-dir "$VERIFIED_BUILD_DIR" \
  --m19-checkpoint "$NEW_PERSISTENT_CHECKPOINT" --m19-checkpoint-pin "$CHECKPOINT_PIN" \
  --m19-config "$M19_CONFIG" --m19-recovery-capsule "$NEW_PERSISTENT_M19_CAPSULE" \
  --m19-recovery-capsule-pin "$M19_CAPSULE_PIN"
# Only in the separately owner-authorized activation/recovery window:
python3 "$NEW_RECOVERY_RUN_BUNDLE/scripts/release-runner.py" run \
  "$NEW_RECOVERY_RUN_BUNDLE" --owner-authorization "$OWNER_AUTHORIZATION_URL"
```

The bootstrap preparation validates the exact original M19 code/manifest/modes,
full restored store and pinned readiness capsule, and requires empty service
cgroup/NPU/microphone ownership. It writes a new plan/backup/PREPARED receipt.
The same launcher/controller arms a fresh 240+360-second independent watchdog;
only successful existing typed supervision creates `BOOTSTRAP_AUTHORIZED`.
Then the existing stop → resource barriers → checked pointer selection → start →
readiness/non-actuating checks run for the fixed M19 target. Fresh M19 bootstrap
starts RuntimeGeneration1; catalog revision/hash and all other gates stay pinned.
A stale/unarmed plan or absent/changed capsule cannot authorize a start. A broken
M20 image is retained and is not executed to validate the preserved M19 target.
Success is `ROLLED_BACK`, operation `M19_BOOTSTRAP_RECOVERY`, after independent old
readiness verification; only then may the timer be disarmed. Ordinary forward
preparation retains the strict frozen-M19 preservation contract.

Old `audit`, `current`, `revisions`, history/CAS/Restore must pass on the helper's
private proof copy before start. After start recheck read-only audit/status and
use ordinary owner editing; do not invent a live CAS edit to prove recovery.
Keep the newer M20 history/release for analysis. No prior lease/START is reusable.

On crash/failure keep service stopped. The journal distinguishes pre/post-exchange
by exact tree inventories, not a possibly stale phase bit. Retry the SAME
transaction after interrupted exchange; it completes preservation/proof without
exchanging again. After VERIFIED a retry validates the archive and M19 semantics,
allowing legitimate subsequent M19 revisions. Incomplete/corrupt staging or
external edits fail closed: retain it as evidence and use a NEW transaction only
if the original current tree is still intact. Missing/corrupt checkpoint,
unsupported atomic exchange, wrong pin, metadata or history errors mean BLOCKED;
never raw-overwrite, remove backups or start the old service to force PASS.
Full power-loss durability depends on the target filesystem honoring fsync;
process-crash/system-VM fixtures are not a real Firefly power-cut qualification.

Forward failure/agent loss leaves the independent watchdog responsible for
restoring the pinned old release/configuration and fresh readiness gates.
Unknown recovery remains BLOCKED_UNKNOWN; do not disarm the timer or declare
success. Only verified forward acceptance or verified restoration permits its
normal disarm. Do not reset deadlines, reuse plans or stop unrelated processes.

Runtime1024/Admin4096,6GiB admission/1GiB reserve, frozen model identities,
background audio and ConfigStore schema/history/LKG contracts are unchanged.
M19 remains PARTIAL_PASS. Physical relays, live voice/wake/STT/NPU, reboot and
owner hardware acceptance belong to #38; MQTT ACK is not physical confirmation.
