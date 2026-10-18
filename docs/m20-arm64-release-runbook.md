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

Use the existing `release-runner.py prepare` with a NEW root-private `/run`
supervisor, exact approved commit/release/assets pin, fresh metadata-preserving
ConfigStore backup and the fixed mandatory recovery fixtures. No old consumed
or boot-bound plan is reusable. Preparation must establish old release and
old-config compatibility BEFORE changing the active configuration contract.

Activation requires another explicit owner window and the runner's existing
`run --owner-authorization URL`. Preserve the independent timer/service,
240-second verification plus360-second recovery budgets, one-use plan and
systemd supervision gate. Confirm empty old service cgroup and exclusive
NPU/microphone ownership before any switch. Do not start a second owner.

Keep the pre-M20 M19-readable configuration backup and its hashes/metadata.
An M19 binary may reject an active MQTT mapping despite schema_version2; schema
number alone is insufficient rollback proof. Before rollback from a later MQTT
configuration, restore/reconcile the approved M19-readable snapshot using the
existing checked recovery/ConfigStore operation, never raw overwrite or history
deletion. Test that exact snapshot with the old config tool. If this cannot be
proven before activation, BLOCK the window. Do not enable new MQTT mappings
until the candidate's owner acceptance and the rollback checkpoint are ready.

Forward failure/agent loss leaves the independent watchdog responsible for
restoring the pinned old release/configuration and fresh readiness gates.
Unknown recovery remains BLOCKED_UNKNOWN; do not disarm the timer or declare
success. Only verified forward acceptance or verified restoration permits its
normal disarm. Do not reset deadlines, reuse plans or stop unrelated processes.

Runtime1024/Admin4096,6GiB admission/1GiB reserve, frozen model identities,
background audio and ConfigStore schema/history/LKG contracts are unchanged.
M19 remains PARTIAL_PASS. Physical relays, live voice/wake/STT/NPU, reboot and
owner hardware acceptance belong to #38; MQTT ACK is not physical confirmation.
