# Seamless Amina invocation — measured acceptance

Base: `0c76e0ead1c29c0ff1edd845a48f133bcec6f3c0`, clean master at start.
Target: Firefly ROC-RK3588S-PC / RK3588S, existing native build and models.
No physical microphone/speaker live acceptance was performed for this milestone.
No model, threshold/window, channel, Qwen protocol/context or execution policy changed.

## Implementation

One native capture feeds KWS + a 48,000-sample mono float ring (3 s) while waiting.
After KWS, 4,096 real samples provide a 256 ms continuation grace. A joined worker
exclusively probes the existing Silero VAD while the same capture thread keeps
collecting real PCM. Post-detection storage, including grace, is capped at 16,000
samples (1 s); overflow makes wake unavailable, never ungated.

Two VAD passes compare real continuation with the same pre-wake history followed
by silence. Each uses 1,280 ms of **virtual endpoint-only** zeros. The real speech
end must extend >=128 ms past detection and the control endpoint. This accounts
for Silero's confirmation delay/recurrent tail without changing its model/config.
A reset followed by real-history refeed produces the single final utterance;
no probe zeros or playback PCM enter STT. Earlier completed utterances are ignored.
No second ALSA owner or STT pipeline exists. Probe and command workers are joined,
and never concurrently own VAD.

```text
WAITING_WAKE -> WAKE_DETECTED -> CHECKING_CONTINUATION
  CONTINUATION_INLINE -> SPEECH_START -> CAPTURING_INLINE_COMMAND (no beep)
  CONTINUATION_SEPARATED -> ACKNOWLEDGING_WAKE -> BEEP_START/BEEP_END
    -> 250 ms drain -> LISTENING_COMMAND -> SPEECH_START -> CAPTURING_COMMAND
SPEECH_END -> PROCESSING -> WAITING_WAKE
```

Only a KWS-admitted inline utterance permits one leading `амина` / confirmed
`афина` token to be removed (exact lower/title/uppercase, ASCII separators).
Raw STT is preserved. Empty, fused or unknown prefixes reject. Internal words,
separated commands and text/Admin are unchanged. Resulting command text goes
through the existing CommandEngine, GenericAction, SafetyGate, binding/executor.

## Measured overhead

[Raw component measurements](performance.json), 30 VAD probes on full-sized silent
history, one existing Silero instance; process CPU time and Linux smaps RSS:

| Metric | Measured |
| --- | ---: |
| Ring PCM payload | 192,000 bytes |
| Ring added RSS (standalone allocation) | 332 KiB |
| Ring + transient snapshot added RSS | 600 KiB |
| Ring copy CPU / 600 s audio | 131.246 ms |
| Ring CPU as one core | 0.02187% |
| Two-probe compute wall p50 / p95 / max | 56.55 / 56.73 / 57.43 ms |
| Probe + final refeed CPU p50 / p95 / max | 77.15 / 77.41 / 78.29 ms |

These are component costs, not total application RSS or hard latency guarantees.
The resident STT/Qwen/KWS costs were not attributed to the ring. The asynchronous
probe keeps capture running during computation; final refeed runs after joining.

[Paced recorded replay](recorded-replay.json), 32 ms input cadence, 2,976 ms ring
history at detection. Real unchanged VAD and Zipformer; exact PCM comparison
verifies recovery of the first 256 ms of speech preceding the synthetic KWS event.

| Recording | Unchanged real STT result | Wake→decision, ms | Maximum capture read gap, ms | Added RSS over warm model baseline, KiB |
| --- | --- | ---: | ---: | ---: |
| 06_kitchen_40.wav | на кухне поставь свет на сорок процентов | 351.44 | 68.29 | 1788 |
| 04_light_on.wav | включи свет | 349.00 | 77.38 | 32 |
| 05_light_off.wav | выключи свет | 350.43 | 66.43 | 104 |

The RSS column covers the voice-loop capture/replay allocation growth over loaded,
warmed VAD/STT in this process, including existing segment history and thread/
allocator retention. Later cases reuse allocations; it is not a clean whole-product
before/after comparison. The first-case peak is a conservative observed warm-loop
increment. Read gaps are paced fake-source measurements, not an ALSA overrun claim.

All three also check that the same recording **with silence after detection**
chooses separated admission, despite VAD hangover. No Amina-prefixed WAV is available
in the accessible DEV corpus: these recordings deliberately have no accepted wake
prefix and are safely rejected after real STT. They prove audio-boundary preservation
independently of any generated STT fixture; they do not prove Amina acoustic accuracy.

## Pipeline acceptance

[Integration evidence](integration-replay.json): one real RKLLM owner and existing
loopback HTTP mock. Four original separated cases use real DEV STT, start near the
5 s deadline, and preserve normal capture completion. Four additional inline cases
use recorded PCM / real VAD and explicitly **scripted wake-prefixed STT fixtures**
for admission → prefix stripping → real compact Qwen/Fast Path → SafetyGate → HTTP.
The independent real-STT replay above prevents fake text from being the sole
boundary proof. No transcript fabrication exists in production code.

| Case | Path | Expected | Route | STT source |
| --- | --- | --- | --- | --- |
| 0 | separated | 40 | LLM_FALLBACK | real Zipformer |
| 1 | separated | 100 | FAST_PATH | real Zipformer |
| 2 | separated | 0 | FAST_PATH | real Zipformer |
| 3 | separated | Unsupported | LLM_FALLBACK | real Zipformer |
| 4 | inline | 40 | LLM_FALLBACK | scripted prefix fixture |
| 5 | inline | 100 | FAST_PATH | scripted prefix fixture |
| 6 | inline | 0 | FAST_PATH | scripted prefix fixture |
| 7 | inline | Unsupported | LLM_FALLBACK | scripted prefix fixture |

Exactly six HTTP POSTs: `40, 100, 0, 40, 100, 0`; unsupported cases execute nothing.
Inline plays zero beeps; separated plays one acknowledgement. All rearm once.
The actual Qwen fallback uses the unchanged compact decoder and stable context.

## Reproduction

Prerequisite: the existing local sherpa/STT/DEV assets (hashes in [inputs.json](inputs.json))
and configured native `build`. No downloads or R&D writes are needed.

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -R 'SeamlessWake|WakeVoiceGating|WakeSessionUx|VoicePipeline|CompactActionDecoder|V1PrototypePipeline|WakeLiveHelper|AminaWorkerLifecycle|AminaStreamingBackend|RkllmContextRefresh'
ctest --test-dir build --output-on-failure
./build/seamless_wake_benchmark build/stt/vad-ab/silero_vad.onnx build/seamless-performance.json
./build/seamless_speech_replay build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt/eval build/seamless-recorded.json
# Only when no other real Qwen runtime is active:
python3 tests/execution/WakeCompactReplay.py build/seamless-integration.json --inline-synthetic
git diff --check
sha256sum assets/wake/amina-v3.tflite
```

Focused tests: **10/10**, 4.04 s (`build/seamless-focused.log`).
Full CTest: **55/55**, 46.90 s (`build/seamless-ctest.log`). Native build passed
without compiler warnings; `git diff --check` and live-helper shell syntax passed.
`SeamlessWake` covers ring capacity/wrap/order, command onset before KWS, immediate
continuation, old completed history, on/off/40 compact fallback, safety rejection,
exact prefix scope and malformed/empty prefixes, no-wake speech, pause/beep/timeout,
beep exclusion, shutdown during grace/inline/probe, continued single-thread capture
while probing, and fail-closed bounded probe overflow. Existing `WakeSessionUx`
covers late speech at 4.8 s, VAD confirmation delay, independent max duration,
beep failure/drain and shutdown while acknowledging/waiting/capturing.
`WakeVoiceGating` still tests detector/audio/reset/VAD failures and worker joining.
Model-backed Amina and compact/context/Fast Path/HTTP regressions remain in CTest.

Frozen tracked Amina SHA256 remains
`bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1`.
Native capture/playback implementations, STT/VAD models/settings and Qwen/safety/
binding/execution code are unchanged from the baseline.

## Live helper and boundaries

`./scripts/v1-wake-live.sh` is prepared with loopback HTTP mock and A–E checklist:
separated on; continuous on; continuous spoken forty; separated command after a
~4 s wait; ordinary command without wake ignored. Path/prefix/latency logs are
explicit. Human live acceptance was intentionally not run.

A pause during the decision window can select beep admission; wait for its arm.
A missing/unknown/clipped wake prefix, preceding words in the same utterance or
history older than three seconds fails safely. Only `афина` is admitted as the
historically confirmed confusion; there is no fuzzy matching. Noise/reverberation,
STT pronunciation variation and synchronous vendor hangs remain existing limits.
The 5000 ms start wait (+288 ms confirmation allowance), 15000 ms speech guard,
17000 ms capture watchdog and deprecated explicit total-timeout behavior remain.
This milestone does not claim new physical combined-invocation or speaker audibility
acceptance, zero hardware overruns, or unchanged accuracy outside the measured corpus.
