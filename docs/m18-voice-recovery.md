# M18.2 — Voice Recovery

Baseline: `53f040214a03a61d4060569e61198c02a897baa7` (merged M18.1 COMPLETE / PASS).
Branch: `hardening/voice-recovery`. Owner hardware acceptance: **COMPLETE / PASS**
(final evidence supplied by the owner, recorded 2026-10-09).
M18.1 is not reopened. No production store changes, service installation, host
reboot, firmware operations, commit, push, PR or merge are part of this task.

## Defect and ownership audit

Previously `src/admin/DemoMain.cpp` caught voice startup/runtime exceptions,
published unavailable voice/microphone and failed wake, destroyed the attempt's
objects, cleared `audioActive`, and notified the drain condition variable. It then
executed `if (!runtime.setupBlocked()) break;`. With normal setup admission open,
the service thread ended permanently. Admin/runtime survived, but reconnecting a
microphone could not start another voice session without a process/service restart.

The architecture remains one systemd-supervised `firefly-demo` process. The existing
service thread owns voice recovery; systemd only supervises process failures.
Recovery never reloads RKLLM, changes configuration, invokes hardware actions, or
changes command parsing, SafetyGate, LanguageIndex, bindings or the Admin UI/API.

`VoiceRecovery` now owns the attempt/backoff loop and audio drain synchronization.
`DemoMain` supplies the existing runtime/health callbacks and concrete voice path.
The same loop handles initial missing microphone/open failure, capture/read
exceptions, wake-worker errors, and the existing two-second wake audio stall.
Unexpected normal session completion also backs off instead of spinning.
Command rejection, unsupported input, STT command errors and safety rejection stay
command results; they do not cause recovery attempts.

## Lifecycle and retry policy

```text
setup blocked ── wait (no audio) ── setup released
                                     │
                                     v
                             loading / opening
                                     │
                      capture + wake initialized
                                     v
                    connected / wake ready / WAITING_WAKE
                                     │
                  infrastructure exception + full teardown
                                     v
                unavailable / wake failed / health degraded
                                     │
                    bounded backoff (no audio ownership)
                                     └──────────> loading / opening
```

Delays after consecutive unstable failures are **1, 2, 4, 8, 10, 10… seconds**.
Retries continue while the process is running, with a maximum delay of 10 seconds.
`steady_clock` deadlines avoid wall-clock changes. Waits are at most **50 ms** per
slice; stop and setup admission are checked before each attempt and wait slice.
No journal message is emitted per polling slice.

The first `WAITING_WAKE` (or `listening` with explicit wake disable) marks session
readiness. A session lasting at least **10 seconds after readiness** resets the
next failure's backoff to one second. Merely opening and immediately stalling does
not reset it. Repeated rearms within a session do not restart this stability timer.
As before, WAITING_WAKE means capture opened/started and wake initialized; it does
not prove future PCM delivery. A subsequent two-second stall degrades and retries.

Expected reconnect latency is the remaining backoff (at most 10 seconds), plus
capture/wake initialization and any prior attempt teardown. This is not a hard
wall-clock guarantee against vendor hangs. A setup block delays reopening until
it is released; an already elapsed retry deadline can then run immediately.

Journal evidence:

- `VOICE_RECOVERY failure reason=...` — exact infrastructure exception;
- `VOICE_RECOVERY scheduled delay_ms=...` — one scheduled delay per failure;
- `VOICE_RECOVERY retry attempt=...` — one actual retry start;
- `VOICE_RECOVERY recovered attempt=...` — first ready state after an outage.

The attempt counter resets after a stable session. The initial successful startup
uses the existing VOICE_STATE evidence; it is not labeled a recovery.

## Resource lifecycle, setup and shutdown

- Voice Zipformer recognizer and Silero VAD are initialized lazily and retained
  on the service thread after successful construction. Failed construction is
  retried. Recognition already uses per-request streams; VAD explicitly resets
  before every new segmenter/session, discarding old PCM and recurrent state.
  These CPU models also remain resident while setup is blocked; they perform no
  voice work then. The existing Admin model memory-admission check is unchanged.
- ALSA capture, Amina worker, segmenter/buffers and acknowledgement object are
  local to each attempt. The accepted wake-before-capture initialization order is
  preserved: ALSA starts PCM in its constructor, so waiting for wake initialization
  after opening capture could overrun its short buffer. Each retry gets a fresh
  wake IPC/model state, including retries after a microphone open failure. The
  small wake worker is recreated to keep failed IPC/streaming state and setup
  drain ownership simple; heavyweight STT/VAD reloads are avoided.
- Wake-loop futures join during both normal shutdown and exception unwinding;
  Amina destruction reaps the owned child, and ALSA/playback destructors close
  their devices before audio ownership is released. No queued utterance is replayed.
- Beep failure remains diagnostic: the wake loop reports ACKNOWLEDGEMENT_FAILED,
  drains playback audio and continues the admitted command session. It does not
  restart healthy capture. Acknowledgement health clears only after a successful
  beep, so an independent playback failure can keep overall health degraded.

The setup admission check and transition to active audio share the mutex used by
`drain()`. RuntimeGenerations closes setup admission before invoking drain. If an
attempt already acquired audio, drain waits for its complete teardown; otherwise
the blocked check prevents a new owner. `audioActive` is true only during an
attempt (including initialization/teardown), false between attempts, during
backoff and while waiting for setup. `audioChanged` notifies on release.

Intentional setup/model switching is not classified as an outage. It interrupts
the current loop and prevents reopening until setup releases the block. No retry
penalty is added for a normal setup drain. Existing request admission and Apply
semantics remain unchanged; recovery never calls Apply or switches the model.

SIGTERM still only sets the signal flag. The service thread observes it within a
50 ms wait slice during backoff, without reopening audio. Active voice uses the
existing stop predicate and joined teardown. Main retains `beginShutdown`,
runtime stop, service join, client completion, `SHUTDOWN complete`, and model
destruction. The existing 45-second audio drain budget is unchanged. Vendor
initialization/inference hangs and existing bounded child teardown remain separate
limits; the retry mechanism does not forcibly terminate vendor calls.

## Health and automated verification

Existing status fields/schema are unchanged. During an outage voice/microphone
are `unavailable`, wake is `failed` with the actual reason, overall is `degraded`,
and Admin/runtime remain available. On retry voice=`loading`, microphone=`opening`,
wake=`starting`; the existing health aggregation can report `starting` during
initialization. At WAITING_WAKE microphone=`connected`, wake=`ready`, overall is
`ready` provided no independent subsystem/configuration failure remains.

The tests use no real NPU/microphone, installed service or production store:

- `VoiceRecovery`: injected monotonic clock/waits check repeated failures, exact
  bounded delays, immediate-failure versus stable-session reset, shutdown during
  first/capped backoff, unexpected session return, and setup blocks during retry.
  A threaded test uses real RuntimeGenerations setup/drain/restore with a fake
  model, including delayed audio cleanup and the single-owner invariant.
- `VoiceRecoveryIntegration`: real DemoMain, HTTP, recovery and wake loop linked
  with fake voice devices and fake RKLLM. Temporary control files inject absent
  capture, read failure and PCM stall. It checks same PID, degraded/ready status,
  later attempts, CPU model reuse, balanced capture/wake destruction, untouched
  configuration bytes, zero executions and SIGTERM/destruction. The fake wake
  constructor rejects initialization over an already-running capture, guarding
  the accepted initialization order.
- `ProductionStartup` retains its fail-soft/empty-store/preserved-store tests.
  Its old assertion forbidding retries is replaced with bounded retry evidence.
  `ProductionStartupContract` remains unchanged.

Build/test commands (existing configured offline dependencies):

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j2
ctest --test-dir build -R 'VoiceRecovery|ProductionStartup|HealthReliability|VoicePipeline|WakeVoiceGating|SeamlessWake|WakeSessionUx|AminaWorkerLifecycle|WakeLiveHelper' --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

Implementation-stage automated results (2026-10-08):

- Configure/build PASS, no compiler warnings in the final build log.
- Focused suites: **11/11 PASS, 36.08 s**. VoiceRecovery **0.13 s**,
  VoiceRecoveryIntegration **18.29 s**, ProductionStartupContract **0.38 s**,
  ProductionStartup **5.55 s**, HealthReliability **9.50 s**; all required voice,
  wake, worker and helper suites passed.
- Full CTest: **73/73 PASS, 273.67 s**. Registered Chromium suites all PASS:
  AdminV2Chromium **63.27 s**, AdminManualChromium **46.29 s**,
  AdminSetupV2Chromium **42.30 s**. B4 frontend source is unchanged.
- An earlier full run was **72/73**: AdminSetupV2Api received `409 setup_busy` at
  `/api/setup/resume`. Its isolated rerun passed (**2.28 s**), as did the final
  full run (**2.28 s**). That API/test was not modified as part of M18.2.
- `git diff --check` PASS; new untracked files also passed a separate whitespace
  check. `ldd` confirmed both new test executables require no RKLLM/ALSA/sherpa
  runtime library. No live hardware acceptance was executed by the agent; the
  subsequent real owner acceptance is recorded below.

Final logs: `/tmp/firefly-m18-2-build-final.log`,
`/tmp/firefly-m18-2-focused-final.log`, `/tmp/firefly-m18-2-ctest-final.log`.
The earlier full-run result is preserved in `/tmp/firefly-m18-2-ctest.log`.

Documentation finalization verification (2026-10-09): focused suites **11/11 PASS,
35.95 s**, including VoiceRecovery, VoiceRecoveryIntegration,
ProductionStartupContract and ProductionStartup; full CTest **73/73 PASS,
271.47 s**, including all three registered Chromium suites. `git diff --check`
passed. Implementation files remained unchanged during this finalization.
Logs: `/tmp/firefly-m18-2-owner-focused.log` and
`/tmp/firefly-m18-2-owner-ctest.log`.

## Final real owner acceptance — COMPLETE / PASS

The following production evidence was verified and supplied by the owner.
The agent did not repeat the hardware/service steps during this documentation
finalization.

### 1. Runtime unplug/replug recovery — PASS

Initial healthy state:

- MainPID=144526;
- NRestarts=0;
- runtime_ready=true;
- voice=WAITING_WAKE;
- microphone=connected;
- overall health=ready;
- wake=ready.

Physical XVF3800 unplug:

- `lsusb` confirmed device ABSENT;
- same MainPID=144526;
- NRestarts=0;
- service stayed active;
- runtime stayed ready;
- voice=unavailable;
- microphone=unavailable;
- overall health=degraded;
- wake failed.

Observed recovery policy in the real journal: delay **1000 ms**, **2000 ms**,
**4000 ms**, **8000 ms**, then **10000 ms**.

After physical reconnect:

- first reopen could transiently report `Device or resource busy`;
- retries continued safely;
- same MainPID=144526;
- NRestarts=0;
- `VOICE_STATE WAITING_WAKE`;
- overall health returned to ready;
- `VOICE_RECOVERY recovered attempt=6`;
- no `systemctl restart` was used.

Post-recovery real voice cycle:

- Amina detected;
- `BEEP_START -> BEEP_END`;
- `LISTENING_COMMAND`;
- `SPEECH_START / SPEECH_END`;
- transcript exactly **"какая погода"**;
- empty domain safely returned **Unsupported / target_not_grounded**;
- returned to WAITING_WAKE;
- MainPID remained 144526;
- NRestarts remained 0.

### 2. Missing microphone at service startup — PASS

Before start, `lsusb` confirmed XVF3800 ABSENT.

Started service with microphone absent:

- MainPID=167662;
- NRestarts=0;
- service active;
- runtime_ready=true;
- runtime ready;
- STT ready;
- voice=unavailable;
- microphone=unavailable;
- overall health=degraded;
- wake failed.

Recovery retries continued with bounded backoff. XVF3800 was then physically
connected **WITHOUT `systemctl restart`**.

Result:

- same MainPID=167662;
- NRestarts=0;
- runtime_ready=true;
- microphone=connected;
- voice=WAITING_WAKE;
- overall health=ready;
- wake=ready;
- `VOICE_RECOVERY recovered attempt=4`.

A transient `Device or resource busy` immediately after USB enumeration was
tolerated; the next retry recovered successfully.

### 3. Shutdown during maximum recovery backoff — PASS

With microphone absent, the journal reached
`VOICE_RECOVERY scheduled delay_ms=10000`. The owner then ran `systemctl stop`
during that backoff.

Observed:

- MainPID before stop=167662;
- NRestarts=0;
- stop_elapsed=2.182 sec;
- service became inactive;
- process exited status=0/SUCCESS;
- no firefly-demo process remained;
- no amina_worker remained;
- port 8080 was released;
- `SHUTDOWN complete`;
- RKLLM destroyed;
- systemd reported `Deactivated successfully` / `Stopped`.

This proves the 10-second recovery wait is interruptible by shutdown and does not
delay graceful stop for the full backoff interval. The measured 2.182 seconds is
the complete service stop, including teardown, not the recovery polling interval.

### 4. Final M18.2 conclusion

**M18.2 owner acceptance COMPLETE / PASS.** Recovery occurs inside the same
`firefly-demo` process. A systemd process restart is not used for transient
microphone recovery, and no `systemctl restart` is required after reconnect.
No `xvf_host REBOOT`, USB unbind/bind, VBUS reset, firmware flashing or firmware
2.0.6 workaround was added to Firefly recovery.

**XVF3800 firmware >=2.0.10 remains a production prerequisite. 2.0.10 is the
directly validated firmware. M18.2 does not replace the firmware prerequisite.**

## Manual acceptance runbook — retained for repeat checks

Owner acceptance above is complete. The following procedure is retained for
owner-operated repeat checks; its commands were not executed by the agent.

**XVF3800 USB firmware >= 2.0.10 remains a prerequisite; only 2.0.10 was directly
validated in M18.1. Firmware 2.0.6 is explicitly rejected on this appliance.**
M18.2 does not replace that prerequisite and implements no `xvf_host REBOOT`,
USB unbind/bind, firmware flash, hub/VBUS reset or external recovery command.

After owner-controlled deployment of this build, use the existing production
unit/store. Do not launch any parallel Firefly/helper process. These commands are
for the owner; the agent has not executed the service or hardware steps.

### Scenario 1: disconnect/reconnect while running

Start with service active and healthy WAITING_WAKE. In an SSH shell:

```sh
systemctl is-active firefly-voice-assistant
voice_pid_before=$(systemctl show firefly-voice-assistant -p MainPID --value)
voice_restarts_before=$(systemctl show firefly-voice-assistant -p NRestarts --value)
voice_since=$(date --iso-8601=seconds)
systemctl show firefly-voice-assistant -p MainPID -p NRestarts
curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
```

Confirm runtime_ready=true, microphone=connected, voice=WAITING_WAKE, wake=ready,
health overall=ready. Physically unplug reSpeaker, then run:

```sh
systemctl is-active firefly-voice-assistant
test "$(systemctl show firefly-voice-assistant -p MainPID --value)" = "$voice_pid_before"
test "$(systemctl show firefly-voice-assistant -p NRestarts --value)" = "$voice_restarts_before"
curl -fsS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/
curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
journalctl -u firefly-voice-assistant --since "$voice_since" --no-pager \
  | grep -E 'VOICE_RECOVERY|VOICE_STATE|HEALTH'
```

Require the same PID, unchanged NRestarts, active service, Admin HTTP 200,
runtime_ready=true, unavailable voice/microphone, failed wake and degraded overall
between retries. Loading/opening/starting during an attempt is expected. Observe
bounded schedules (1/2/4/8/10 seconds); no rapid repeated attempt storm.

Reconnect the same reSpeaker. **Do not restart the service or invoke any recovery
helper.** Poll status for up to 30 seconds as an operational acceptance window:

```sh
for voice_check in $(seq 1 30); do
    curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
    sleep 1
done
test "$(systemctl show firefly-voice-assistant -p MainPID --value)" = "$voice_pid_before"
test "$(systemctl show firefly-voice-assistant -p NRestarts --value)" = "$voice_restarts_before"
journalctl -u firefly-voice-assistant --since "$voice_since" --no-pager \
  | grep -E 'VOICE_RECOVERY|VOICE_STATE|HEALTH'
```

Require connected microphone, ready wake/overall, WAITING_WAKE and a recovered
event. Record reconnect-to-ready latency; failure to recover within the window
is an acceptance failure to investigate, not permission to restart to claim PASS.
Say **«Амина»**, then **«Какая погода?»**. Require WAKE_DETECTED, beep,
LISTENING_COMMAND, SPEECH_START/SPEECH_END, transcript, safe Unsupported on the
empty domain with no hardware execution, and return to WAITING_WAKE:

```sh
journalctl -u firefly-voice-assistant --since "$voice_since" --no-pager \
  | grep -E 'VOICE_STATE|VOICE text=|VOICE_ACK|VOICE_RECOVERY'
curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
```

### Scenario 2: microphone missing at service start

Owner stops the service, physically unplugs reSpeaker, then starts it:

```sh
sudo systemctl stop firefly-voice-assistant
# Physically unplug reSpeaker after the stop completes.
sudo systemctl start firefly-voice-assistant
voice_pid_before=$(systemctl show firefly-voice-assistant -p MainPID --value)
voice_restarts_before=$(systemctl show firefly-voice-assistant -p NRestarts --value)
voice_since=$(date --iso-8601=seconds)
systemctl is-active firefly-voice-assistant
curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
journalctl -u firefly-voice-assistant -n 100 --no-pager
```

Require available Admin/runtime and degraded voice with continuing bounded retries.
Plug reSpeaker in **without restarting**. Repeat Scenario 1's status polling,
PID/NRestarts comparisons and real voice cycle. Same process must reach WAITING_WAKE.

Optional graceful stop while absent/retrying (after collecting recovery evidence):

```sh
# Physically unplug reSpeaker and observe a scheduled retry first.
sudo systemctl stop firefly-voice-assistant
systemctl show firefly-voice-assistant -p ActiveState -p MainPID -p Result
journalctl -u firefly-voice-assistant -n 100 --no-pager
pgrep -af 'firefly-demo|amina_worker.py'
ss -ltnp '( sport = :8080 )'
sudo fuser -v /dev/snd/*
```

Require inactive, MainPID=0, Result=success, SHUTDOWN complete, RKLLM destroyed,
Deactivated successfully, no owned demo/Amina process, released port/audio, and
no timeout/SIGKILL. Empty pgrep/fuser results may return exit code 1. Do not kill
unrelated device owners. Leave service restoration to the owner's operational plan.

## Known limits

Recovery cannot repair failing firmware/hardware, missing dependencies or model
files; those failures continue at capped backoff with Admin available. It does not
reinitialize the separate Admin Copilot transcriber, reload RKLLM, or clear unrelated
health failures. The existing non-wake loop has no two-second no-PCM watchdog;
production uses wake gating, whose stall detection is unchanged. No promise of
deadline-bounded recovery from a vendor hang or forced cancellation of an already
admitted command is added. Real USB disconnect/reconnect, missing-microphone
startup recovery, post-recovery voice and shutdown during maximum backoff passed
owner acceptance as recorded above. Live memory usage with retained CPU models
during Admin setup has not been remeasured in this task.
