# Milestone 5.5: full-STT wake feasibility, initial observations

2026-09-13. **Historical initial observations.** The subsequent
[fresh DEV evaluation](milestone-5.5-stt-wake-dev.md) is complete and recommends
C: reject the unchanged full-STT exact-prefix gate for the tested wake forms.
No candidate advances to validation. The initial handoff and observations below
are preserved as the record before fresh DEV was available. Production remains at
`882281c4d7856497668d73b658bec0bd84617c56`. The initial exact-prefix results are
unfavorable; no accepted spelling was selected at that initial stage.

## Experiment and verification

The [isolated evaluator](../experiments/stt-wake-55/README.md) reuses the unchanged
full Russian Zipformer, Silero VAD and VoiceSegmenter defaults (300 ms padding
on each side). It decodes only VAD speech. It preserves raw STT and separately
tests exact whole-word prefixes `файрфлай` and `файерфлай` after lowercasing and
whitespace normalization. Internal spaces are never removed. No fuzzy match,
transcript correction, KWS tuning, training, download or production change.
No CommandEngine or RKLLM was built into or invoked by this experiment.

The standalone CMake build passed. The ExactWakePrefix suite passed all five
test methods, including middle-of-sentence names, longer words, split spellings,
cross-spelling rejection, and absence of stale suffixes. Native replay reproduced
all eight frozen Milestone 4 DEV speech transcripts exactly, and all three
synthetic silence WAVs skipped STT. Binary dependencies/symbols were inspected:
no CommandEngine, VoiceProcessor or RKLLM. Production source, includes, main
CMake and frozen text DEV data still match the checkpoint byte for byte.

## Initial archived DEV observations

These are four **previously recorded human H340 DEV** samples from Milestone
5.3, not fresh Milestone 5.5 validation. Original hashes were verified.

| Spoken reference | Raw full-STT transcript | STT ms |
|---|---|---:|
| Файрфлай | файфолай | 55.390 |
| Файерфлай | фаерфлай | 57.030 |
| Файрфлай включи свет на кухне | файр флай включи свет на кухне | 92.420 |
| Файрфлай на кухне поставь свет на сорок процентов | файр флай на кухне поставь свет на сорок процентов | 127.891 |

For **every row**, expected wake is yes. Under each independent provisional
form, the matched prefix is absent, detected wake is no and extracted suffix is
absent. Each WAV emitted exactly one STT segment. Both one-utterance samples
were recorded without a deliberate pause; distance was normal headset position,
without a recorded numerical measurement.

| Metric | `файрфлай` exact probe | `файерфлай` exact probe |
|---|---:|---:|
| Wake recall, all archived DEV positives | 0/4 | 0/4 |
| Wake-only recall | 0/2 | 0/2 |
| No-pause wake + exact command suffix | 0/2 | 0/2 |
| Recall on spoken Файрфлай | 0/3 | 0/3 |
| Recall on spoken Файерфлай | 0/1 | 0/1 |
| False wakes on initial archived negatives | 0/11 | 0/11 |
| False wakes on separate legacy negative audit | 0/5 | 0/5 |

Exact transcription of the spoken wake word at the first transcript prefix is
0/4. No short-pause or distance-sensitivity conclusion is available yet. The
observed `фаерфлай` and `файр флай` are retained as observations, not aliases.
The four clips do not establish a consistent form across modes/pronunciations.

## Existing negative material

For every row below, expected wake is no. Both provisional forms have no
matched prefix, no detected wake and no extracted suffix. All raw strings are
shown as emitted. The similar-word recording produced two segments; these are
reported separately, without joining their transcripts.

| Source/sample | Raw STT | STT ms |
|---|---|---:|
| M4 DEV 01_door | дверь | 49.205 |
| M4 DEV 02_open_door | открой дверь | 55.233 |
| M4 DEV 03_open_entrance_door | открой входную дверь | 74.523 |
| M4 DEV 04_light_on | включи свет | 53.467 |
| M4 DEV 05_light_off | выключи свет | 64.326 |
| M4 DEV 06_kitchen_40 | на кухне поставь свет на сорок процентов | 101.312 |
| M4 DEV 07_bedroom_half | в спальне свет наполовину | 75.534 |
| M4 DEV 08_corridor_off | в коридоре выключи свет | 75.574 |
| Synthetic silence 2 s | STT skipped; no transcript | — |
| Synthetic silence 5 s | STT skipped; no transcript | — |
| Synthetic silence 30 s | STT skipped; no transcript | — |
| Legacy 09_no_wake_light | включи свет на кухне | 61.403 |
| Legacy 10_no_wake_door | открой входную дверь | 59.953 |
| Legacy 11_conversation | сегодня хорошая погода после обеда я пойду гулять | 127.157 |
| Legacy 12_similar, segment 1 | файл открыт | 58.514 |
| Legacy 12_similar, segment 2 | флаг поднё фарфор стоит на полке | 115.333 |
| Legacy 13_room_noise, 30 s | учишь | 53.420 |

The initial negatives contain 64 s of human command recordings plus 37 s of
synthetic silence. The separate legacy audit adds 32 s of human speech and 30 s
labelled room background from the previously observed Milestone 5.3 EVAL set.
None of that material is new validation. There are 16 distinct negative WAVs,
163 seconds total, and zero false wakes for either probe. Padded recording
margins are included in these durations. Replaying a clip for performance does
not add independent exposure.

The room-background sample triggered VAD/STT and raw `учишь`; its acoustic cause
is unconfirmed. It produced no wake, but must not be counted as VAD-negative
silence. Fresh silence and room activity remain separate requested cases.

## Initial RK3588S performance

Recognition time excludes initialization and VAD endpoint delay. Across the 18
STT calls in the two unique-material replay runs, mean latency was **75.427 ms**,
range **49.205–127.891 ms**. No runtime/segmentation error or active EOF occurred.
The archived DEV run averaged **0.209 ms** per VAD frame, maximum **0.511 ms**.
Its wake segments became available 500 ms after the VAD-reported end; this is
not an independently annotated human word-end measurement.

A separate paced replay reused an 8 s kitchen command without wake followed
by the existing 30 s synthetic silence. It retained the full STT model in RAM.

| Measurement | 8 s no-wake command WAV | 30 s synthetic silence |
|---|---:|---:|
| STT calls | 1 | 0 |
| STT wall time | 102.969 ms | 0 |
| STT CPU time | 203.224 ms | 0 |
| VAD compute time | 60.080 ms | 229.118 ms |
| Total process CPU time | 349.158 ms | 263.991 ms |
| CPU utilization, one-core scale | 4.364% | 0.880% |
| Current RSS, start → end | 234348 → 246936 KiB | 246936 → 246936 KiB |
| Process cumulative peak RSS | 246936 KiB | 246936 KiB |
| Maximum source-frame delivery lag | 71.464 ms | 0.060 ms |

Initialization in that run took 9.103 s and is excluded from the table. Its peak
RSS is approximately **241.1 MiB**. The separate legacy-negative run peaked at
251052 KiB (245.2 MiB); these small runs do not establish a long-run memory bound.

The short command versus silence difference is 3.48 percentage points on a
one-core CPU scale at this specific speech frequency (one utterance per eight
seconds). It is not a forecast for continuous conversation. Recognition alone
cost 0.203 CPU seconds for 2.718 s of padded audio; sustained eligible speech
would keep paying STT costs even when no wake prefix is present. Long speech
over the unchanged 15 s limit is rejected and must be counted separately.

These are paced WAV observations, not native ALSA live measurements. Synchronous
decode explains the measured delivery lag; replay retains every frame. The
evaluator does not implement production capture concurrency or busy/quiet
rearming. Current Milestone 4 already has this same VAD/STT work for silence
and eligible no-wake speech. A later prefix gate would add a small text check
and could avoid command inference, while preserving the STT expense. Total
Milestone 4 idle CPU/RAM including loaded RKLLM is **not measured** here, because
running it is outside this experiment. Fresh long silence/conversation and
distance measurements are pending.

## Fresh recording handoff

Run from the repository root in a local Firefly terminal:

```sh
python3 experiments/stt-wake-55/scripts/record.py
```

The helper displays the exact Russian phrase before each recording. Speak
only after SPEAK NOW, once per recording, and then remain quiet. Use approximately
5 cm and 50 cm from mouth to H340 microphone; do not change input gain. It
requests **24 positive and 12 negative recordings**, around 15 minutes including
prompts. There is no STT feedback during capture.

At **each distance**, say these with **Файрфлай**, then repeat with **Файерфлай**:

1. «Файрфлай» — two separate wake-only recordings.
2. «Файрфлай включи свет на кухне» — no deliberate pause.
3. «Файрфлай в спальне поставь свет на семьдесят процентов» — no deliberate pause.
4. «Файрфлай открой входную дверь» — a short natural pause after the name.
5. «Файрфлай какая сегодня погода» — a short natural pause after the name.

Then follow the helper for these negatives, without deliberate pauses inside
the sentences:

1. «Включи свет на кухне».
2. «В спальне поставь свет на семьдесят процентов».
3. «Открой входную дверь».
4. «Какая сегодня погода».
5. «Файл открыт, флаг поднят, фарфор стоит на полке».
6. «Фейерверк закончился, отправь файл Андрею».
7. «Я сказал файрфлай включи свет на кухне».
8. «Мы обсуждали название файерфлай после обеда».
9. «Сегодня хорошая погода, после обеда я пойду гулять».
10. Remain quiet for 60 seconds.
11. Make ordinary room sounds without speaking for 30 seconds: paper, typing, chair movement.
12. Speak ordinary Russian about your day, food and plans for three minutes;
    do not say the product name. Use natural conversational pauses.

All attempts are preserved under `build/stt-wake-55/recordings/dev-TIMESTAMP/`.
Send the printed session path when finished. A stopped session can be resumed
using the helper's displayed command. These recordings are **DEV only**. After
inspecting them, freeze any supported exact spelling policy before requesting
a separate fresh validation session. Do not use that later validation to choose
spellings. The final recommendation A, B or C is deferred until the required
fresh evidence is available; the current 0/4 results do not support integration.

Local raw results and per-candidate/per-event reports:

- `build/stt-wake-55/results/archived-dev-run1/`
- `build/stt-wake-55/results/legacy-negatives/`
- `build/stt-wake-55/results/paced-cost/`

Each contains source/manifest hashes, raw JSONL, stderr, per-sample metrics and
Markdown tables. All production reference files remained read-only. No fresh
microphone capture was started by the agent.
