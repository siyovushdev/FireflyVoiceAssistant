# Milestone 4 live baseline

Checkpoint date: 2026-09-13. Live implementation: `c43f2fa` (Implement live VAD STT voice pipeline). This checkpoint documents results only; it makes no behavioral changes.

## Scope and architecture

Firefly ROC-RK3588S-PC, Ubuntu 22.04, Linux/aarch64, RK3588S with 8 GB RAM. Fully offline, one running process:

```text
Configurable ALSA microphone (H340 development default)
  -> continuous mono PCM capture
  -> Silero VAD + bounded original-audio history
  -> speech window with 300 ms pre-roll / 300 ms post-roll
  -> full Russian Zipformer STT
  -> raw transcript unchanged into CommandEngine
  -> DeterministicParser, otherwise RKLLM/Qwen3 fallback
  -> SafetyGate -> strict ActionValidator -> compact action
```

AudioSource, SpeechDetector, SpeechRecognizer, VoiceSegmenter and VoiceProcessor keep capture, boundaries, recognition and command processing separate. VAD, STT and RKLLM initialize once. Capture continues during processing, but overlapping commands are discarded rather than queued. Rearming requires one second of VAD quiet. The text CLI remains available. No wake word, Android, cloud speech, or hardware execution.

## Frozen stack and current parameters

| Component | Checkpoint configuration |
|---|---|
| Capture | Native ALSA, 16,000 Hz, mono S16_LE; configurable device, development default `plughw:CARD=H340,DEV=0` |
| STT | sherpa-onnx 1.13.7 Linux/aarch64 CPU; full Russian Zipformer transducer; existing encoder.int8.onnx, decoder.onnx, joiner.int8.onnx, tokens.txt; 2 threads; greedy_search |
| VAD | Existing Silero ONNX model; CPU, 1 thread; threshold 0.5; 512-sample window; minimum speech 0.25 s; minimum silence 0.5 s; VAD maximum segment 20 s |
| Padding | Current configurable live default: 300 ms before and after; selected on 8 DEV WAVs, not a permanently frozen production constant |
| Live limits | Maximum speech 15 s; bounded history (maximum speech + 4 s); maximum STT audio 17 s; one worker, no utterance backlog |
| LLM | RKLLM 1.3.0; Qwen3-4B W8A8 RK3588 3-core NPU model; frozen compact v5 prompt |
| Sampling | top_k=1, top_p=1.0, temperature=0.1, repeat_penalty=1.0, frequency_penalty=0, presence_penalty=0 |
| Context | max_new_tokens=16, max_context_len=1024, skip_special_token=true, enable_thinking=false, keep_history=0, user prompt input |
| KV lifecycle | Existing Qwen3 chat template; empty-user bootstrap once; retain system KV in RAM; clear user KV before each request; no serialized persistent prompt cache |
| Text safety | Existing parser, CommandEngine, SafetyGate and validator semantics unchanged; no transcript correction; 192-byte input limit; existing 30 s RKLLM deadline |
| Protocol | L\|K\|N, L\|B\|N, L\|G\|N, L\|C\|N (integer 0..100); D for explicit entrance-door opening; U for rejection |

Keep these models, decoding/VAD settings, prompt, parser, SafetyGate and protocol unchanged unless separately authorized. The entrance-door gate still requires opening intent, a door reference and an entrance reference. Preserve the false reject below; do not introduce a transcript substitution to hide it.

## Fresh live regression

Nine requested human-spoken phrases in one restarted process. Previous interrupted logs are excluded and preserved. Exact scoring uses lowercase, unpunctuated expected transcripts prepared before recording; raw STT output is not changed.

| Metric | Result |
|---|---:|
| Exact STT | 8/9 (88.9%) |
| Correct final actions | 8/9 (88.9%) |
| False accepts | 0 |
| False rejects | 1 |
| Wrong actions | 0 |
| FAST_PATH / LLM_FALLBACK | 3 / 6 |
| STT latency range | 52.96–156.62 ms |

Latency is recognition time, excluding model initialization and command inference. This small live set establishes a baseline, not a production accuracy guarantee.

| # | Spoken reference | Raw STT | Speech / padded s | STT ms | Route | Expected | Final | Correct |
|---|---|---|---|---:|---|---|---|---|
| 1 | в спальне поставь свет на семьдесят процентов | в спальне поставь свет на семьдесят процентов | 3.334 / 3.934 | 146.80 | LLM_FALLBACK | L\|B\|70 | L\|B\|70 | Yes |
| 2 | в гостиной включи свет | в гостиной включи свет | 1.222 / 1.822 | 72.25 | FAST_PATH | L\|G\|100 | L\|G\|100 | Yes |
| 3 | в коридоре выключи свет | в коридоре выключи свет | 1.414 / 2.014 | 78.78 | FAST_PATH | L\|C\|0 | L\|C\|0 | Yes |
| 4 | на кухне сделай свет наполовину | на кухне сделай свет наполовину | 3.782 / 4.382 | 156.62 | FAST_PATH | L\|K\|50 | L\|K\|50 | Yes |
| 5 | на кухне поставь свет на десять процентов | на кухне поставь свет на десять процентов | 2.118 / 2.718 | 101.16 | LLM_FALLBACK | L\|K\|10 | L\|K\|10 | Yes |
| 6 | открыть дверь на входе | открытвер на входе | 1.158 / 1.758 | 69.84 | LLM_FALLBACK | D | U | No |
| 7 | открой дверь | открой дверь | 0.614 / 1.214 | 52.96 | LLM_FALLBACK | U | U | Yes |
| 8 | свет на максимум | свет на максимум | 0.966 / 1.566 | 63.76 | LLM_FALLBACK | U | U | Yes |
| 9 | расскажи мне новости | расскажи мне новости | 1.222 / 1.822 | 69.02 | LLM_FALLBACK | U | U | Yes |

The sole scored failure was **«открыть дверь на входе» → «открытвер на входе» → U**, expected D. This is an STT error and a safe false reject. It remains uncorrected; no STT, VAD, padding, parser, SafetyGate, Qwen3 or prompt tuning was performed.

## Silence and extra detection

Measured silence intervals of **23.10 s** and **24.36 s** caused zero STT/CommandEngine invocations and no action outputs. These claims apply to the measured intervals.

One additional, unconfirmed segment before phrase 7 yielded raw `м`, speech duration 1.094 s, padded duration 1.694 s, STT latency 68.80 ms, LLM_FALLBACK, final U. Its source remains unconfirmed; it is not classified as known silence or intentional speech. It safely produced no executable action and is reported separately from the nine scored phrases. Total detections were ten.

The process shut down with SIGINT and exit code 0. No implementation changes were made based on failures.

## Prior validation and checkpoint artifacts

- Text DEV baseline: 48/48, zero false accepts/rejects or wrong/invalid actions; 17 FAST PATH, 31 LLM FALLBACK.
- Original 8-WAV DEV comparison: full audio 6/8 exact STT and 7/8 actions; unpadded VAD 5/8 and 6/8; selected 300 ms padding 8/8 and 8/8. Selection on DEV does not make padding a frozen production constant.
- Milestone 4.4: all eight unit-test suites passed; native streaming replay matched all eight expected transcripts with one persistent STT model; two synthetic silences bypassed STT.
- Real startup: LISTENING in 30.36 s; SIGINT shutdown in 1.82 s. User-reported initial live smoke: exact kitchen-40, entrance-door and weather transcripts, correct L\|K\|40 / D / U, and silence with no processing.

Build/run instructions and lifecycle limits: [live-voice.md](live-voice.md). Raw fresh-regression logs remain locally under `build/stt/live-regression-45/restart-20260913T062934Z/`. This tracked document preserves the baseline independently of ignored build outputs. Models, runtimes, recordings and generated build artifacts are deliberately not committed. All files under `~/demo_Linux_aarch64` remain read-only.

Milestone 4 is checkpointed. Stop here: no wake word or hardware execution.
