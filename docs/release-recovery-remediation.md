# Release recovery remediation

This is preparation for Chief Engineer review, **not a new rollout**. The one
authorization recorded in PR #19 was consumed. Production still runs
`wake-beep-dc7b142e-20261011`, with the old grounding vulnerability. Another
deployment requires separate owner authorization. No production stop, restart,
activation, Apply, voice/model change or actuator command occurred in this work.

## Incident: established cause and evidence limit

Original evidence is preserved at
`/home/firefly/firefly-safety-rollout-20261012T084111Z/`.
All 56 files in its original SHA256 ledger were verified before diagnosis.
The [audit bundle](evidence/release-recovery/incident.json) identifies the original
source hashes, JSON pointers/line references and chronological excerpts.

On 2026-10-12 (UTC):

| Time | Observation |
| --- | --- |
| 09:06:47.361636 | Recovery deadline armed; timer active before stop. |
| 09:06:48.142233 | Cutover marker written; current production PID 380056. |
| 09:06:48.202554 | Voice stopped; shutdown completed at 09:06:48.438942. |
| 09:06:51.075170 | PID 380056 reported RKLLM destroyed. |
| 09:06:51.319644 | systemd reported successful deactivation. |
| 09:06:51.327875 | `systemctl stop` returned 0. |
| 09:06:51.407465 | Single-shot resource assertion aborted forward deployment. |
| 09:06:51.575531 | First recovery's stop returned 0; its identical assertion failed. |
| 09:06:51.742409 | Recovery process PID 420757 exited 1; service failed. |
| 09:08:24.720935 | Agent-triggered supervisor re-entry completed stop. |
| 09:08:24.890441 | Original release started; runtime PID 421157. |
| 09:09:14.983726 | Recovery verified READY / WAITING_WAKE and unchanged store. |

Neither the candidate pointer nor candidate runtime was activated. The confirmed
mechanism defect is in the incident controller's `Production.stop/free` and
`recover`: both forward and recovery asserted an immediately empty `fuser`
result; recovery exited on the first refusal. The transient oneshot recovery
service was created without failure restart. Its fixtures assumed instantaneous
resource release and did not exercise this failure.

**The resource holder's historical PID and device are unknown.** The assertion
proves a nonempty parsed PID set, but the old controller did not persist that
set, fuser output, cgroup or process identities. The assertion ran before its
MainPID check, so MainPID=0 at the failure instant is also not recorded. The
journal proves successful shutdown, not resource ownership. The bounded kernel
journal interval contains no corresponding release/error record. No surviving
evidence proves an escaped child, delayed driver release, or false-positive
fuser result. In particular PID 380056 must not be labelled the offending owner.
This missing attribution cannot be reconstructed by restarting production, and
no such reproduction was attempted.

The original algorithm was also executed offline with a simulated delayed
owner: two immediate recovery calls fail without starting the old release;
the corrected algorithm waits and restores. This proves the recovery defect,
not the identity or cause of the historical resource holder.

## Focused mechanism change

`scripts/production-rollout.py` retains the original incident controller's
release/store/build/health gates and single-cutover marker. It is now reviewable
in the repository, rather than an untracked operational script. The resource
barrier and recovery loop live in `packaging/firefly_recovery.py`.

* A bounded barrier requires two consecutive observations of inactive/failed
  service, MainPID=0, ControlPID=0, empty service cgroup including descendants,
  and zero owners of both NPU and microphone. Missing devices, unreadable proc
  data, malformed/failed fuser output and unsupported cgroup layout fail closed.
* Every observation records UTC, owner PIDs, process stat/start identity, cgroup,
  executable and raw fuser output/return status. Non-verbose fuser's normal
  mmap/access markers are accepted; error diagnostics are not evidence of freedom.
* Recovery retries transient failures until the **original** armed absolute
  deadline. A verified old release makes repeat recovery a no-op. If the pointer
  never changed, recovery starts the original release without activation.
* Recovery stops only the recorded cutover worker and the existing Firefly unit;
  it never kills an arbitrary owner PID. A persistent owner or hung child blocks
  switching/starting. The existing unit's stop policy remains unchanged.
* A root-private lock serializes pointer/service operations. The worker is stopped
  before recovery acquires that lock. Probes and commands are bounded by the
  current operation budget; deadline/boot identity survives supervisor re-entry.
  Exit 78 indicates an exhausted deadline and prevents endless supervisor restart.

Application implementation, SafetyGate, ConfigStore, protocol, frontend,
voice tract, models, production unit and packaging activation logic are unchanged.
The controller/helper are operator tools, not installed into the runtime bundle.

## Required supervision for a separately authorized attempt

Do not reuse the consumed incident plan. Before any future stop, prepare a new
root-owned mode-0700 supervisor directory with immutable root-owned copies:
`scripts/production-rollout.py`, `packaging/__init__.py`,
`packaging/firefly_recovery.py`, and the existing packaging tools under
`tools/scripts/stage-release.py` and `tools/packaging/`. Record all tool hashes.
The directory also needs `evidence/`, `production-before.json` and `plan.json`.
Keep the existing verified full backup/release/unit/model/identity checks.

The incident plan fields remain required; additionally record the current
`/proc/sys/kernel/random/boot_id` as `boot_id` and pin the root-private controller
and helper bytes as `controller_sha256` and `recovery_helper_sha256`. After the
timer is armed, obtain its actual `NextElapseUSecMonotonic` from systemd. Finalize
`deadline_monotonic` as that integer divided by 1,000,000; set `armed_monotonic`
to `(NextElapseUSecMonotonic - 240000000) / 1000000`. Keep
`verification_budget_seconds=240` and `recovery_budget_seconds=360`. This avoids
guessing the trigger from the time the agent launched systemd-run. Freeze this
plan before starting the forward worker; do not reset these deadlines on recovery
or supervisor restart. Verify the read-only production `preflight` before downtime.

The independent timer must be active before the single worker can stop anything.
The recovery service must use **Type=exec**, `Restart=on-failure`, `RestartSec=1s`,
`RestartPreventExitStatus=78`, `StartLimitIntervalSec=0`,
`TimeoutStartSec=10s`, `TimeoutStopSec=10s`, `RuntimeMaxSec=360s` and the default
`KillMode=control-group`. These properties apply only to the temporary recovery
supervisor, never to the production unit. Type=exec makes failure restart
available; its absolute plan deadline prevents a process restart extending the
authorized window. For example, in a separately authorized window:

```sh
sudo systemd-run --unit="$ROLLBACK_UNIT_BASE" --on-active=240s \
  --timer-property=AccuracySec=250ms \
  --property=Type=exec --property=Restart=on-failure --property=RestartSec=1s \
  --property=RestartPreventExitStatus=78 --property=StartLimitIntervalSec=0 \
  --property=TimeoutStartSec=10s --property=TimeoutStopSec=10s \
  --property=RuntimeMaxSec=360s \
  /usr/bin/python3 "$SUPERVISOR/scripts/production-rollout.py" rollback "$SUPERVISOR"
```

The controller now **automatically verifies** the actual timer/service settings
before `CUTOVER_STARTED` and the first production stop. It reads typed D-Bus
properties via systemd's `busctl --system --json=short`; it does not parse display
strings from `systemctl show`. The gate requires exact reciprocal timer/service
linkage, loaded transient units, an active/waiting timer, an inactive/dead recovery
service, and no pending jobs/conditions. It verifies root identity, exact ExecStart
argv to this root-private controller in `rollback` mode with this plan directory,
restart/exit-status/timeouts/runtime/kill settings above, and no injected
environment, extra execution hooks or failure/reboot actions. Device/network/proc
isolation that would prevent resource/health checks is rejected; ordinary TERM/KILL
signals and terminating timeout modes are required. Root-controlled
directories/files, pinned code bytes and the original cached plan SHA256 are
rechecked. Missing/malformed/unsupported settings or queries fail closed.

The single `OnActiveUSec=240000000` monotonic trigger must match the frozen plan's
deadline exactly at microsecond precision, with no calendar/randomized trigger,
accuracy at most 250ms and more than 180 seconds of forward budget remaining.
Boot identity and the original 360-second recovery budget must still match.
A rejected preflight creates no cutover marker, stops no service and **does not
start an unverified rollback unit**. Raw accepted settings and plan SHA256 are
recorded in private `supervision-preflight.json`.

Run a separate fixture fault proof before downtime. Retain the timer until independent
production verification succeeds. Keep the original single-worker timeout and
one-cutover marker; do not configure restart on the forward worker. Expired
plans, persistent unknown owners and a nonfunctional systemd cannot be made safe
by bypassing ownership checks: abort before cutover if restoration cannot be
guaranteed. This patch cannot guarantee recovery from an indefinitely wedged
kernel/device owner or failure of PID 1 itself.

## Offline verification

`python3 tests/ReleaseRecoveryTests.py` covers delayed release, hung service
children, foreign owners, false free samples, probe errors/timeouts, original
absolute deadlines, transient recovery failures, supervisor crash after pointer
switch, repeated restoration and both pre-switch/pre-stop rollback cases.
It also checks actual `fuser` with an ordinary temporary file and a child process.
The suite is registered as CTest `ReleaseRecovery`.

An additional real **transient systemd fixture** demonstrated exit 23 followed
by automatic restart into a second PID with PPID=1: exactly one simulated switch
and one simulated start, old state READY, no manual recovery trigger. It uses
ordinary JSON files and never calls production systemctl, NPU, ALSA or HTTP.
See [verification.json](evidence/release-recovery/verification.json) for results,
raw logs and SHA256. This is offline recovery evidence, not a production rollout
or acoustic acceptance. No new Qwen inference is claimed.

The [supervision gate follow-up evidence](evidence/release-recovery/supervision-gate/verification.json)
addresses Chief Engineer review `5489261317`: property/plan failures exercise the
actual `main/deploy` boundary and prove no marker, stop or rollback launch. A
separate actual root-private transient service/timer passed the typed guard
without invoking deployment; its timer was then stopped. Both this proof and
the original recovery fault evidence remain offline, with production unchanged.
Results: 34/34 recovery tests, including 193 rejected deployment configurations
with no marker/stop/control calls, and 82/82 full CTest PASS. The shared recovery
helper is byte-identical to `cd08f53`. Previous evidence and its checksum ledger
remain intact; new raw snapshots, case results and logs have a separate ledger.
