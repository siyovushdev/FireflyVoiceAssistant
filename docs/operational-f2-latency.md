# Historical F2 response latency at 46ac3ea

The current [natural feedback policy](operational-natural-feedback.md) replaces
the acceptance phrases and uses silence for independently confirmed commands.
The measurements below describe the previous 46ac3ea/c1314dc implementation.

The first implementation synthesized every reply, including repeated sentences.
Warm-model hardware measurements at the initial `53429eb` source showed
8.813–10.903 seconds from synthesis start to first playback for common replies.
Those historical numbers exclude recognition, command processing and TTS load.

This change preserves Supertonic3 INT8 F2/SID1, ru, CPU4, five steps, speed1:

- Embed twelve complete synthetic F2 sentences with exact size/SHA256 pins.
  Stock replies bypass both model verification and model loading.
- Brief command receipts say executor acceptance and explicitly distinguish it
  from physical device state. Detailed results retain action/device facts.
- Keep complete plain-text sentences, playing sentence N while synthesizing N+1.
- Reuse only exact-text PCM, maximum 16 chunks / 8 MiB / 20s per entry, volatile
  and scoped to the existing audio lease. Facts and safety are recomputed first.
- Preserve cancellation, finite queues, model admission, Setup drain, generation
  checks, single capture ownership and the acoustic tail.

Build-time bank verification and CPU regressions include corrupted/missing/
symlinked audio, wrong profile, changed text, LRU limits, failed model admission,
cached playback cancellation, next successful turn and absence of SDK calls for
cold stock replies. The native `f2_latency_probe` is an explicitly invoked,
uninstalled playback test; never a CTest or production startup operation.

## Measured on Firefly

Playback measurements used real F2/ALSA and the installed pinned assets, with
synthetic fixture text. Each row below contains ten successive responses. The
standalone playback probe had no RKLLM/STT loaded; these dynamic timings must
not be presented as full-assistant timings.

| Standalone series | First PCM min / median / max |
| --- | --- |
| Cold stock, including new output object, deliberately absent model | 48.7 / 52.7 / 71.0 ms |
| Warm stock | 55.7 / 63.8 / 66.8 ms |
| New short text, first turn includes asset verification/model load | 1.767 / 2.018 / 3.523 s |
| Exact repeated short text from RAM | 56.7 / 60.9 / 62.4 ms |
| New longer two-sentence text, warm model | 2.903 / 3.157 / 3.191 s |

The separate actual appliance test used unchanged KWS/MicroFrontend/VAD/STT,
RuntimeWorker, command safety and a private real HTTP endpoint. Only the PCM
capture source was replayed from existing local recordings/synthetic speech;
there was no new microphone recording or physical relay command. Four explicit
ON/OFF utterances produced exactly four corresponding HTTP POSTs. From completed
result to first PCM: **64.9, 44.8, 47.8, 46.8 ms**. Stop during the second reply
cancelled playback; subsequent commands still worked without repeat dispatch.

A fifth, read-only state question correctly reported independently observed OFF
and sent no command. Its first new dynamic response took **13.120 s**, including
TTS asset verification/load; synthesis alone was **10.722 s**. Thus newly worded
answers in the full assistant can still be slow. The standalone probe's faster
dynamic timings are not a replacement for this measurement. No cause for the
difference is asserted here.

A real Runtime → Admin4096 Setup → Cancel → Runtime cycle released F2/cache and
returned to runtime/STT ready, WAITING_WAKE. Native FDs were 12 before/after;
threads went from 26 with cached F2 to 12 after teardown. The reserve remained
above 1 GiB. Both controlled windows restored the original service, matching
release/unit/Amina/debug and the full live-store hash/mode inventory.

First PCM means the first successful ALSA write, not measured acoustic onset.
The untrimmed stock acceptance WAV itself has about **360 ms** before its first
20 ms window above −45 dBFS. No leading audio was trimmed or normalized. Actual
speaker/room acoustic latency and subjective owner listening remain NOT_RUN.
The two-sentence acceptance recording lasts 5.579 s; fast onset is distinct from
its complete spoken duration.

## Validation and identities

Implementation/hardware source: `c1314dc3647b4bd0a58fb0f584adece4a57a4ecc`.
Subsequent build-generator hardening uses explicit exceptions (also under
`python -O`) and atomic output, with identical embedded PCM constants. The final
clean build/source identity and component equality are recorded in evidence.

- Full registered host CTest: **82/82 MQTT OFF, 85/85 MQTT ON**. Optional DNS
  namespace and DISPLAY-gated legacy browser registrations were not enabled.
- Frontend typecheck, **66/66** tests, offline build; explicit headless Chromium
  against the real C++ server, nine settings/CAS/stop checks: PASS.
- Actual public F2 output and worker regressions, pinned-bank negative cases,
  changed-text cache miss, caps/eviction, cancellation/recovery, rejected model
  admission: PASS. Speaker ASAN/UBSAN/LSAN: **45 checks**, no findings.
- Clean detached real AArch64 cross-build with the existing verified SDK,
  workers 0755; native dependency closure/build identity: PASS.

Private evidence: `/home/siovush/firefly-m19-control/evidence/operational-f2-latency-20261101`.
Two initial harness failures were retained: transferred test executables lacked
execute permission; a private connection fixture used an invalid stable ID.
Both failed closed and restored the original service. Only private test inputs
were corrected, then the complete successful runs above were performed.

Fresh owner speech/listening, acoustic echo/noise/distance tests, long soak,
physical relay and production activation remain NOT_RUN. Native startup/recovery
fixture targets were cross-compiled; their separate native-only CTests were not
run in this latency window. No latency result is inferred for those scenarios.
No permanent production installation, release or merge is part of this change.
