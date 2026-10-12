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
`/proc/sys/kernel/random/boot_id` as `boot_id`. Set `deadline_monotonic` to the
end of the 240-second forward-verification budget, and
`recovery_budget_seconds=360`. These deadlines must not reset on recovery or
supervisor restart. Verify the read-only `preflight` mode before downtime.

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

Verify the actual timer/service properties and run a separate fixture fault
proof with these properties before downtime. Retain the timer until independent
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
