# V1 Amina wake gating

**Amina Wake V1 live acceptance passed on 2026-09-28**, confirmed by the operator
and the production voice / HTTP mock logs below. The subsequent
[Qwen latency / command-session UX milestone](v1-qwen-latency-command-ux.md)
adds native beep acknowledgement and split speech-start/capture timing. Its
acceptance is automated replay; the historical human pass below predates beep.

Production entry point: `firefly-demo --voice`. Wake gating is enabled by default.
The legacy `firefly-assistant --voice` diagnostic CLI remains an ungated, non-hardware
M4 tool; use `firefly-demo` for product acceptance. Admin text and Admin Copilot
requests are explicit user operations and are not microphone wake sessions.

## Architecture and lifecycle

```text
one native ALSA capture (XVF3800, stereo S16_LE, 16000 Hz, channel index 1)
    WAITING_WAKE -> PCM-only Amina worker
    WAKE_DETECTED -> ACKNOWLEDGING_WAKE -> native ALSA beep (PCM drain/discard)
    250 ms input drain -> LISTENING_COMMAND (fresh VAD/history; wait for speech)
    CAPTURING_COMMAND -> existing SherpaVad / VoiceSegmenter until endpoint
    PROCESSING -> existing SherpaRecognizer -> RuntimeWorker::processVoice
        -> CommandEngine -> SafetyGate / ActionValidator -> GenericAction
        -> BindingResolver -> HttpHardwareExecutor -> Predicted state
    WAITING_WAKE (fresh frontend + streaming model state + score window)
```

C++ owns ALSA for the entire lifetime and explicitly starts the nonblocking PCM
stream (configuration alone can leave USB capture in PREPARED). KWS has no audio device, VAD, STT, LLM,
command or execution APIs. One bounded PCM request/response is in flight over a
local socket. No second `arecord`/ALSA consumer, no audio queue, no speech backlog.
In WAITING_WAKE only KWS processes audio; the initialized VAD/STT/LLM remain idle.
During PROCESSING capture drains/discards PCM; KWS and VAD are idle. All PCM before
the wake detection block, including that block, is excluded from the command.

One wake admits the first complete utterance only, even if VAD returns multiple
boundaries. Success, empty/unsupported STT, STT error and oversized speech return
to WAITING_WAKE. The default 5000 ms deadline waits for speech **start** only;
Silero's unchanged 250 ms confirmation delay gets a bounded 288 ms allowance.
After speech is confirmed, this start deadline is disabled. Existing VAD endpoint
and post-roll end capture; a separate 15000 ms speech guard and 17000 ms capture
bound prevent endless speech/noise. No speech yields COMMAND_START_TIMEOUT;
oversized speech yields COMMAND_MAX_DURATION. The explicit deprecated
`--command-listen-timeout-ms` retains its old total listening deadline (including
endpoint/post-roll), and is disabled by default. Inference keeps its existing
STT/RKLLM deadlines. Ordinary subsequent speech needs another wake.

The reference smoothing rule is **mean of the last seven scores >= 0.99**.
“Window/consecutive 7” is not reinterpreted as seven individual threshold crossings.
The shadow observer's repeat-event cooldown is replaced by single-session admission;
KWS does not run during a command and all streaming state is recreated on rearm.
Feature extraction uses the same MicroFrontend, 160-sample hops, three feature
frames per invoke, float32-to-int8 input conversion. Output uses the model's
`scale * (uint8_value - zero_point)`, never `/255`. All 14 state tensors stay owned
by the interpreter between invokes; resource handles are never read as values.
Frontend pending samples, feature frames and smoothing history are bounded.

Acknowledgement is a native nonblocking 100 ms, 880 Hz beep, with 10 ms fades
and moderate PCM amplitude. Default output is `plughw:CARD=rockchipes8388,DEV=0`.
One capture owner drains/discards PCM during playback and for 250 ms afterwards;
only then is fresh command VAD armed. Say “Амина”, hear the beep, then speak.
There is no per-wake shell/process or playback worker. Playback failure is logged
and exposed separately in health; a healthy microphone/VAD/STT may continue the
already wake-admitted session.

**Known limitation V1 — command timing:** command STT can sometimes capture a
remaining part of the wake phrase or lose the beginning of the command when the
pause/timing is imperfect. The accepted live run includes `афина включи свет на
кухне` and a truncated `свет на кухне`. Wait for LISTENING_COMMAND before speaking
the command. Seamless continuous “Амина включи…” is not guaranteed; no transcript
correction or wake/STT model tuning is introduced. Beep separates the two phases,
but does not guarantee continuous wake-plus-command speech.

## Artifact and runtime packaging

`assets/wake/amina-v3.tflite` is the explicitly authorized small versioned product
artifact; `assets/wake/manifest.json` records its frozen identity. SHA256:

`bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1`

The worker checks the SHA256 of the bytes it passes into the interpreter at startup
and reset, plus IO shapes/dtypes, quantization metadata and state counts. Any
mismatch prevents voice admission. The reference shadow-runner remains external,
unchanged, and is not a runtime dependency.

CMake stages data in `build/share/firefly/wake/`. `cmake --install` installs these
files under `${CMAKE_INSTALL_DATADIR}/firefly/wake/` (data only; the existing product
has no complete installer). A release places the binary in `<prefix>/bin` and the
wake data in `<prefix>/share/firefly/wake`; lookup is executable-relative, independent
of cwd. No production path references the R&D directory. Custom `--wake-model`
does not change the frozen hash or worker location.

The KWS Python environment is a separately packaged dependency, pinned in
`runtime/wake/requirements.txt`: LiteRT 1.2.0, pymicro-features 2.0.2, numpy 2.2.6.
The acceptance board uses an independent `build/wake-venv` made by copying already
installed packages into a new venv, with no downloads or reference modifications.
A future installer must stage these packages offline and pass its interpreter via
`--wake-python`; it must not depend on an R&D venv. The product C++ core and its
fake tests need none of these packages.

## Configuration

| Option | V1 default / constraint |
| --- | --- |
| `--wake-enabled` | `true`; `false` is an explicit diagnostic opt-out |
| `--wake-model` | executable-relative `share/firefly/wake/amina-v3.tflite` |
| `--wake-python` | `python3`; pass packaged venv interpreter when needed |
| `--wake-cutoff` | `0.99`, other values rejected in V1 |
| `--wake-window` | `7`, other values rejected in V1 |
| `--command-start-timeout-ms` | `5000`, allowed 1000..15000; speech-start only |
| `--command-listen-timeout-ms` | deprecated explicit total listening cap, 1000..60000; absent by default |
| `--playback-device` | `plughw:CARD=rockchipes8388,DEV=0` |
| `--audio-device` | `plughw:CARD=Array,DEV=0` |

The production voice input is now stereo with channel index 1 for both KWS and
existing STT. The original mono constructor remains the default for legacy tools.
No Web Admin sensitivity UI or catalog-schema change is introduced.

## Health and failures

`GET /api/status` exposes `health.subsystems.wake` independently and the runtime
`voice` state. Ready is reported only after model/IPC/audio/VAD/STT startup and reset.
Wake/model/worker/capture/VAD failure sets wake failed, overall degraded, and voice
unavailable. There is no automatic always-listening fallback or restart loop.
Two seconds without incoming PCM, or an audio overrun/discontinuity, closes the gated voice path; restart is required.
Explicit `--wake-enabled false` is logged and is the only ungated product mode.

Worker startup has a 10 s deadline, reset 1 s, inference IPC 500 ms. Timeout,
unexpected reply, worker exit or write failure terminates the voice loop. Worker
teardown closes IPC for graceful EOF, allows a bounded 100 ms exit period, then
kills if needed and reaps its owned child (even on native hangs). Unrelated parent
descriptors are closed before exec, including any RKLLM/HTTP handles.
SIGINT/SIGTERM stops admission, the existing RuntimeWorker rejects new execution,
and the voice loop joins in-flight processing before destroying STT/capture/KWS.
An already running STT/vendor RKLLM call still has the baseline vendor-hang limits;
there are no detached processing threads. No beep subprocess is launched.

## Build and deterministic checks

```bash
cmake -S . -B build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT="$PWD/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu" \
  -DSHERPA_ONNX_INCLUDE_DIR="$PWD/build/stt/vad-ab" \
  -DFIREFLY_WAKE_PYTHON="$PWD/build/wake-venv/bin/python"
cmake --build build -j4
ctest --test-dir build -R 'WakeVoiceGating|WakeLiveHelper|Amina|V1PrototypePipeline|HealthReliability' --output-on-failure
ctest --test-dir build --output-on-failure
```

`WakeVoiceGating` uses fake PCM, wake events, VAD and time. `AminaWorkerLifecycle`
checks child exit, malformed replies, hanging workers, deadlines and reaping.
`AminaStreamingBackend` is opt-in CPU/model-backed, with no mic/NPU: integrity,
streaming silence, bounded histories, reset, truncated IPC and quantization/window
regressions. `V1PrototypePipeline` additionally routes gated fake STT through the
existing production HTTP mock, including unknown and empty commands.

## Live acceptance on the Firefly

After tests, with all other RKLLM and microphone consumers stopped:

```bash
./scripts/v1-wake-live.sh
# Another terminal: follow the printed voice.log path.
# Status: curl -s http://127.0.0.1:8080/api/status
# Device state: curl -s http://127.0.0.1:8080/api/devices/state
```

The helper creates a fresh private store from `config/demo/v1-kitchen.json`, starts
only the loopback V1 HTTP mock and one product runtime, refuses known competing
consumers, and cleans up both owned processes on exit. It never reuses a store that
could contain a real device binding. Logs remain in the printed `build/wake-live.*`
directory. Stop with Ctrl-C. No service is installed/enabled.

For every case, wait for WAITING_WAKE first. Check both voice and HTTP mock logs.

| Input | Expected |
| --- | --- |
| «Включи свет на кухне» without wake; ordinary conversation | no STT/command/HTTP request |
| «Амина», then «Включи свет на кухне» | one Accepted, Predicted brightness 100; WAITING_WAKE |
| ordinary speech / «Выключи свет на кухне» without wake | no additional HTTP request |
| «Амина», then «Выключи свет на кухне» | one Accepted, Predicted brightness 0; WAITING_WAKE |
| ordinary speech without wake | no additional HTTP request |
| «Амина», then «Поставь свет на кухне на сорок процентов» | one Accepted, Predicted brightness 40; WAITING_WAKE |
| «Амина», then silence for ~5 s | COMMAND_START_TIMEOUT, WAITING_WAKE; no HTTP request |
| «Амина», then «Какая погода?» | Unsupported, WAITING_WAKE; no HTTP request |
| Ctrl-C | SHUTDOWN complete; no remaining worker/runtime/capture |

Record actual STT, route, action, Accepted/Predicted and request counts. Word-form
“сорок” normally uses the existing RKLLM fallback. Do not claim physical device
confirmation from HTTP mock acceptance, or live phrase acceptance from startup-only
or fake tests. Wake/STT models, parser and SafetyGate remain unchanged; the follow-up milestone
changes only the Qwen output contract and context lifecycle.

## Accepted human live results — 2026-09-28

The operator confirmed successful Amina Wake V1 live acceptance on the Firefly
ROC-RK3588S-PC with reSpeaker XVF3800. Evidence: `build/wake-live.C2Mt9U/voice.log`
and `build/wake-live.C2Mt9U/http-mock.log`. The frozen Amina model, cutoff 0.99,
window 7, ASR channel index 1, VAD/STT and existing command pipeline were retained.
Execution used only the V1 loopback HTTP mock; no physical hardware was controlled.

| Scenario | Actual STT text | Route | Observed result | STT latency |
| --- | --- | --- | --- | --- |
| Wake + kitchen light on | `включи свет на кухне` | FAST_PATH | brightness 100; HTTP 200 | 328.794 ms |
| Wake + kitchen light off | `выключи свет на кухню` | LLM_FALLBACK | brightness 0; HTTP 200 | 331.969 ms |
| Wake + kitchen light at forty percent | `включи свет на кухне на сорок процентов` | LLM_FALLBACK | brightness 40; HTTP 200 | 416.469 ms |
| Wake remainder included in command | `афина включи свет на кухне` | LLM_FALLBACK | brightness 100; HTTP 200 | 360.393 ms |
| Incomplete command | `свет на кухне` | LLM_FALLBACK | Unsupported; no execution | 284.624 ms |
| Unsupported speech | `вот` | INPUT_REJECTED | Unsupported, target_not_grounded; no execution | 266.931 ms |
| Unsupported speech | `искусный интел что ты программируешь` | INPUT_REJECTED | Unsupported, target_not_grounded; no execution | 808.443 ms |

All seven sessions have the sequence WAKE_DETECTED → LISTENING_COMMAND →
PROCESSING → WAITING_WAKE. The mock received exactly four brightness POSTs, in
order **100, 100, 0, 40**, all with HTTP 200; the three rejected sessions produced
no hardware request. HTTP success represents Accepted / Predicted in the existing
V1 pipeline, not physical-device confirmation. Latencies above measure STT only,
not total command or LLM latency.

The live pass does not remove the timing limitation documented above or establish
a statistical false-activation rate. No architecture changes or new features were
added during finalization. Production state, route, rejection, health and timing
logs are retained; no per-frame debug dump or audio recording is shipped.

## Earlier integration and reliability verification — 2026-09-28

- Focused voice/wake/M14/M15/M17/V1 suites: 8/8 passed.
- Final full CTest after the ALSA fix: **51/51 passed**, 47.23 s.
  Log: `build/wake-ctest.log`; build: `build/wake-build.log` (no compiler warnings).
- Separate core-only build (`FIREFLY_BUILD_ASSISTANT=OFF`, ADMIN/VOICE OFF):
  **2/2** wake admission/worker lifecycle suites passed, 1.40 s; no NPU, ALSA,
  microphone, model packages or downloads required for those tests.
- Real product startup with frozen model and XVF3800 reached WAITING_WAKE,
  microphone connected, wake ready and overall ready. `/proc/asound` confirmed
  **RUNNING**, S16_LE, two channels, 16000 Hz; PCM counters advanced. A 30 s
  observation remained WAITING_WAKE with `last_voice=null` and no HTTP requests.
  This is an idle streaming check, not a measured ordinary-speech rejection test.
- Stopping the owned KWS child with SIGTERM produced voice unavailable,
  wake failed, overall degraded within **0.284 s** in this run (polling included).
  Admin/STT stayed ready. The KWS child was reaped and ALSA closed; no ungated
  fallback or command occurred. Graceful helper/runtime shutdown logged
  `SHUTDOWN complete`; runtime, KWS and mock processes were gone, microphone closed.
- Final live artifacts: `build/wake-live.oIb5SP/` (`voice.log`, `http-mock.log`,
  `status-ready.json`, `status-observation.json`, `status-kws-failed.json`,
  `failure-observation.json`, ALSA snapshots). No audio was saved.

The initial live attempt (`build/wake-live.JjvYuT/`) exposed ALSA staying PREPARED
without PCM; its idle/health indication was not valid evidence of wake streaming.
This was fixed by explicit `snd_pcm_start` and a two-second no-PCM watchdog, with
a deterministic stall regression test. The final observation above used the fix.
No prompt, parser, SafetyGate, STT/VAD settings or frozen wake parameters were tuned.

### First operator wake observations and diagnostic follow-up

`build/wake-live.1cLHsp/voice.log` contains three WAKE_DETECTED transitions:
first STT text `х` was rejected (`target_not_grounded`), then two sessions timed
out; all three returned to WAITING_WAKE. Those initial observations did not
establish the spoken command/timing or prove successful command capture. They are
retained as earlier diagnostic evidence; the later accepted run is documented above. No wake/STT/parser parameters were changed in response.
Voice result logs retain `audio_s`, `speech_s` and `stt_ms` as useful production
diagnostics for the known timing limitation.

The same run's `http-mock.log` exposed a separate acceptance-helper race: port 8765
was already occupied, and connecting to that unrelated listener could pass the
old readiness check before our mock failed to bind. The helper now requires a
PID/host/port readiness file written by its own mock **after** successful bind.
An already running mock must be stopped by its owner before using this helper;
the helper does not take over or stop unrelated listeners.

Follow-up verification: **52/52 CTest passed** (`build/wake-followup-ctest.log`),
including the new mock readiness/occupied-port tests. The real helper also refused
the occupied 8765 before starting runtime (`build/wake-helper-conflict-test.log`),
leaving the unrelated mock untouched. Existing model-backed VAD/STT replay passed
all eight DEV speech transcripts and two silence cases
(`build/wake-followup-speech-replay.log`). These replay results do not substitute
for human wake-plus-command acceptance.

## Milestone finalization checks

After recording the accepted live results, the existing build completed and the
final focused run passed **9/9 suites** (6.28 s); the full CTest run passed
**52/52 suites** (46.07 s). Logs remain local under `build/wake-final-build.log`,
`build/wake-final-focused.log` and `build/wake-final-ctest.log`.

`git diff --check` and the staged diff check passed. The staged product model is a
regular Git blob of **60,912 bytes**, whose SHA256 exactly matches the frozen
manifest above. Only this explicitly authorized small model is versioned; build
outputs, Python environment, temporary diagnostics and live logs remain ignored.
Useful production logs are retained, including state transitions, health failures,
command results and audio/STT durations needed to diagnose the V1 timing limitation.
