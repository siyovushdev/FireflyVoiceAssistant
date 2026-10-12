# Reproducible supervised release procedure

This preparation authorizes **no fifth rollout**. Production remains on
`wake-beep-dc7b142e-20261011`; the new SafetyGate is not installed. A future
attempt requires reviewed code and a new explicit owner authorization.

## Rollout #4: established cause and evidence limit

The untracked preparation wrapper added `assert 'NRestarts=1' in props` after
an additional real restart fixture. It attempted to prove `Restart=on-failure`
beyond the already reviewed mandatory tests and typed supervision gate. This
snapshot assertion was not part of a versioned, tested release procedure.
Its failure aborted before production was stopped: downtime was zero.

The first `systemctl show` response was not preserved. Therefore its returned
counter and the immediate reason for the assertion failure remain unknown.
The original journal proves exit 23 followed by PID-1 restart counter 1;
fixture PIDs 497549 and 497570 restored the simulated old release. A later
`LoadState=not-found` / `NRestarts=0` response is not evidence of the first
response. See [incident.json](evidence/release-procedure/incident.json), including
the original file hashes and precise invocation reference. Originals remain intact.

## Fixed preparation and execution entry points

[`scripts/release-runner.py`](../scripts/release-runner.py) coordinates the
existing packaging verifier, production controller, recovery helper and
asynchronous launcher. Those reviewed mechanisms are unchanged. No fixture
adapter is included in the production recovery bundle.

First build the approved clean merge commit with the existing pinned native
voice/Admin dependencies, then stage, verify and install a **new immutable
release directory** using the [existing packaging procedure](m18-relocatable-production-packaging.md).
Do not activate it, replace models, change the unit or delete the original release.
The build directory must retain the full applicable CTest executables and the
frontend must retain its installed dependencies. Preparation reruns the existing
CTest and frontend typecheck/test/build commands as `firefly`, without downloads.
Use the [hotfix build instructions](production-grounding-hotfix.md).

Use only these two checked-in entry points; substitute the approved absolute
source path, full merge SHA, staged release ID and a new unique `/run` directory:

```sh
# Non-disruptive preparation; no service stop or pointer activation.
sudo -n env PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$SOURCE/scripts/release-runner.py" prepare \
  --source "$SOURCE" --approved-commit "$APPROVED_COMMIT" \
  --new-release "$NEW_RELEASE" --root "$SUPERVISOR"

# Only within a separately owner-authorized window, after PREPARED.
sudo -n env PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$SUPERVISOR/scripts/release-runner.py" run \
  "$SUPERVISOR" --owner-authorization "$OWNER_AUTHORIZATION_URL"
```

`SOURCE` must be canonical and clean. `SUPERVISOR` must be a new direct child of
`/run`, for example `/run/firefly-release-YYYYMMDDTHHMMSSZ`. Preparation pins
the tracked runner and copied tools, verifies installed old/new release identity,
source fingerprint, identical Admin assets, libraries, wake artifacts, unit,
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

Run the operator unit tests and the complete real systemd file-only coordinator
suite with a fresh private evidence destination:

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
