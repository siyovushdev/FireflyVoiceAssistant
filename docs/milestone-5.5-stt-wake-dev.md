# Milestone 5.5: fresh DEV evaluation

**DEV recommendation: C — reject the unchanged full-STT exact-prefix wake gate
for these product wake forms.** Dedicated custom wake-word training is the
recommended next approach when separately authorized. No training, integration,
or validation recording was started. Neither provisional spelling advances to
validation, and no alternate spelling was selected.

Both `файрфлай` and `файерфлай` returned **0/24 wake recall** on the fresh human
DEV set. All 24 positive recordings reached STT, so these misses are not caused
by silence gating skipping the positive recordings. STT transcribes the name
inconsistently. Examples include `фальфлай`, `файр флай`, `фай флай`, `файфлай`,
`фаерфлай` and `файф лайф`. These strings remain raw observations, without
correction, word joining or new accepted aliases. The four commands/questions
spoken without a wake word (`dev_25`–`dev_28`) transcribed exactly.

## Data and scope

Session: `build/stt-wake-55/recordings/dev-20260913T125751Z/`.
The user's message contained a placeholder path; this was the only completed
session in the prescribed output directory. The manifest identifies H340,
16 kHz mono PCM S16_LE, and the `fresh_dev` split. All 36 WAV hashes and formats
were verified against the manifest. There were 36 first attempts, all selected:
24 positives (240 s) and 12 negatives (360 s), totalling 600 s including recording
silence. No sample was excluded or replaced. Manifest SHA-256:
`ab4526ab81d9cbf9a1254af23c32397580a788328e55b4c7d2a1da60d61f777a`.

This is DEV only, not held-out validation. Archived results remain separate.
Original WAVs, model files and production sources were unchanged. The production
checkpoint is still `882281c4d7856497668d73b658bec0bd84617c56`. The existing
full Russian Zipformer, Silero settings, 300/300 ms padding and 15 s speech limit
were reused. CommandEngine, RKLLM and the rejected small KWS model were not
invoked. No new recording, resampling, gain adjustment or transcript correction
was performed.

The isolated build and all five ExactWakePrefix test methods passed. No harness
or prefix policy change was needed for this evaluation.

## Accuracy and failure categories

Each form is an independent exact-prefix probe after lowercasing and whitespace
normalization only. It must occupy the beginning of a raw transcript and end
at a whole-word boundary. No middle-of-sentence substring matching is allowed.

| Metric | `файрфлай` | `файерфлай` |
|---|---:|---:|
| Overall wake recall | 0/24 | 0/24 |
| Wake-only recall | 0/8 | 0/8 |
| One-utterance wake recall | 0/16 | 0/16 |
| No-pause wake + exact suffix success | 0/8 | 0/8 |
| Short-pause wake + exact suffix success | 0/8 | 0/8 |
| Recall at nominal 5 cm | 0/12 | 0/12 |
| Recall at nominal 50 cm | 0/12 | 0/12 |
| Recall on spoken Файрфлай | 0/12 | 0/12 |
| Recall on spoken Файерфлай | 0/12 | 0/12 |
| False-wake events / negative recordings | 0 / 12 | 0 / 12 |

Exact transcription of the spoken wake word at the first raw-transcript prefix
was **0/24**. Neither distance worked under either tested prefix. That does not
establish that distance has no acoustic effect: it means both conditions failed
this exact gate. Distances and pause conditions are recorded instructions, not
independently measured acoustic annotations. The DEV command/pause pairing is
confounded as documented in the recording plan.

All 24 positives had at least one STT result, producing 27 segments. The main
failure category is wake-name transcription mismatch (24/24). Two one-utterance
recordings split into separate wake and command segments: `dev_05` (short pause)
and `dev_21` (instructed no pause). The latter fails the one-utterance requirement
even apart from spelling. Wake-only `dev_01` produced two segments; the source
of its extra utterance is unconfirmed and was not silently discarded. There
were no runtime errors, overlength rejections, active EOFs or detected duplicate
wakes. Raw mismatches were not repaired and command suffixes were not invented.

Seven WAVs contain saturated PCM samples: `dev_01` (479), `dev_02` (49),
`dev_10` (28), `dev_15` (62), `dev_16` (66), `dev_29` (13), `dev_34` (8).
Five of those are positives; **all 19 unclipped positives also failed both
prefixes**. Clipping is a recorded quality limitation, not a demonstrated cause
of the STT errors or a reason to remove inconvenient samples. Gain and audio
were not modified. This is one human DEV session; no population recall or
long-term false-wake guarantee is claimed.

The fresh 60 s quiet-background recording and 30 s room-activity recording
emitted zero VAD utterances and invoked STT zero times. Both later-name negative
sentences produced no wakes; every emitted segment was checked independently.
Zero false wakes provides little reassurance when true wake recall is also zero.

## Conversation coverage

`dev_36` is 180 s long but yielded only **4.850 s of VAD-reported speech** in
three segments (6.650 s total padded STT audio): `а сегодня`,
`ресторан покушал семьёй поехали всё нормально`, and `к`. The VAD-reported
speech fraction is 2.69%. This is not evidence of three minutes of sustained
conversation. A question about actual speaking duration was sent to the user;
without confirmation, sparse speaking and missed speech cannot be distinguished
from the STT/VAD results alone. The clip's observed false-wake count is zero,
but conversation stress coverage remains unestablished. This limitation does
not rescue the failed positive wake tests or justify selecting a spelling.

## Performance

The primary 600 s DEV pass was accelerated WAV replay. Across its 42 STT calls,
mean decode latency was **80.225 ms**, median **74.199 ms**, range
**46.911–145.446 ms**. Initialization is excluded. Mean VAD compute time across
18,767 frames was **0.203 ms/frame**, maximum **0.499 ms/frame**. Every emitted
segment became available 500 ms after the VAD-reported speech end; this is not
an independent human word-end annotation. Wake-plus-command detection would
wait until the utterance endpoint before STT can return its prefix.

Primary-run peak RSS was 245080 KiB (**239.3 MiB**), process cumulative.
Separate paced replay of the three longer DEV clips measures CPU/RAM over
their original durations. Its results are recorded below. Replays do not add
independent examples or negative exposure.

## Reproduction and artifacts

```sh
cmake --build build/stt-wake-55/bin -j2
ctest --test-dir build/stt-wake-55/bin --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 experiments/stt-wake-55/scripts/evaluate.py \
  --manifest build/stt-wake-55/recordings/dev-20260913T125751Z/manifest.json \
  --output build/stt-wake-55/results/NEW-UNUSED-DEV-RUN
```

Outputs are preserved under
`build/stt-wake-55/results/fresh-dev-20260913T125751Z/`: original native
`raw.jsonl`, `stderr.txt`, copied manifest, `run.json` with source/binary hashes,
`samples.json`, `summary.json`, all-event/all-candidate `REPORT.md`,
`dev-audit.json`, and `dev-decision.json`. The latter records the DEV rejection,
empty accepted-form list, evidence hashes and no candidate proceeding to
validation. Paced runs have their own source-linked manifests and result
directories. No request for further recordings is required to reject these
failed DEV candidates; a future training task needs a new instruction.

## Paced CPU/RAM measurements and cost interpretation

Each longer clip was replayed in its own process, with one persistent full-STT
model. Initialization took 9.268–10.263 s and is excluded from the table.
All paced transcripts and segment boundaries exactly matched the primary replay.
No runtime errors or active EOFs occurred. No microphone capture was involved.

| Measurement | Quiet background, 60 s | Room activity, 30 s | Conversation-labelled WAV, 180 s |
|---|---:|---:|---:|
| STT calls | 0 | 0 | 3 |
| Total VAD compute ms | 827.012 | 225.354 | 1396.445 |
| Total STT wall ms | 0 | 0 | 260.515 |
| Total STT CPU ms | 0 | 0 | 504.715 |
| Total process CPU ms | 953.709 | 264.642 | 2358.721 |
| CPU utilization, one-core scale | 1.589% | 0.882% | 1.310% |
| Current RSS, start → end, KiB | 233000 → 233736 | 234052 → 234456 | 232500 → 251024 |
| Process peak RSS, KiB | 233736 | 234456 | 251024 |
| Process peak RSS, MiB | 228.3 | 229.0 | 245.1 |
| Maximum source-frame delivery lag, ms | 0.083 | 0.062 | 117.511 |

The 60 s quiet recording kept STT unused throughout, with end RSS only 736 KiB
above its start. Room activity likewise skipped STT. The conversation-labelled
WAV incurred 0.505 CPU seconds of STT work for its three VAD segments, but its
low detected speech fraction prevents using 1.31% CPU as a sustained-speech cost
estimate. The separate runs are observations under their execution conditions;
the lower average CPU of that sparse clip than of the quiet run is not a claim
that speech is cheaper than silence. The paced conversation decode times were
61.292, 148.959 and 50.264 ms. Synchronous STT causes source-frame delivery lag;
the replay retains every frame and is not production ALSA concurrency.

For another concrete cost observation, the four short no-wake commands/questions
in the primary run consumed **549.715 ms of STT CPU** for **7.256 s of padded
audio**, across four 10 s WAVs. This is roughly 0.076 CPU seconds per padded
audio second at that workload, or 1.37% of one core averaged over those 40 s
of recording for recognition alone. These are estimates derived from an
accelerated replay, excluding VAD/capture; they are not a direct measurement
of continuous conversation CPU utilization.

Current Milestone 4 idle already uses the same VAD and skips STT for VAD-negative
silence. Eligible speech without a wake also already incurs this full-STT cost.
A later text-prefix gate would therefore save none of that recognition work;
it could only avoid subsequent command processing. Total production idle CPU
and RAM, including a loaded RKLLM model, were not measured because that engine
was outside the authorized experiment. The measured 228–245 MiB is the isolated
speech evaluator, not the full Milestone 4 assistant footprint. No production
performance or wake-gating integration claim is made.

Paced evidence: `paced-silence/`, `paced-room-noise/`, `paced-conversation/` and
`paced-verification.json` beneath the fresh DEV result directory. Their separate
manifests refer to the original hashed session WAVs. Replays count as performance
checks only, not extra validation samples or extra independent negative exposure.

## Per-sample raw results

The two independent provisional probes have identical outcomes on every row below:
no matched prefix, no wake and no extracted suffix. This table applies separately
to `файрфлай` and `файерфлай`; it does not define a combined accepted policy.
Multiple events from one WAV remain separate. Empty cells marked — indicate absence,
not a repaired transcript or a command sent for execution. Full prompt and pause metadata
remain in the copied manifest and samples.json. Distances are nominal centimetres.

| Sample/event | Expected wake | Mode / cm | Raw STT transcript | Matched prefix | Wake | Suffix | STT ms |
|---|---|---|---|---|---|---|---:|
| dev_01/1 | yes | wake_only / 5 | `фальфлай` | — | no | — | 54.913 |
| dev_01/2 | yes | wake_only / 5 | `вайрфлай` | — | no | — | 46.911 |
| dev_02/1 | yes | wake_only / 5 | `файр флай` | — | no | — | 55.964 |
| dev_03/1 | yes | one_utterance / 5 | `в альфай включи свет на кухне` | — | no | — | 83.071 |
| dev_04/1 | yes | one_utterance / 5 | `файфы в спальный пасо свет на семьдесят процентов` | — | no | — | 113.162 |
| dev_05/1 | yes | one_utterance / 5 | `файфэл` | — | no | — | 53.626 |
| dev_05/2 | yes | one_utterance / 5 | `открой входную дверь` | — | no | — | 70.245 |
| dev_06/1 | yes | one_utterance / 5 | `файф лай какая сегодня погода` | — | no | — | 95.412 |
| dev_07/1 | yes | wake_only / 5 | `фай флай` | — | no | — | 54.352 |
| dev_08/1 | yes | wake_only / 5 | `фай флай` | — | no | — | 51.562 |
| dev_09/1 | yes | one_utterance / 5 | `файф лайф включи свет на кухне` | — | no | — | 89.520 |
| dev_10/1 | yes | one_utterance / 5 | `файф лайф в спальной поставьте свет на семьдесят процентов` | — | no | — | 140.629 |
| dev_11/1 | yes | one_utterance / 5 | `файфлайф открой входную дверь` | — | no | — | 84.935 |
| dev_12/1 | yes | one_utterance / 5 | `файф лай какая сегодня погода` | — | no | — | 86.524 |
| dev_13/1 | yes | wake_only / 50 | `файфлай` | — | no | — | 53.109 |
| dev_14/1 | yes | wake_only / 50 | `файфлай` | — | no | — | 53.069 |
| dev_15/1 | yes | one_utterance / 50 | `файф лайф включи свет на в кухне` | — | no | — | 109.340 |
| dev_16/1 | yes | one_utterance / 50 | `фальф лайф в спальне поставь свет на семьдесят процентов` | — | no | — | 118.014 |
| dev_17/1 | yes | one_utterance / 50 | `фай флай открой входную дверь` | — | no | — | 87.527 |
| dev_18/1 | yes | one_utterance / 50 | `фай фай какая сегодня погода` | — | no | — | 102.115 |
| dev_19/1 | yes | wake_only / 50 | `фаерфлай` | — | no | — | 53.197 |
| dev_20/1 | yes | wake_only / 50 | `файр флай` | — | no | — | 56.543 |
| dev_21/1 | yes | one_utterance / 50 | `файфра` | — | no | — | 53.033 |
| dev_21/2 | yes | one_utterance / 50 | `включи свет на кухне` | — | no | — | 65.970 |
| dev_22/1 | yes | one_utterance / 50 | `файф лайф в спальне поставь свет на семьдесят процентов` | — | no | — | 126.485 |
| dev_23/1 | yes | one_utterance / 50 | `файр флай открой входную кверх` | — | no | — | 76.529 |
| dev_24/1 | yes | one_utterance / 50 | `фай флай какая сегодня погода` | — | no | — | 96.275 |
| dev_25/1 | no | negative / 5 | `включи свет на кухне` | — | no | — | 63.043 |
| dev_26/1 | no | negative / 5 | `в спальне поставь свет на семьдесят процентов` | — | no | — | 89.692 |
| dev_27/1 | no | negative / 5 | `открой входную дверь` | — | no | — | 66.770 |
| dev_28/1 | no | negative / 5 | `какая сегодня погода` | — | no | — | 64.688 |
| dev_29/1 | no | negative / 5 | `файл открыт файл поднят` | — | no | — | 94.201 |
| dev_29/2 | no | negative / 5 | `фарфор стоит на полке` | — | no | — | 73.404 |
| dev_30/1 | no | negative / 5 | `фейевеш закончился` | — | no | — | 74.994 |
| dev_30/2 | no | negative / 5 | `отправь файл андрею` | — | no | — | 72.532 |
| dev_31/1 | no | negative / 5 | `я сказал фай фалай` | — | no | — | 76.611 |
| dev_31/2 | no | negative / 5 | `включи свет на кухне` | — | no | — | 64.397 |
| dev_32/1 | no | negative / 5 | `мне обсуждали название фарифлей после обеда` | — | no | — | 113.473 |
| dev_33/1 | no | negative / 5 | `сегодня хорошая погода после обеда я пойду гулять` | — | no | — | 126.615 |
| dev_34/none | no | negative / 5 | STT skipped; no transcript | — | no | — | — |
| dev_35/none | no | negative / 5 | STT skipped; no transcript | — | no | — | — |
| dev_36/1 | no | negative / 5 | `а сегодня` | — | no | — | 61.533 |
| dev_36/2 | no | negative / 5 | `ресторан покушал семьёй поехали всё нормально` | — | no | — | 145.446 |
| dev_36/3 | no | negative / 5 | `к` | — | no | — | 50.011 |
