# Operational voice dialogue and F2

This opt-in addition uses the existing single assistant process. It answers
operational questions and speaks concise exceptions to confirmed command success; it does not add
open-ended chat or a second command executor.

## Owner controls

Web Admin → Overview → **Голосовые ответы · F2** opens the existing System
editor. Choose **Выключено / Кратко / Подробно** and independently enable
**Вопросы о работе ассистента**. Review shows the exact proposed preferences;
Cancel changes nothing. Save persists a candidate, and explicit Apply publishes
it through the existing revision/hash CAS. Apply changes the running process
without restart. The initial defaults are speech off and questions disabled.

The optional `system.voice_dialogue` object contains `responses`
(`off`, `brief`, `detailed`) and `questions_enabled` (boolean). Both schema
versions validate it strictly. An absent object retains the previous canonical
configuration bytes/hash. Existing configurations are not rewritten at startup.

Questions disabled means a question receives a disabled-feature explanation
(if speech is enabled), never command execution. Speech off leaves diagnostic
text available. Diagnostics shows the active mode, F2 lifecycle/error, original
STT text, reply and whether Qwen classified the question. **Остановить речь**
sends a guarded same-origin `POST /api/voice/stop` with `{}`. It cancels current
or pending speech without changing a device, configuration or Setup session.

## What can be asked

Examples: “Ты включила свет?”, “Свет на кухне включён?”, “Что сейчас
включено?”, “Что ты сделала последним?”, “Почему команда не выполнилась?”,
“А на кухне?”, “А он сейчас работает?”, “Что ты умеешь?”.

Common questions use a deterministic read-only route. For other question-like
phrases, the existing serialized Runtime Qwen (context 1024) may choose one of
five bounded question codes. It cannot supply device IDs, facts, spoken prose
or commands. The original words must still resolve against the active catalog;
unknown/ambiguous references require clarification. An unrecognized model
response is rejected. Qwen's default command prompt is restored after the
classifier, including error paths.

Context is in RAM: at most six follow-ups within 120 seconds, tied to the
active configuration hash. A clearly referenced device may support “он/она”;
ambiguous references, new generations, Setup and restart clear or invalidate
context. Context never fills a missing **command** target. Unknown question objects do
not select other devices in the same room; failed scope clears the old referent.
A current-generation last-action answer retains its known device for follow-ups. Execution history
is the existing bounded process-local history; after restart the assistant
explicitly says that earlier execution evidence is unavailable. Playground
activity is excluded from operational history.

Facts come from the active ConfigStore generation, DeviceStateStore and
production execution events. Configuration initial values are not observations.
A transport ACK/HTTP success yields **Accepted / Predicted**. Spoken replies state
that executor acceptance does not confirm physical device state.
Only independent fresh observations yield **Confirmed**; stale observations
(default 60 seconds) are described as stale. Channel connectivity is not power
state. A delivery timeout means unknown outcome with no automatic retry.
Pre-execution rejections are associated with a device only when the original
request supports that association; otherwise the answer states uncertainty,
instead of presenting an older successful command as the latest attempt.
After exact target/scope validation, state/on-list questions may perform existing
configured observation reads: validated HTTP GET, or fresh data from the current
MQTT subscription. One overall 500 ms budget covers at most two devices (four in
detailed mode). HTTP reads use the smaller configured/remaining timeout; no
arbitrary URL/topic is constructed. MQTT questions never publish a state-request
or command. Unknown scope, disabled questions and command inputs perform no
question read. Failed HTTP reads cannot reuse older cached data as a new answer.
No cloud is used.

Commands continue through the original input → grounding → SafetyGate → domain
→ binding → registered MQTT/HTTP executor path. Negations, statements and
unsupported/ambiguous commands retain existing rejection protections. Replies
use bounded templates and configured labels/properties, not generated facts.

## F2 assets and ownership

The accepted [F2 pilot](f2-voice-pilot-handoff.md) supplies the sentence/queue/
ALSA implementation. The production CLI adds `--tts-model-dir /absolute/path`.
The directory must be canonical, owned by root or the assistant user, and not
group/world writable. Files must be regular, non-symlink, safely owned and not
group/world writable. Exact SHA256 and sizes are checked before Sherpa loads
anything. There is no automatic download or alternative voice selector.

| File | Bytes | SHA256 |
| --- | ---: | --- |
| duration_predictor.int8.onnx | 3700147 | c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db |
| text_encoder.int8.onnx | 36416150 | c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff |
| vector_estimator.int8.onnx | 78400833 | 20cd86fa5c6effedfda0e7cffe5b0569ca401c440a0c3a1d72bf39286c0db3fd |
| vocoder.int8.onnx | 25991073 | e923d60f53f95eb1ce235f1dc33ec56d9c057823c96fa6f8acf98f32b0da6152 |
| tts.json | 8253 | 42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09 |
| unicode_indexer.bin | 262144 | 8402ca48e5189a8950138580b0fff64db6f072f24ac07cd54ba8b2fbb9883b30 |
| voice.bin | 517168 | 67d5209b0ee8ce6c74105ffbe12fe6a7628aea3b4ba2fcb308a4a67938a93ce8 |

Profile: Supertonic 3 INT8, Russian F2/SID 1, CPU four threads, five synthesis
steps, speed 1.0, sherpa-onnx 1.13.7. Weights remain external/private (~145 MiB);
model provenance/license and release provisioning must be retained when a
production package is prepared. This change does not install those weights or
change the permanent unit/release pipeline.

Sixteen fixed full F2 sentences are embedded in the executable from SHA256-pinned
mono S16_LE/44.1 kHz WAV files in `assets/voice/f2`. These are synthetic F2
audio, not owner recordings. Stock replies play without TTS asset verification
or model creation, including the first turn. Build-time verification rejects
missing/corrupted files and profile changes. Command feedback is silent, in both enabled speech modes, only after matching
independent observations of every requested assignment, received/ordered no
earlier than this command. Transport ACK/PUBACK, prediction, stale, opposite,
partial or missing data never qualify. The bounded 500 ms window performs one
configured HTTP read or waits for existing MQTT telemetry, without retrying the
command. Empty expected effects cannot be silently called confirmed.

Otherwise feedback is short: “Команду отправила, ответа пока нет”, “Не удалось
подтвердить выполнение”, or a brief refusal/clarification. A contradictory fresh
sample says the device answered but execution was not confirmed. Technical
execution details stay in logs. One-device state questions can answer “Да”/“Нет”
from confirmed fresh power state, inverting OFF predicates correctly. Negative
or alternative questions report the actual value to avoid ambiguous yes/no.
Unknown or failed reads say “Сейчас не могу проверить”. Names and locations for
group/property/history replies come from configuration; no grammatical gender
of arbitrary device names or model-generated facts is guessed. History retains
whether the voice confirmation window succeeded, without calling it current state.
See [natural feedback behavior](operational-natural-feedback.md).

Dynamic TTS loads lazily once per audio lease, after command processing releases the
Runtime request. Loading requires 2 GiB free before and 1 GiB reserve after;
existing Runtime/Admin admission remains unchanged. Queues hold at most 32
text chunks and two audio chunks; every waveform is finite, 44.1 kHz and bounded.
Overshoot is attenuated, quiet input is not amplified. Operational text is
literal; configured names resembling markup are not silently stripped. Plain
operational replies are split only at sentence boundaries, without inserting
punctuation or cutting a sentence into word fragments. Playback of the first
complete sentence overlaps synthesis of the next.

An exact-text LRU PCM cache holds at most 16 chunks / 8 MiB (20 seconds per
entry), in RAM only, for this one fixed voice/profile. It stores no state or
execution decisions. Every input still goes through current-generation facts,
original command safety and normal speech admission before PCM can be selected.
Changed device state, names or wording select different PCM. Setup/recovery
teardown discards the entire cache with the audio lease. A cancelled or failed
voice response never repeats a device command. `F2_TRACE first_playback` records
the first successful ALSA write; `VOICE_REPLY_READY` allows measuring from the
completed command/question result, including cold speaker construction. This
is software output timing, not acoustic loopback measurement.

KWS, MicroFrontend, VAD, STT, input channel 1, Amina weights/threshold/window
are unchanged. The existing capture loop drains microphone input while
processing, speaking and for a 500 ms echo tail. No second capture process is
opened. Beep and F2 playback are sequential. Audio resources, including cached
TTS, drain before AI Setup loads Admin Qwen. Apply/off, Setup, audio-stop and
shutdown interrupt speech. Failed TTS keeps the command result/diagnostic text;
it never resends a command. Missing/unsafe assets fail dynamic speech only; embedded stock replies remain available.

See [measured F2 latency and its limits](operational-f2-latency.md).

## Limits and recovery

No free chat, automatic learning, background room transcription or voice
barge-in. Use Web Admin audio-stop to interrupt F2. SDK synthesis cancellation
is cooperative; ALSA writes/drain are nonblocking and bounded. A 60-second
speech deadline requests cancellation, but an SDK hard hang still requires the
existing process supervisor's hard termination. Do not describe this as a
kill-isolated TTS subprocess. Spoken language remains bounded to configured
operational facts; unsupported vocabulary can require a clearer question.

Before a later **owner-authorized** deployment, preserve release pointers,
units, models and a verified **full ConfigStore checkpoint**, including history,
backups, LKG and metadata, using the existing release recovery procedure.
An old executable cannot read a history containing the new optional settings.
Restoring only `configuration.json` is insufficient. Use the established
checkpoint/quarantine rollback flow, preserve the newer store for analysis,
and verify old-binary audit/current/revisions/CAS/history/recovery in the
isolated fixture before activation. Never raw-overwrite the live store.
The existing `config-checkpoint.py` authority pins the archived M19 executable;
it accepts only a baseline that executable can read. Do not point it at an
already MQTT-enabled M20 store or change its identity pin to force admission.
The archived-M19 fixture proves whole-store restoration for that baseline;
production preparation must separately admit a checkpoint compatible with the
actually installed previous M20 release. That release/admission work is not
performed by this source-only feature PR.

This development task performs no activation, release-pointer switch or merge.
Hardware fixtures use separate private stores, loopback endpoints and transient
supervision with a restore action. On an interrupted diagnostic window, stop
only its named transient test unit, then start the original voice service and
verify `/api/status`: runtime/STT ready and `WAITING_WAKE`. Ordinary speech-off
is a reversible live setting; it does not revert configuration history.

## Verification entry points

- `OperationalDialogue`: actual RuntimeWorker/SafetyGate/domain/state/history,
  with explicit STT/model doubles and adversarial classifier outputs.
- `OperationalVoiceHttp`: real C++ HttpServer/ConfigStore and loopback HTTP
  device; exact positive dispatches, independent observation, zero additional
  side effects from questions/negative controls, persisted live preferences.
- `F2StockIntegrity`, `F2Output`: pinned embedded bank, corruption/missing/path
  rejection, cold stock without a TTS model, pending cancellation and recovery.
- `F2Sentence`, `F2Speaker`: literal text/queue/ALSA/TTS seams,
  cancellation, timeout, errors, subsequent successful turns and cleanup.
- Frontend typecheck/tests/build; `OperationalChromium.cjs` against the real
  C++ fixture tests Cancel, Review, one Save→Apply, persistence, CAS conflict
  and guarded audio stop.
- `operational_voice_native` is an uninstalled BUILD_TESTING-only executable
  using the same DemoMain with an explicit paced-PCM input seam. Normal
  `firefly-demo` has no replay environment switch. Synthetic/owner PCM replay
  is distinguished from live owner/acoustic acceptance in the test report.
