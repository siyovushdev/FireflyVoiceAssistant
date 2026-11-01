# Current supervised release and recovery procedure

Start with the [dated production baseline](production-baseline.md) and [build guide](build-and-test.md). Current known release is `m19-c7fa1490-20261015T150156Z`; rollback is `m19-320b80a6-20261014T173500Z`. Recheck live identities before any future preparation. This document grants no activation permission.

Large Runtime/Admin/STT/VAD models stay under `/opt/firefly/voice-assistant/models`. Stage using the existing `--production --reuse-installed-models` contract. From the [Amila revision](amina-amila-release.md), the small checked-in Amina model is packaged as a regular file: leave `FIREFLY_WAKE_MODEL_REFERENCE` empty and omit `--wake-reference-release` for its first release. An optional no-copy wake reference must point to an immutable release containing the exact **current** model; the original grounding anchor remains protected for old releases and rollback. See the [M19 large-model contract](m19-production-preparation.md). `install-release` creates an inactive immutable release only. It is not an activation, and a read-only audit does not run it.

Pin the new assets fingerprint from reviewed source and actual built dist in `APPROVED_ADMIN_ASSETS_SHA256`. New assets need not equal old assets. Runner/ELF/manifest/fresh dist must all match that explicit pin; omitting it deliberately preserves the legacy old/new equality gate. Never disable integrity verification. For installed c7 the pin is `fd3cbf4dd6403204f35a8759c8678282d37958854d7654117c5cdc29a01110f9`; a future frontend change requires its own reviewed pin.

Keep installed active/rollback releases and the grounding Amina anchor. All old prepared/consumed plans are historical and boot-bound. Prepare a fresh recovery bundle for each separately authorized attempt; no old START is reusable. The historical rollout #4 failure is preserved [separately](history/release-procedure-rollout4.md), with original evidence unchanged.

## Fixed preparation and execution entry points

[`scripts/release-runner.py`](../scripts/release-runner.py) coordinates the
existing packaging verifier, production controller, recovery helper and
asynchronous launcher. These existing mechanisms include the reviewed M19 model-reference and assets-pin changes. No fixture
adapter is included in the production recovery bundle.

First build the approved exact clean product commit with the existing pinned native
voice/Admin dependencies, then stage, verify and install a **new immutable
release directory** using the [existing packaging procedure](m18-relocatable-production-packaging.md).
Do not activate it, replace models, change the unit or delete the original release.
The build directory must retain the full applicable CTest executables and the
frontend must retain its installed dependencies. Preparation reruns the existing
CTest and frontend typecheck/test/build commands as `firefly`, without downloads.
Use the [current build instructions](build-and-test.md). Earlier hotfix reports are historical.

Offline application checks and mandatory fixtures strip `FIREFLY_OWNER_ACCEPTED`
even when inherited from the operator. Only the two read-only installed-package
`verify` invocations explicitly receive live-root permission. This does not
authorize `run` or a deployment; see the
[environment-isolation evidence](evidence/release-runner-auth-env/README.md).

Use only these two checked-in entry points; substitute the approved absolute
source path, full approved product SHA, staged release ID and a new unique `/run` directory:

```sh
# Non-disruptive preparation; no service stop or pointer activation.
sudo -n env PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$SOURCE/scripts/release-runner.py" prepare \
  --source "$SOURCE" --approved-commit "$APPROVED_COMMIT" \
  --new-release "$NEW_RELEASE" --root "$SUPERVISOR" \
  --approved-admin-assets-sha256 "$APPROVED_ADMIN_ASSETS_SHA256"

# Only within a separately owner-authorized window, after PREPARED.
sudo -n env PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$SUPERVISOR/scripts/release-runner.py" run \
  "$SUPERVISOR" --owner-authorization "$OWNER_AUTHORIZATION_URL"
```

`SOURCE` must be canonical and clean. `SUPERVISOR` must be a new direct child of
`/run`, for example `/run/firefly-release-YYYYMMDDTHHMMSSZ`. Preparation pins
the tracked runner and copied tools, verifies installed old/new release identity,
source fingerprint, explicitly pinned new Admin assets, libraries, wake artifacts, unit,
seed and shared-model contract. It creates a root-private mode-0700 recovery
bundle and a complete rights-preserving ConfigStore backup with file SHA256 and
metadata. Existing readiness, configuration and sole-owner gates must pass.
Mandatory operator tests are a fixed list: launcher, recovery, runner, and the
five existing real systemd file-only fixtures. Every mandatory command's original
response is saved before evaluating its return code.

Execution refuses a changed bundle or repeated invocation. It rechecks the old
runtime, submits the independent timer/service, atomically freezes the original
boot-bound plan from the real timer, and passes the existing typed
`supervision_gate` before submitting any forward worker. Verification budget is
240 seconds; recovery budget is a further 360 seconds. Actual linkage, ExecStart,
Type, restart policy, exit-78 behavior, deadlines, protection and code/plan pins
remain mandatory. The unchanged resource barrier requires confirmed empty
service cgroup and NPU/microphone ownership before switching.

Runtime generation IDs are local to one process. Before cutover, the complete
active identity must equal the captured baseline. After the supervised cutover
and a different worker PID, startup generation1 is required for either the new
release or recovery of the old release. Revision/hash, stored configuration,
bytes/owners/modes, binary/argv/library/unit and sole-owner checks stay exact;
a changed generation in the original process or an unplanned pre-cutover reset
does not qualify as a restart.

The reviewed launcher submits asynchronously and observes completion within
the original deadline. Its 15-second limit applies to submission only. Do not
add an outer 15-second timeout, temporary shell/Python launcher, live assertion
or additional gate. Final success requires the controller's acceptance and a
fresh independent pass of the existing gates; final restoration requires fresh
old-release gates. Only then is this bundle's timer disarmed.

Optional unit diagnostics, including any displayed NRestarts value, are recorded
without changing authorization. An unavailable diagnostic does not negate a
verified release. Missing mandatory proof always blocks or requires recovery.
Agent loss, worker failure and observation uncertainty leave the original
independent watchdog responsible for recovery. Unknown recovery is reported
`BLOCKED_UNKNOWN`; it is never converted to success and the watchdog stays armed.
Never reuse a consumed directory, reset its deadline, retry a launch, stop foreign
processes or restore the ConfigStore by overwriting it.

## Offline validation and limits

CPU unit tests are safe in an isolated build. The real systemd coordinator suite
is a separately scoped integration procedure; do not run it merely to audit
production. For authorized release preparation, use a fresh private evidence destination:

```sh
ctest --test-dir build -R '^(ReleaseRecovery|DeploymentLauncher|ReleaseRunner)$' --output-on-failure
sudo -n env PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 tests/ReleaseRunnerSystemdTests.py "$NEW_EVIDENCE_DIR"
```

The coordinator suite runs the fixed mandatory preparation suite, then exercises
successful work lasting over 15 seconds, candidate startup failure, abrupt forward
worker exit, loss of the runner process, failed optional diagnostics, and a crashing
rollback process restarted by PID 1. The real 240-second independent timer and
original 360-second recovery budget are retained. Each case checks unchanged store
metadata, final file-only readiness, one forward switch, repeated-launch rejection
and the unchanged plan. Restart evidence uses durable results and journal records,
not an ephemeral NRestarts snapshot assertion.

The fixture substitutes only runtime/package/hardware boundaries with private
files and ordinary CPU processes; the real controller, launcher, systemd supervision
gate and recovery flow run unchanged. Preparation's production identity/backup
branches also have isolated unit tests. No fixture opens production NPU, ALSA,
ConfigStore or Admin API. This proves the operator procedure, **not** a fresh
production C++/Qwen/audio acceptance. Full native build and application checks
remain mandatory preparation for the next authorized release.

See [verification.json](evidence/release-procedure/verification.json) for results,
original evidence references and SHA256, and `SHA256SUMS` for the published bundle.
