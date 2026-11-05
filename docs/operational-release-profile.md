# Permanent operational voice profile

This is an opt-in profile of the existing CMake, `stage-release.py` and supervised
`release-runner.py` procedure. It adds no service or assistant. A separately
authorized installation window is still required. Do not reuse a consumed plan.

## Prepare

1. Build a clean exact commit with the pinned offline aarch64 SDK, using
   `scripts/m20-arm64-candidate.py ... --operational-voice`. The candidate contains
   both MQTT workers and the unchanged Amila wake revision. Its manifest pins the
   seven existing Russian F2 assets; it contains no downloaded or new model.
2. As root, call `packaging.voice_profile.provision_models(source, destination)`
   with the verified existing F2 directory and
   `/opt/firefly/voice-assistant/models/tts`. This publishes an independent
   root-owned copy after checking all hashes, sizes and permissions. It never
   hardlinks, changes or removes the pilot weights. The other shared models stay
   unchanged. Keep the package disk/RAM admission limits.
3. Verify/finalize/install the candidate **inactive**, through the ordinary
   packaging entry points. The profile's exact rendered unit differs from the
   legacy unit only by `--tts-model-dir .../models/tts` and
   `--wake-stt-confirmation true`. Inline mode and reply preferences come from
   the existing configuration; they are not overwritten by CLI defaults.
4. Freeze a complete, private, coherent copy of the accepted pilot ConfigStore
   after retiring its only writer. Preserve current, every backup, LKG, lock and
   metadata with their owners/modes/times. Do not import just configuration.json.
5. Run the ordinary fresh preparation, adding
   `--operational-store /absolute/frozen/pilot-store`. Keep the existing original
   M19 checkpoint/config/readiness-capsule arguments and the exact Admin assets
   pin. Native CTest, frontend checks and mandatory real systemd file-only
   fixtures remain mandatory. No network dependency installation is performed.

Preparation uses the two actual immutable config executables to prove each whole
store on disposable clones: audit/current/reconcile, every history/LKG document,
revisions, CAS save and restore. Current LKG must already match the ready source.
No live store is changed during preparation. An arbitrary override is rejected;
only the literal existing pinned Amila/debug overrides can be admitted. Unknown
unit edits are never repaired to get a passing result.

The new persistent `/userdata/<supervisor-name>-state` contains:

- `old-checkpoint/` and `new-checkpoint/`, including complete history and LKG;
- `old-readiness.json`, a separately pinned immutable-release recovery capsule;
- `transition.json`, the sealed checkpoint/unit pair.

Retain these directories, their pins, source/build/package checksums and a private
off-device copy before activation. They contain private configuration and must
not be added to Git. The old rollback capsule admits the durable Amila override,
but excludes the temporary `/run` wake-audio debug session.

## Activate and verify

Use only the copied prepared runner's `run ... --owner-authorization ...`.
It retains the original independent 240-second verification / 360-second recovery
supervision. After the service and hardware resources are quiescent, a journaled
whole-directory exchange preserves the replaced tree, publishes only approved
unit bytes and reloads systemd. The existing packaging tool switches the pointer.
The loaded unit/argv, hashes, hardening, catalog revision/hash and sole hardware
owner are checked before service startup and again after readiness.

Recovery also restores a partially migrated store/unit when the release pointer
still names the old executable. Unknown unit bytes block startup. A successful
execution must have fresh independent gates and a disarmed watchdog. Archive the
prepared bundle privately before an authorized reboot, because `/run` is volatile.
After reboot verify the new boot ID, permanent service PID/identity, Runtime/STT
ready, WAITING_WAKE, Admin assets and unchanged settings. No test action should
publish a device command automatically.

## Later owner-authorized recovery

Prefer the active attempt's independent recovery. For recovery on a later boot,
use a **fresh** supervisor and the retained immutable old release, whole checkpoint
and readiness capsule. Never rerun an old `run`, manually switch pointers or raw
overwrite ConfigStore. Keep the newer complete store/history for analysis.

With only the voice service stopped and resources verified free, use the approved
source's existing checkpoint CLI. Substitute the recorded paths and pins:

```sh
sudo -n env FIREFLY_OWNER_ACCEPTED=1 PYTHONDONTWRITEBYTECODE=1 python3 \
  "$SOURCE/scripts/config-checkpoint.py" restore \
  --store /var/lib/firefly/store \
  --checkpoint "$OLD_CHECKPOINT" --checkpoint-pin "$OLD_CHECKPOINT_PIN" \
  --config "$OLD_RELEASE/bin/firefly-config" --authority-release "$OLD_RELEASE" \
  --transition-point "$STATE/transition.json" --transition-pin "$TRANSITION_PIN" \
  --transaction /var/lib/firefly/NEW-unique-rollback-transaction
```

This atomically exchanges whole trees and restores only the sealed old unit
bundle. It does not start the service or switch a pointer. On any error keep the
service stopped; retain the journal and both trees. Retry the same interrupted
transaction when its proof permits; incomplete staging requires a new transaction.

Then run `release-runner.py prepare-rollback --operational` with a new `/run`
root, clean approved source/build and these recorded arguments:
`--checkpoint`, `--checkpoint-pin`, `--config`, `--recovery-capsule` and
`--recovery-capsule-pin`. After PREPARED, execute that copied runner once with
fresh owner authorization. Its stopped-state bootstrap verifies the immutable
release/capsule and loaded unit **before** any start, and uses the same independent
watchdog. Check old readiness, current, revisions/history, CAS and LKG on a private
clone. Keep the preserved newer history and checkpoints; do not delete them for PASS.

The original frozen M19 authority/CLI remains the default. The explicit
operational bootstrap uses a different capsule format and cannot silently relax
the frozen M19 binary, schema, hashes or path guards.
