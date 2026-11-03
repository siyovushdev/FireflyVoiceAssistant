# Optional STT confirmation before the wake beep

This is an opt-in experiment for the after-beep input mode, not a new wake
model or command parser. The current Amina/Amila model, MicroFrontend, cutoff
0.99 and seven-score mean remain unchanged.

Start a reviewed appliance build with `--wake-stt-confirmation true` and
`--wake-command-mode after-beep`. The default is `false`; disabling the flag
restores the existing wake path. Auto/inline mode and disabled KWS cannot be
combined with confirmation. This flag is not a Web Admin/ConfigStore setting.

On a KWS candidate, the existing single capture owner retains up to three
seconds of its ASR-channel PCM, then collects 256 ms of real audio to finish
the word. No VAD, gain, normalization or synthetic silence is applied. The
already loaded command STT decodes this bounded window on its existing CPU
configuration, while capture keeps draining. If the whole context lacks the
name, the same recognizer makes at most one additional decode of the final
1.6 seconds. Models are not reloaded and no second capture process starts.

Only whole words `амина`, the owner-accepted STT alias `афина`, or the separate
tokens `а мина` confirm a candidate, with Russian case folding. There is no
fuzzy spelling repair, arbitrary substring match, or acceptance of any
nonempty transcript. This text is never passed to Qwen, CommandEngine,
SafetyGate, history, MQTT/HTTP executors or the reply/TTS path. A confirmed
candidate only allows the usual beep, drain and new command capture.

No beep or command admission occurs after rejection, STT error, cancellation
or the two-second admission deadline (including the 256 ms tail). One STT
job exists at a time. A late result is discarded; the slot is reused only
after that job joins. Shutdown/Setup also join it before releasing STT or
capture. The existing synchronous ONNX decoder has no hard cancellation API:
the deadline limits admission, not the decoder's CPU lifetime. An underlying
decoder hang could delay teardown, just as in command STT; this experiment
does not introduce an unsafe detached thread or force-kill the process.

`VOICE_STATE WAKE_DETECTED` still marks the original KWS candidate, preserving
direction-helper timing. Then `VERIFYING_WAKE`, `WAKE_CONFIRMED` or
`WAKE_REJECTED` show the additional decision. `WAKE_CONFIRMATION` JSON logs
the actual texts and timings of both STT attempts; `matched` is the textual
check, not final admission. `WAKE_CONFIRMED` is authoritative after deadline
and cancellation checks. `WAKE_CONFIRMATION_TIMEOUT` means no late beep.
Raw audio is kept in bounded RAM only by this feature. Existing separately
enabled wake-debug recording is unchanged. Logs can contain nearby speech.

## Recorded development checks

On the previously exposed 53 positive KWS candidates with an available real
256 ms tail, the real C++ `SherpaRecognizer` and `SttWakeConfirmation` confirmed
50/53. The three misses transcribed as `аминар`, `аминок`, and `мира`; they
remain misses and are not silently added as aliases. These are test clips,
not 53 different speakers. The two new owner-labelled noise clips and the
previous false `салют у нас` were all rejected. The third owner-labelled
positive was rescued by decoding the tail (`выходить на сами на` → separate
tail decode `амина`); the original transcript was retained.

Those four debug snapshots end at the original candidate. They have no
recorded post-candidate tail, so their replay tests the confirmation component
on the available PCM, not a full acoustic/production-loop replay. No missing
future audio is fabricated. The matrix was used during development; it is
not independent generalisation evidence, a zero false-wake claim, or a live
hardware acceptance. The flag must stay off by default pending an explicitly
authorized owner trial and evaluation of the additional misses/latency.

Unit/integration tests cover preserved PCM, aliases, noise rejection, errors,
late-result suppression, one slot, next-wake recovery, cancellation and the
appliance entrypoint. `wake_confirmation_replay STT_DIR INPUT_JSON REPORT_JSON`
is an explicitly run CPU-only saved-PCM tool; it never opens an audio device.
Input model/WAV hashes must be checked by the caller, as in the private replay
evidence. No user WAVs or additional model binaries belong in Git.
