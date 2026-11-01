# Operational voice dialogue + F2 — developer verification

Source integration based on `d75dafbe58216c2796b4b8ac2813d063b37e87e3`.
The final feature commit is the Draft PR head. This is developer acceptance,
not production activation or independent approval.

## Environments

PC: Linux x86_64, Python 3.12.3, Node 24.12.0, CMake 3.28.1,
Chrome 154.0.8037.57. Matching existing Mosquitto/libmosquitto 2.0.18 were
used from the previously prepared private dependency root; no packages were
installed. ARM64 cross compilation used the existing readonly offline SDK,
manifest SHA256 `7ddd4a9356cff1a7adc08b52b270f0bf13e014aaf0a27085630842d7ce979ae9`.

Actual Firefly: RK3588S/aarch64, Ubuntu 22.04.5, kernel 6.1.118,
RKLLM 1.3.0 / driver 0.9.8, existing Runtime 1024 / Admin 4096 models,
existing STT/VAD, accepted Amina/Amila model and pinned F2 assets.
The installed release remained `m20-master-9ca85f8` throughout.

Private evidence root on the PC:
`/home/siovush/firefly-m19-control/evidence/operational-voice-f2-20261101`.
Temporary device fixtures use `/home/firefly/operational-f2-test-20261101`.
Neither raw owner recordings, credentials nor model weights are published in Git.

## Automated checks

| Check | Result |
| --- | --- |
| Clean host MQTT=OFF CMake/build/CTest | **79/79 PASS** |
| Clean host MQTT=ON CMake/build/CTest, real local dependency binaries | **82/82 PASS** |
| Related tests after final classifier adjustment | **5/5 PASS** |
| Operational schema/dialogue/RuntimeWorker checks | **80 PASS** |
| F2 incremental framing / literal facts, all byte splits | **696 PASS** |
| F2 speaker lifecycle/concurrency/cancellation/error checks | **22 PASS** |
| F2 speaker ASAN + UBSAN + leak detection | **22 PASS**, no sanitizer finding |
| Frontend typecheck / unit tests / offline build | **PASS / 66/66 PASS / PASS** |
| Headless Chromium against actual C++ HttpServer | **PASS**, nine flow checks |
| Real aarch64 demo, test replay executable, config CLI and MQTT workers | **BUILD PASS**, actual device execution |
| Archived real M19 CLI → new settings → full checkpoint restore | **PASS**, current/history/CAS and preserved newer tree |

Host mode excludes the real RKLLM/ALSA appliance targets; those were separately
cross-built and exercised on Firefly. Optional DNS namespace tests were not
enabled in this feature matrix: MQTT transport/DNS subprocess source is unchanged.
They are not counted as passed. CTest's legacy DISPLAY-gated browser registrations
were absent; the new explicit headless Chromium acceptance was actually run.
No skipped test is being used as a positive result.

Chromium checked: Cancel and local staging leave storage unchanged; readable
Review includes both preferences; exactly one Save followed by Apply; same PID;
reload persistence; competing revision rejected with no retry/overwrite; guarded
speech stop; zero device execution and zero external asset requests.

The real HTTP fixture additionally checked an exact ON/OFF POST pair, read-only
questions and negative controls with zero additional POST, independently polled
Confirmed state, Save versus Apply, persisted restart and Unknown physical state
after restart. Adversarial Qwen outputs, including executable-looking JSON,
cannot escape the question route.

## Firefly checks

The existing voice service was stopped only within bounded transient test windows
and restored to runtime/STT ready and `WAITING_WAKE`. Temporary private stores,
an authenticated Mosquitto on loopback port 19843 and a loopback HTTP device
fixture were used. Production broker, physical relay topics and live ConfigStore
were not test endpoints. The test credential directory was bound readonly into
only the transient unit's mount namespace.

Normal command cases used actual accepted KWS, MicroFrontend, VAD, STT,
RuntimeWorker, SafetyGate, binding and executor classes. The uninstalled replay
executable changes only PCM input to a paced private WAV. An existing owner wake
recording was followed by synthetic test utterances. This is PCM replay through
real models/hardware, **not a new live owner utterance or acoustic acceptance**.
Actual F2 PCM was written to the existing ALSA playback device.

Passed cases:

- MQTT ON and HTTP ON positive controls reached their isolated endpoints and
  spoke Accepted/Predicted, explicitly without physical confirmation.
- Negation and unsupported utterance spoke a refusal with no additional command.
- In one process: ON → “Ты включила свет” → “А он сейчас работает” → “А на
  кухне” → Qwen-classified “Как там свет на кухне” → negation → failure question
  → two Setup/Cancel recovery cycles → OFF. Exactly two command PUBLISH packets
  (ON/OFF); two separate state-topic fixture publications are independent
  telemetry controls, not commands. Questions/recovery created no extra command.
- Before telemetry, the answer was Predicted. After an authenticated independent
  state packet, it was Confirmed. After freshness expired, Qwen's state question
  received “показание устарело, текущее значение неизвестно”.
- Speech-stop during real Qwen processing suppressed the pending PCM; subsequent
  replies worked. A repeated wake waveform while speaking was discarded.
- Both actual Admin 4096 entry/Cancel cycles released F2/runtime STT/VAD and Runtime,
  admitted Admin only after the 6 GiB check, restored Runtime and kept one NPU
  owner. A subsequent command reloaded F2 and succeeded. No automatic replay.
- Preferences changed through the actual HTTP Save/CAS/Apply path in the same
  native process. Save alone left active speech policy unchanged; Apply muted
  speech and disabled questions.
- Missing TTS assets preserved an actual Accepted command result, reported the
  speech failure and did not republish the command.
- Stop during real ALSA playback cancelled output. The next question in the same
  process spoke successfully using the same cached TTS instance.
- A separate 12-second native session opened the actual stereo Array capture,
  channel 1, with the candidate as its sole microphone owner. No new recording
  was saved and no owner utterance was requested. The service recovered afterward.

The final monitored multi-turn window kept at least **1712.68 MiB available**;
peak process RSS was **5550.53 MiB** (including mapped model/DMA memory),
peak FDs 20 and threads 32. Idle FDs returned to 15; cached F2/ORT worker threads
were bounded (27 total after the final turn), including after Setup and speech
cancellation. All candidate processes exited before restoration. These bounded
checks do not prove day-long leak freedom. The final live ConfigStore inventory
matched its initial SHA256 inventory exactly (eight files), release/unit pins
and Amina selection matched, and only the original service owned RKLLM afterward.

The native fixture initially rejected an artificial MemoryMax=6G because charged
cache left insufficient cgroup reserve. Subsequent fixtures used the installed
service's unlimited MemoryMax policy, with the unchanged 6 GiB admission / 1 GiB
physical reserve checks, finite session lifetime and a separate memory watchdog.
A synchronous 60-second test-client Setup request timed out during actual model
verification/loading; the final fixture used the existing asynchronous Setup API.
A TIME_WAIT port preflight was corrected to allow reuse of closed sockets while
still rejecting live listeners. These failed fixture attempts remain in evidence.

Initial classifier instructions returned a safe clarification for a colloquial
question. The final short prompt explicitly separates classifying a question from
knowing its answer. A direct real-model probe returned `[1]` in all six prompt/
contract controls; the final full KWS/STT path then returned the grounded stale
state above. Model-generated facts are never spoken.

## Limits and remaining production work

- Fresh owner speech, subjective listening/voice quality, distances/noise/acoustic
  self-echo matrix, barge-in and long soak are **NOT_RUN**. There is no voice
  barge-in implementation. Machine-confirmed ALSA output is not a claim of
  subjective audio quality.
- No physical relay commands, live-device telemetry acceptance, production
  installation, package finalization, pointer switch, release/tag or merge.
- Admin entry/Cancel checks are bounded recovery checks, not 4096-token saturation.
- TTS synthesis uses CPU: in the monitored multi-turn run, first PCM followed
  the first synthesis start by **8.81–10.90 seconds**, excluding STT, model
  classification and initial asset/model loading. This is a measured limitation,
  not an instant-response claim. The SDK cancellation
  callback is cooperative; a hard SDK hang still needs process supervision.
- Before production: independent code review, a normal ARM64 package with the
  pinned external F2 assets/provenance, compatible whole-store checkpoint and
  owner-authorized activation/rollback rehearsal, then live owner/acoustic tests.
  Do not relax the existing M19 checkpoint identity pin for an M20 store.

See [behavior, assets and recovery boundaries](operational-voice-dialogue.md).
