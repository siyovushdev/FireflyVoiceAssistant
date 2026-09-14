# Milestone 5.5 full-STT wake feasibility

Isolated evaluation subproject. Production remains at
`882281c4d7856497668d73b658bec0bd84617c56`. No assistant, CommandEngine, RKLLM,
KWS, hardware actions, training or downloads are involved. Existing model files
and all production sources remain unchanged.

## Build and replay

```sh
cmake -S experiments/stt-wake-55 -B build/stt-wake-55/bin
cmake --build build/stt-wake-55/bin -j2
ctest --test-dir build/stt-wake-55/bin --output-on-failure
```

The standalone target compiles the existing `SherpaSpeech.cpp` and
`VoiceSegmenter.cpp` directly, with the installed 1.13.7 CPU library/header.
There is no linkage to the production core. Full Russian Zipformer retains two
threads and greedy decoding; Silero retains its existing one-thread settings.
The default segment configuration is unchanged: 300 ms pre/post-roll, maximum
speech 15 seconds. One VAD and one full STT model persist throughout a run.

The native runner streams WAV PCM in bounded memory, feeds 512-sample frames
and decodes only valid VAD utterances. It does not concatenate transcripts.
It preserves every emitted segment, empty result, failure and active EOF.
WAVs must be original 16 kHz, mono PCM S16_LE. There is no resampling, automatic
trimming, tail insertion or endpoint flush; recordings need trailing silence.
Long speech uses the existing rejection and quiet-recovery behavior.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 experiments/stt-wake-55/scripts/evaluate.py \
  --manifest build/stt-wake-55/archived-dev-manifest.json \
  --output build/stt-wake-55/results/NEW-RUN-NAME
```

Manifest schema: `split` (`archived_dev`, `fresh_dev`, or
`legacy_negative_audit`), `recording_complete`, and `cases`. Each selected case
has unique `id`, absolute `file`, `sha256`, boolean `expected_wake`, and optional
`prompt`, `mode`, `spoken_form`, `expected_suffix`, `pause`, `distance_cm`.
Positive rows need `spoken_form` and the expected suffix for exact-mode scoring.
All input WAV hashes are checked, and existing result directories are refused.
Initial local manifests are preserved in `build/stt-wake-55/` and copied into
each result directory. They reference original WAVs without modifying them.

Outputs: `run.json` records commands/source hashes; `raw.jsonl` and `stderr.txt`
retain native diagnostics; `samples.json` contains each event, candidate prefix,
wake decision, command suffix and timing; `summary.json` groups recall by mode,
pronunciation, distance and pause; `REPORT.md` gives a row for every event and
each provisional form, including samples where STT was skipped.

## Exact prefix definition

Two independent probes: `файрфлай` and `файерфлай`. Neither is selected as an
accepted policy. Preserve raw text; lowercase and collapse whitespace only.
A match requires the complete form at position zero followed by whitespace or
end of transcript. The command suffix comes from this minimally normalized
text. No punctuation stripping is currently necessary for this STT; punctuation
attached to the wake word is conservatively a nonmatch. No substring search,
word joining, fuzzy matching, typo repair, phonetics or LLM detection occurs.

`ФАЙРФЛАЙ  включи свет` matches the first probe. `я сказал файрфлай включи свет`,
`файрфлайка`, `файр флай`, `фаерфлай` and `firefly` do not. An alternate emitted
spelling is an observation, not an automatic alias.

Recall uses all expected-positive recordings, with each spoken form also
reported separately. Exact wake-word transcription means the first emitted
transcript begins with the **spoken reference form** as a whole word. The
one-utterance success metric requires exactly one emitted segment, one match,
an exact normalized reference suffix, and no active EOF. Wake-only exact-mode
success requires the same conditions with an empty suffix. Detection anywhere
among a negative recording's VAD segments counts as a false wake; segmentation
can make a later spoken name appear at the beginning of a new raw transcript.
Report that risk rather than using recording-level context to suppress it.
Record duplicate positive wakes and segmentation failures separately. Never
count repeated WAV replays as new independent evidence.

## Fresh human DEV recording

Run in a local terminal on the Firefly with H340 connected:

```sh
python3 experiments/stt-wake-55/scripts/record.py
```

`--list` prints all prompts without opening the microphone. The helper requests
Enter before every recording and prints SPEAK NOW after one second of leading
quiet. It uses existing `arecord`, fixed H340, 16 kHz mono S16_LE, and no inference.
All attempts, hashes, distances, pause instructions and retry reasons are
retained under `build/stt-wake-55/recordings/dev-TIMESTAMP/`. The displayed
`--resume PATH` command resumes a stopped session. Retry only a speaking/capture
mistake, without inspecting STT output.

24 positives: at approximately 5 cm and 50 cm mouth-to-microphone distance,
say each of the following with **Файрфлай**, then with **Файерфлай**:

1. The name alone, twice as separate recordings.
2. Name + «включи свет на кухне», with no deliberate pause.
3. Name + «в спальне поставь свет на семьдесят процентов», with no deliberate pause.
4. Name + «открой входную дверь», with a short natural pause (~0.2–0.3 s).
5. Name + «какая сегодня погода», with a short natural pause (~0.2–0.3 s).

12 negatives: the four commands/questions without the name; two similar-word
sentences; two continuous sentences with the name later; one short conversation
sentence; 60 s quiet background, 30 s room activity without speech, and 180 s
ordinary Russian speech. The helper gives exact Russian wording. For the long
conversation, speak naturally about your day, food and plans, without the product
name, using normal pauses. Do not read the positive prompts aloud during it.
Record practical distances and any deviations. No capture gain is changed.

## DEV and validation separation

1. Inspect archived DEV only for the initial spelling observation. Older EVAL
   negatives are explicitly labelled legacy audit, never fresh validation.
2. Record and inspect the new DEV set above, without changing STT/VAD/padding.
3. If a consistent exact form is supported, write a DEV decision with selected
   form(s), fixed normalization, source hashes, metrics and recording IDs before
   collecting validation. No choice is made from validation results. If no form
   is supported, preserve the rejection; do not substitute an alias to improve
   scores after the fact.
4. Only after that lock, collect new H340 validation recordings in a separate
   directory. Include both pronunciations and both distances, wake-only repeats,
   and every command with both pause conditions (40 positives), plus new
   negatives and sustained silence/conversation. This removes the DEV plan's
   command/pause confounding. The initial helper/evaluator deliberately do not
   enable validation before a DEV decision exists.
5. Score the locked policy without changes, report error categories and
   confidence limits appropriate to the sample size, then make one A/B/C
   recommendation and stop. A failed validation must stay failed.

No second-utterance command state or production wake gate is implemented.

## Performance interpretation

Native output distinguishes VAD compute time per frame, the audio-time interval
from the VAD-reported speech end to segment availability, STT wall/CPU time,
initialization, total processing CPU, current RSS and process cumulative peak
RSS. The endpoint interval is not an independently annotated human word-end
latency. A wake inside a longer utterance cannot be reported before that
utterance is complete; wake-only and one-utterance latency differ by design.

Default replay is accelerated. `cpu_percent_one_core_audio_estimate` divides CPU
seconds by original audio seconds. CPU/wall on an accelerated run is not live
CPU utilization. `--paced` delivers frames at their source times; its CPU/wall
is a paced replay observation, still excluding ALSA capture and production
thread orchestration. Synchronous decode can delay delivery, so maximum lag is
reported; no frames are dropped and this is not a live integration benchmark.
The WAV reader's memory is bounded, even for long conversation/silence.

For cost measurement, use separate paced sessions for fresh 60 s silence and
180 s conversation, retaining initialization and errors separately. STT must
have zero calls during VAD-negative silence. Real background noise can produce
VAD false positives and STT output, which must remain visible. Long-speech
rejections and uncovered speech are not successful transcription. Three minutes
of conversation is a practical smoke check, not sufficient exposure to establish
a low long-term false-activation rate.

The Milestone 4 **speech front end** already runs the same VAD while idle and the
same STT for eligible speech without a wake word. Prefix gating adds a small
text comparison after this work; it cannot save that STT cost. It could later
avoid command inference. Total Milestone 4 idle CPU/RAM, including its loaded
RKLLM model, is not measured here: invoking it is outside this experiment.

Initial observations: [initial report](../../docs/milestone-5.5-stt-wake.md).
Fresh DEV evaluation: [DEV report](../../docs/milestone-5.5-stt-wake-dev.md).
The tested exact forms were rejected on DEV; no spelling was selected and no
candidate proceeds to validation. The recording/validation plan above is retained
for provenance, not a request to collect more recordings for these failed candidates.
