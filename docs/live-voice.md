# Live voice mode (Milestone 4 checkpoint)

Linux/aarch64 only. No wake word, network service, or hardware action execution.
The existing text mode and command semantics are unchanged. Speak only after
`LISTENING`; wait for the next `LISTENING` before the next command.

## Build

The installed ALSA development package is 1.2.6.1. Use the existing sherpa-onnx
1.13.7 CPU bundle and its exact versioned C header (downloaded in Milestone 4.2).
No model/runtime files belong in Git.

```sh
cmake -S . -B build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT="$PWD/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu" \
  -DSHERPA_ONNX_INCLUDE_DIR="$PWD/build/stt/vad-ab"
cmake --build build -j2
ctest --test-dir build --output-on-failure
build/speech_replay_tests build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
```

Voice is an optional build feature; pure C++ unit tests need neither audio devices
nor models. `speech_replay_tests` is a separate, explicit CPU/model-backed test.

## Start

From the repository root:

```sh
./build/firefly-assistant --voice \
  --audio-device 'plughw:CARD=H340,DEV=0' \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --stt-model-dir "$PWD/build/stt/model" \
  --vad-model "$PWD/build/stt/vad-ab/silero_vad.onnx"
```

H340 is only the development default; `--audio-device` accepts another ALSA PCM
name. `--pre-roll-ms` and `--post-roll-ms` default to 300 each (allowed 0..1000).
These are DEV-selected live defaults, not frozen production constants.
`--max-utterance-ms` defaults to 15000 (allowed 1000..15000).
`--voice-run-seconds N` optionally bounds a capture session for diagnostics.
Existing RKLLM timeout and verbose options remain available.

## Layers and lifecycle

`AudioSource` separates PCM capture from speech processing. `AlsaAudioSource`
uses one nonblocking `snd_pcm_open` capture stream, `snd_pcm_set_params` for
16 kHz mono S16_LE, `snd_pcm_readi` and bounded 50 ms `snd_pcm_wait` calls. ALSA
plugins may convert the hardware format. No arecord subprocesses are used.

`SpeechDetector`/`SherpaVad` feed 512-sample blocks to the exact versioned C API.
VAD retains the evaluated CPU/one-thread settings: threshold 0.5, minimum speech
0.25 s, minimum silence 0.5 s, window 512, maximum VAD segment 20 s.

`VoiceSegmenter` owns bounded original PCM history and VAD sample coordinates:

1. IDLE: feed VAD and retain rolling audio; silence emits nothing.
2. SPEECH: retain the utterance and monitor its maximum duration.
3. END/POST-ROLL: use the VAD start/end, include configured padding, and wait
   until all post-roll samples exist. The VAD's 500 ms endpoint delay usually
   already supplies the 300 ms post-roll. Clamp pre-roll at stream start.
4. PROCESS: send one bounded utterance to a single asynchronous worker. Capture
   and VAD continue; speech overlapping PROCESS is discarded, never queued.
5. WAITING_FOR_QUIET: discard any continuing speech until one second of VAD quiet,
   reset framing, then print LISTENING. This avoids executing speech tails.

`SpeechRecognizer`/`SherpaRecognizer` retains the full Russian Zipformer model,
CPU/two-thread/greedy-search settings. It uses the same batch-of-one decode API
path as the existing sherpa-onnx-offline executable. Each request owns a fresh
stream/result. `VoiceProcessor` passes the raw recognized string unchanged to
CommandEngine. All diagnostics go to stderr; stdout contains compact actions.

VAD, STT, and RKLLM initialize once before opening capture. RKLLM retains its
existing bootstrap and per-request KV policy. Repeated utterances reuse models.

## Limits and failure behavior

PCM history is bounded to maximum speech plus four seconds (304,000 floats at
defaults); STT audio is bounded to 17 seconds including maximum allowed padding.
There is at most one worker and no utterance backlog. Oversized speech is rejected
with U and discarded until quiet; vendor duration splits cannot make a tail into
a new accepted command. Completed VAD boundaries are also checked against the
speech limit, so no valid command is silently truncated.

ALSA overruns/suspends get one prepare attempt and invalidate current framing.
Recovery requires quiet. Unrecoverable open/read or VAD failures stop voice mode
with an explicit error. STT failures/empty audio produce U without calling the
command engine; subsequent requests use fresh per-request state. Empty recognized
text is passed unchanged and rejected by the existing input policy.

SIGINT/SIGTERM only set a stop flag. The main loop stops emitting results and
joins any inference before destroying models. STT has no integrated cancellation;
shutdown waits for an in-flight vendor decode. RKLLM retains its existing timeout
watchdog. Vendor process aborts/hangs cannot be recovered with C++ exceptions.
No unsafe detached threads are used. An uninterrupted VAD coordinate epoch stops
at 24 hours, before the vendor's signed 32-bit sample counter can overflow.

## Manual human-speech check

After LISTENING, speak these separately, waiting for LISTENING between them:

- «На кухне поставь свет на сорок процентов» → L|K|40
- «Открой входную дверь» → D
- «Какая сегодня погода?» → U

Leave several seconds of silence between commands: silence must produce no
STT call, Qwen3 call, or repeated U. Use Ctrl-C to exit. The user reported this initial live smoke test passed. The subsequent
[fresh live baseline](milestone-4-baseline.md) records 8/9 correct transcripts
and actions. These small sets do not establish production accuracy or safety.

## Verified in this milestone

- All eight CTest suites passed, including the unchanged text/core suites.
- Native streaming replay: all eight H340 speech WAV transcripts match the
  selected 300 ms DEV results; the two synthetic silence WAVs make zero STT calls.
  One persistent STT instance handles all eight speech requests.
- Real H340 + STT + RKLLM startup reached LISTENING in 30.36 s. A brief capture
  check produced no transcript/action output; SIGINT exited with status 0 in
  1.82 s. Logs: `build/stt/live-startup.*` and `build/stt/live-replay.log`.
- Fresh live regression: 8/9 exact transcripts and correct actions; zero false
  accepts/wrong actions and one false reject. See the tracked
  [Milestone 4 baseline](milestone-4-baseline.md) for every utterance, the measured
  silence intervals and the unconfirmed extra segment.
