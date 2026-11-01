# Amina v3 — Amila revision (2026-11-01 release inputs)

The owner selected the already trialled `quiet-1500` Amina model for future
normal releases. This change promotes its exact bytes into the existing CMake
and staging pipeline. It does not install or activate a release on Firefly.
The separately accepted [F2 conversation pilot](f2-voice-pilot-handoff.md) remains
outside the product runtime until a later integration task.

## Exact release identity

| Item | Identity |
| --- | --- |
| Revision | `v3-amila-20261029` |
| Checked-in source | `assets/wake/amina-v3-amila-20261029.tflite` |
| Installed model path | `share/firefly/wake/amina-v3.tflite` (stable basename) |
| Model bytes | 61,080 |
| Model SHA256 | `24000c0e3f9fd3249f56e5f836719aba8f7a3f23c1f4a2c571bdf0bf6cc8a1dd` |
| Worker SHA256 | `e9db1780ce3f560b04b4c1b94db6b4e018680d3582d8266ceed76cd14a45ffff` |
| Wake manifest SHA256 | `1873f5e3afd51678f5f81f28aeae67494daed0abd5fda98b0306daf78271bde1` |

The original source `assets/wake/amina-v3.tflite` remains byte-for-byte unchanged:
60,912 bytes, SHA256 `bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1`.
Old immutable releases and their original grounding anchor must be retained.
The package verifier accepts only the complete pinned legacy or promoted
model/worker/metadata triple, and rejects mixed triples and invented revisions.

Only the worker's model SHA constant changes. MicroFrontend, input conversion,
512-sample IPC, streaming state, threshold **0.99**, mean of **7** scores,
KWS lifecycle and STT are unchanged. The model retains int8 `[1,3,40]` input,
uint8 `[1,1]` output and output scale 1/256. It does not correct STT transcripts;
«Афина» in STT and an actual KWS decision remain separate observations.

## Qualification and limitations

Private source/training evidence: `/home/siovush/amina-amila-20261029`.
The frozen selection is `candidate/quiet-1500/candidate.tflite`.
`freeze.json` pins qualification SHA256
`319c6280c8febe6cbea0a1c7dc9fb381ce22568195c55cb71e3ca0fdbb0f6cfc`
and evaluator SHA256
`be2969066e176fa241ad1c2d51e716aa21285e97fe16eaa0dc8ac31eabcd6c4d`.
Raw user audio, private reports and training checkpoints stay outside Git.

The October 29 qualification passed all 28 predeclared gates. Preserved checks
include 38/38 exposed owner positives, 4/4 reviewed examples, 10/10 prior live
positive hits and 12/12 warm positives. The owner «Амила» training example and
27 transformed training probes produced zero wakes, as did 11 training and
3 development TTS negatives. These are training-fit/exposed regression results,
not an independent claim of unseen-speaker accuracy.

After freezing, fresh natural FINAL negatives contained 60,681.96 seconds with
zero wake events; two FINAL TTS negatives (10.225 seconds) also had zero events.
The six-hour background regression still had **three** shadow wake events,
the same count as the prior candidate. This revision does not promise zero
false wakes or validated coverage for every accent, age or gender.
The October 29 native preflight matched 60 files / 10,632 blocks between normal
and diagnostic workers, including preserved continuation tails.

Fresh November 1 replay uses the checked-in Engine and its real subprocess
entrypoint, without model-SHA overrides: **110 files / 17,734 blocks**, all wire
replies and recorded max scores/mean7 equal the qualified results. It includes
37/37 remaining owner positives, the additional owner A positive, 4/4 reviewed
examples, 10/10 prior live positive hits, and zero wakes on the 28 owner-derived
Amila negatives and 14 train/dev TTS negatives. Two diagnostic prior-miss
positives still include one miss; they are not silently counted as successes.
These are repeated regressions on existing recordings, not new owner sessions.
Private fresh evidence is under
`/home/siovush/firefly-m19-control/evidence/amina-release-20261101`.

## Build, package and recovery boundary

Use the existing [build guide](build-and-test.md) and
[release procedure](release-procedure.md). Default CMake now copies the new
small model into the build/install tree under the stable installed filename.
For the first release of this revision, omit `FIREFLY_WAKE_MODEL_REFERENCE`
and `--wake-reference-release`; reuse the seven large installed shared models
as before. The old grounding model cannot satisfy the new model's identity.
An explicitly requested immutable model reference must contain these exact
new bytes and pass all existing canonical path/ownership/hardlink guards.

The supervised preparation preservation gate permits exactly one coupled
legacy-v3 → Amila transition for the three files in the identity table, with
unchanged safe modes and other properties. Other libraries, unit, seed, shared
models and protected artifacts remain frozen; the pre-existing three explicit
MQTT/TLS library additions remain the only allowed library additions.
No checkpoint, ConfigStore or rollback algorithm changes are included.

`WakeReleaseContract` tests both exact profiles, mixed/tampered rejection,
strict preservation and real clean CMake data/component installation.
`AminaStreamingBackend` is opt-in with the existing pinned KWS Python runtime.
No cloud, microphone, MQTT publish, Qwen inference or systemd action is needed.

Before a future owner-authorized rollout, prepare and verify a fresh release
using the existing runner and checkpoint procedure. Check the selected model
and any pre-existing candidate/debug bind overrides explicitly: an override
must not conceal which model the installed release is using. Preserve the old
release and its anchor. This Git promotion does not remove debug overrides,
switch current/previous pointers, or claim a new ARM64 installation.
