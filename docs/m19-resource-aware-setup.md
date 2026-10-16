# M19 Resource-Aware AI Setup

> Dated milestone/design record. Its test results and deployment statements apply to the recorded revision, not today. For current M19 identity, architecture and operating instructions, start at [the production baseline](production-baseline.md) and [README](../README.md). Historical results below are preserved.

## Qualification — 2026-10-14

Real Admin Qwen3-4B ctx4096, original AI Setup, private Review / one Apply,
Runtime restoration and an instrumented human ON command passed. The owner
separately confirmed ON and OFF; instrumented OFF remains unverified. Overall
acceptance is PARTIAL_PASS, with no full-context stress or rollout qualification.
The final source/build identity and validation are recorded in
[the finalization record](m19-finalization.md) and
[the evidence bundle](evidence/m19/resource-aware-20261014/README.md).

## Historical feasibility gate — before implementation, 2026-10-13

Decision at that time: a bounded lifecycle fix was technically justified;
hardware qualification was outstanding. No hardware-limit conclusion follows from the M19 failure:
its 6 GiB pre-load admission rejected the request before Admin init.

The preserved September 30 trial used the same Admin4096 artifact and one Admin
STT on this RK3588S 8 GB. Two setup cycles completed, peak RSS/PSS was
5,149,668/5,147,969 KiB, and minimum sampled MemAvailable was 1,916,952 KiB.
See `benchmarks/dialogue-setup/rk3588-results.json`. This was a smaller workload
than the current live service, not a full-context stress test.

The October 13 production snapshot had 3.843 GiB in four DRM DMA buffers,
5.023 GiB charged to the service cgroup, and approximately 1.67 GiB globally
available. Those overlapping measurements must not be added. The live service
retained two STT instances, including the unused command STT/VAD during Setup.
No snapshot exists at the failed admission point. Neither a leak nor a precise
recoverable-byte count was established.

Thus the implementation removes the known unnecessary owners and measures the
remaining budget. It does **not** lower 6/1 GiB or promise admission under current
background load. If the budget is still unavailable, entry fails closed.

## Ownership and transitions

- `RuntimeGenerations` closes command admission before draining voice work.
- `VoiceRecovery::drain` waits for the entire audio lease, including workers and
  failure callbacks, then releases idle command STT/VAD under the same mutex that
  admits a new voice attempt. This also works during microphone retry backoff.
  Ordinary microphone retries retain their CPU models.
- `SetupResources` serializes Admin STT, model init/context refresh/inference,
  teardown, and voice model creation/destruction. Existing Admin STT finishes;
  new transcriptions during a model operation receive the existing `stt_busy`.
  Admin STT itself stays resident. Its recognition settings are unchanged.
- The process-wide RKLLM lease rejects a second owner before `rkllm_init`, including
  partial initialization. Successful destroy releases the lease; uncertain
  destruction retains it and never retries that handle, including in its
  destructor. This is an in-process guarantee, not a lock respected
  by unrelated/older executables on the board.
- A drain timeout does not initiate model restoration over still-active voice
  resources. Failed restoration keeps command admission closed. Successful model
  restoration allows the existing voice recovery loop to recreate STT/VAD/wake
  and report WAITING_WAKE only after its normal ready callback.
  A previously admitted voice command completing after a failed drain cannot
  execute hardware while the transition is in RESTART_REQUIRED.

## Memory policy and diagnostics

No background sampling thread or new dependency is introduced. Bounded phase
records (`SETUP_MEMORY`) contain MemAvailable, process PSS, own DRM DMA-buffer
bytes, leaf cgroup usage, the minimum ancestor cgroup headroom, and required KiB.
They contain neither prompts nor audio. cgroup v2 memory.max and memory.high are
both respected conservatively; unknown required accounting fails admission.

The 6 GiB check runs before model verification and again before init, including
after verification's file-cache activity. The 1 GiB floor runs after Admin init
and before/after every Admin bootstrap or inference, including context refresh.
Runtime loading/bootstrap also checks the 1 GiB floor, so restoration cannot
report READY after an observed reserve violation.
Voice CPU model creation checks the same floor before/after loading; a failed
post-load check releases those newly created owners and leaves voice unavailable.
An additional bounded 2-second check confirms that this process's DMA descriptors
return to their pre-model baseline after destroy. Missing evidence or a timeout
prevents another load through that model owner. A descriptor baseline does not
prove that every driver allocation has already returned to MemAvailable.
Admission additionally allows up to two seconds for observable memory reclamation
to reach the unchanged 6 GiB budget. It logs only entry and outcome, not every
poll; expiration remains a memory-shortage rejection, distinct from retained DMA.

The floor is not an allocation reservation or an OOM guarantee: a transient peak
inside a synchronous vendor call, other processes, and driver allocations still
need hardware measurement. Vendor init/destroy hangs cannot safely be turned into
hard time bounds by detaching threads. Existing joined inference watchdog rules
remain in effect; no forced thread termination was added.

Public worker error codes distinguish memory shortage/accounting failure,
resource/audio drain timeout, DMA release timeout, RKLLM ownership/runtime/deadline
failure, and restore failure. Detailed vendor causes stay in diagnostics; config
identity and fail-closed admission remain authoritative.

## Verification boundary

Offline tests use the real adapters and orchestration with a fake RKLLM C API,
fake voice devices, small synthetic memory files, and private temporary stores.
No real model, microphone, NPU or hardware command is needed. Existing Setup
Cancel/Apply/CAS/rollback coverage remains part of the CTest gate.

No guaranteed byte saving is claimed: destroying one redundant recognizer/VAD is proven by
ownership tests, but the allocator's return of RAM requires physical measurement.
The 3.843 GiB DMA measurement describes the previous runtime, not newly saved RAM.

Any further physical qualification requires new owner permission and an exclusive
test device/window. The bounded October 14 result does not cover the following
additional stress/failure qualification. Capture phases before/after voice release, destroy, verification,
init, bootstrap, requests and restoration; include repeated cycles, maximum
permitted inputs, Admin STT, background load and injected failures. Require no
overlapping handles, no growing DMA/PSS baseline, no OOM/allocation failures,
the retained OS reserve, unchanged config on failure, and restored WAITING_WAKE.
Offline PASS does not establish Admin4096 hardware reliability or authorize rollout.

## Historical offline verification result — 2026-10-13

Build: `cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64`,
then `nice -n 10 cmake --build build -j1` succeeded.

The initial complete CTest run passed 86 of 88 suites. ProductionPackaging hit
the workspace disk-space guard; VoiceRecoveryIntegration lacked the required
HTTP Origin/admin headers in its new test request. The test headers were fixed.
Only generated build caches created during this stage were compacted to recover
space; production and prior evidence were preserved.

After the final source changes, all 14 targeted suites passed (211.89 seconds):
RkllmEngineFailures, RkllmContextRefresh, MockExecutionUnifiedRuntime,
ResourceAwareSetup, AdminTranscribe, DialogueSetup, AdminGuardedCommand,
AdminSetupV2Chromium, M19DemoVerticalSlice, AdminSetupV2Api, VoiceRecovery,
ProductionStartup, WakeAcknowledgementOptions and VoiceRecoveryIntegration.
ResourceAwareSetup reported 72 parent-process checks, plus terminal destroy
failure assertions in an isolated child. ProductionPackaging subsequently
passed separately (61.25 seconds). Thus all 88 suites have successful coverage
across the full run and reruns; this was not a single final 88/88 invocation.

Logs are preserved under `build/resource-aware-evidence/`. Generated object files
and static libraries are preserved in the verified archive
`build/resource-aware-link-cache-20261013T110914Z.tar.gz`; final executables remain
available. A future build can regenerate those intermediates or restore them
from that archive.

At the end of that historical stage, the read-only production check showed: service active/running, PID 773964,
NRestarts=0, release pointer still
`releases/grounding-818157ec-20261013T040322Z`. No production rollout, real RKLLM
inference, model/configuration change, commit, push or PR was performed.

## Follow-up: Kimi K1 and controlled hardware preparation

K1 is fixed through compile-time synthetic SetupResources injection into the
three DemoMain fake-RKLLM fixtures only. The real appliance has no environment
or CLI override. Seven affected/related CTest suites passed normally and with
MemAvailable=0 in a private mount namespace. Production meminfo is unchanged.
Supervisor sampling no longer treats one global low-memory observation as an
extra admission threshold; product phase checks and OOM events remain enforced.
The current 14-suite / 36-test gate supersedes these historical preparation results.
See [the current controlled test plan](m19-controlled-hardware-preparation.md).
