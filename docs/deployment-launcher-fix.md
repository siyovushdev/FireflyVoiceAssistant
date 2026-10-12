# Forward deployment launcher: rollout #2 correction

This PR prepares a launcher only. It authorizes no third rollout. Production
remains on `wake-beep-dc7b142e-20261011`; its grounding vulnerability remains.
The application, ConfigStore, models, voice tract and existing independent
rollback controller/helper/supervision gate are unchanged.

## Exact failure

The second rollout used an untracked shell/STDIN wrapper. Its complete command
was recovered from the original Codex execution record, not reconstructed from
the final summary. See the [incident evidence](evidence/deployment-launcher/incident.json)
for record/file hashes and the [original wrapper](evidence/deployment-launcher/original-launcher-command.txt).
It submitted the worker as `systemd-run --unit=firefly-safety-cutover2-20261012T111230Z
--property=Type=oneshot --property=TimeoutStartSec=240s
--property=TimeoutStopSec=10s --property=KillMode=control-group
/usr/bin/python3 /run/firefly-safety-rollout2-20261012T111230Z/scripts/production-rollout.py
deploy /run/firefly-safety-rollout2-20261012T111230Z`, then executed:

```python
output = ops.run(worker, timeout=15)
launched = True
```

Without `--no-block`, systemd-run waited for the oneshot start job, which includes
the deployment operation. At 11:32:05.253790 UTC the Python wrapper raised
TimeoutExpired, retaining `launched=false` even though CUTOVER_STARTED existed.
Python 3.10 subprocess.run kills and reaps its client on this timeout; the original
client PID was not recorded. That did **not** stop the PID-1-owned forward worker
PID 467296. It completed activation and started candidate PID 467543 at 11:32:37.
The mandatory rollback service PID 467718 was explicitly triggered at 11:33:09;
systemd then terminated the forward worker with TERM. The original release
returned READY/WAITING_WAKE as PID 467978 at 11:34:50.793540. This was an operator
launcher failure, not proof of a SafetyGate or resource-barrier defect.

## Minimal fix

[`scripts/deployment-launcher.py`](../scripts/deployment-launcher.py) separates
submission (`start`) from observation (`observe`); `run` performs both. Submission
uses `systemd-run --no-block`, with the existing worker type, stop policy and
240-second start timeout. `RemainAfterExit=yes` retains successful terminal status
so systemd cannot garbage-collect the completion evidence before observation.
This applies only to the forward worker. The recovery service/timer are unchanged.

The 15-second timeout now bounds submission only. A failed/uncertain client result
is recorded without retrying the launch or treating the worker as completed.
Observation uses actual typed systemd Unit/Service properties, including pending
job, PID, result and exit status, until the **original** plan deadline. Exit zero
requires ACCEPTED, deployed-verified evidence and the existing production gates
before DEPLOYED_VERIFIED. Unconfirmed probes or elapsed deadlines cannot pass.
No launcher path stops a worker or disarms/changes the independent watchdog.

Before a separately authorized attempt, copy the reviewed launcher into the new
root-private supervisor as `scripts/deployment-launcher.py` and include its exact
Git-reviewed SHA256 as `launcher_sha256` in the frozen original plan. Keep all
existing build/backup/timer fixture checks and the automated supervision_gate.
The launcher checks its own pinned root-owned copy and creates an exclusive
FORWARD_LAUNCH_STARTED claim before submission; duplicate or uncertain launches
must not be resubmitted. Replace the temporary Python/systemd-run launch wrapper
with this checked-in entry point:

```sh
# Only after new owner authorization and the complete existing preflight.
sudo /usr/bin/python3 "$SUPERVISOR/scripts/deployment-launcher.py" run "$SUPERVISOR"
```

`start` may instead be followed by `observe` in a separate process using the same
directory/plan. Do not wrap `run`/`observe` in a 15-second client deadline. The
original systemd timer/rollback controller remain armed during observation and
own restoration after failures, agent loss or deadline expiry. Independently
verify production before the existing authorized watchdog-disarm step.

## Verification

See [verification.json](evidence/deployment-launcher/verification.json) and the
linked raw file-only evidence. The real systemd fixture uses uniquely named
transient units, private `/run` files and ordinary Python processes. It never
opens NPU/ALSA, uses the production service/API, switches a release or writes the
production store. It reproduces the old 15-second client timeout while the worker
continues, then verifies new submission, completion after more than 17 seconds,
nonzero worker exit, immediate startup failure, observer timeout and subsequent
independent timer restoration. Test assertions distinguish expected failures
from test failures. The initial fixture attempt and its test-only corrections
are retained with the final evidence.

```sh
ctest --test-dir build -R '^(ReleaseRecovery|DeploymentLauncher)$' --output-on-failure
sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 tests/DeploymentLauncherSystemdTests.py PRIVATE_NEW_EVIDENCE_DIR
```

No new C++/frontend build or hardware acceptance is claimed for this operator-only
change. Targeted tests and read-only before/after production comparisons provide
the scope-appropriate checks. A third rollout still requires separate authorization.
