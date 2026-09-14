# Milestone 5.6 — custom wake-word training feasibility

Research checked: 2026-09-13. Status: **design complete; training and integration not started**.

Milestone 4 remains at `882281c4d7856497668d73b658bec0bd84617c56`. Its production sources, models, VAD/STT settings, CommandEngine, RKLLM configuration and protocol are unchanged. The rejected small Russian sherpa KWS and FULL-STT prefix approaches are closed. The latter's fresh DEV result was 0/24 wake detections, including 0/8 wake-only and 0/8 no-pause examples; see [the recorded evaluation](milestone-5.5-stt-wake-dev.md).

## 1. Recommendation

**Use OHF-Voice/micro-wake-word to train a dedicated MixedNet detector from random initialization, exporting a streaming quantized TFLite model.** Start with real Russian recordings and environmental augmentation. Treat TTS as an optional later ablation after its voice and output rights are documented.

This is a practical training candidate, not evidence that “Файрфлай” already meets product targets. Russian recall, command coarticulation, deployment-microphone transfer and RK3588S timings remain unmeasured.

The reasons to choose it are its small streaming network, maintained Linux inference reference, and independence from a pretrained English speech embedding. It can learn this acoustic pattern without a vocabulary, transcript spelling, ASR decoder or continuously running STT. Its additional cost is a TFLite runtime and careful reproduction of the feature frontend. [Trainer](https://github.com/OHF-Voice/micro-wake-word), [MixedNet implementation](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/microwakeword/mixednet.py), [Linux reference](https://github.com/OHF-Voice/pymicro-wakeword).

Expected custom artifact: **approximately 50–200 KiB**, with a 250 KiB pilot ceiling. Expected standalone native Linux process: **10–40 MiB RSS and roughly 0.5–3% of one RK3588S Cortex-A76 core**, single inference thread. These are engineering planning ranges, not board measurements. The RTX 3050 Laptop GPU with approximately 3.7 GB VRAM should be sufficient at a small batch size. No cloud GPU is required for the proposed pilot.

## 2. Framework comparison and exact research revisions

The revisions below identify the inspected sources; they do not authorize upgrading any production dependency. Repository HEAD and the latest release are deliberately distinguished.

| Candidate | Verified revision / activity | Product assessment |
|---|---|---|
| **microWakeWord** — `OHF-Voice/micro-wake-word` | HEAD `4665173cd35f1cff9a61e06fc427f124766c488e`, 2026-07-06; package metadata still says 0.1.0. Recent work includes TensorFlow 2.21 / NumPy 2 compatibility. Model-release tags are not trainer version numbers. | **First choice.** Existing streaming export and embedded deployment experience; trainer still requires engineering and explicit data preparation. |
| **openWakeWord** — `dscripka/openWakeWord` | Latest release v0.6.0, 2024-02-11, tag commit `c8ef6912c5feccf1037b852d9bc6c7ed644135ba`; inspected HEAD `368c03716d1e92591906a84949bc477f3a834455`, 2025-12-30. Slower recent release cadence, not archived. | Credible fallback if frozen embeddings prove useful on Russian. Easier head training, larger continuous workload and less direct Russian support. |
| **Small ONNX classifier: Heed** — `AndreiBulzan/heed-wakeword` | v0.1.3, 2026-05-25, tag commit `0ca4d4b72da4a10139fdb9bd311e8f9803746861`; HEAD `fbf7722adafcbb76981994d876040c48884293c2`, 2026-05-27. Repository created 2026-05-23. | Concrete PyTorch/ONNX option, but too little maintenance history to call it the simpler proven product path. Reserve option, not a second 5.7 training project. |
| **sherpa / icefall / k2 custom KWS** | sherpa-onnx v1.13.8, 2026-09-10, tag commit `11afbd009a7f8c08f4bcf2fc1b265d0df4670fbf`; icefall HEAD `3f848bb6d0acc970c9b294a30ca0a04a7c9c78d1`; k2 HEAD `ec31d2c96e04665eadb276ebcfd436892f74b0aa`. Active upstreams. | Strong native runtime, disproportionate acoustic-model training work for one fixed word. Exclude from 5.7. |

Activity/release evidence: [microWakeWord commits](https://github.com/OHF-Voice/micro-wake-word/commits/main/), [openWakeWord releases](https://github.com/dscripka/openWakeWord/releases), [openWakeWord commits](https://github.com/dscripka/openWakeWord/commits/main/), [Heed releases](https://github.com/AndreiBulzan/heed-wakeword/releases), [Heed repository metadata](https://api.github.com/repos/AndreiBulzan/heed-wakeword), [sherpa release](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.8), [icefall revision](https://github.com/k2-fsa/icefall/commit/3f848bb6d0acc970c9b294a30ca0a04a7c9c78d1), [k2 revision](https://github.com/k2-fsa/k2/commit/ec31d2c96e04665eadb276ebcfd436892f74b0aa).

### Language, architecture and workflow

| Candidate | Russian/custom-word suitability | Training and export | Linux/aarch64 and C++ path |
|---|---|---|---|
| microWakeWord | Binary acoustic classification permits custom non-English words. No Cyrillic tokenizer is involved. Russian consonants, stress, vowel reduction and the transition into the following command matter through the recordings. Existing English model results do not validate Russian. | Fixed micro speech frontend → temporal mixed depthwise convolutions and pointwise convolutions → sigmoid. TensorFlow training on cached features; convert nonstreaming training model to stateful streaming TFLite, then INT8 quantize. | Native C feature frontend plus TensorFlow Lite C API, wrapped with C++17 RAII. OHF's Python wrapper supplies a Linux ARM64 reference using the C library. Native deployment need not include Python or full TensorFlow. |
| openWakeWord | Official README still says English-only. Training a custom Russian head is technically plausible, but transfer from its frozen encoder must be validated. Do not confuse spelling-independent acoustic input with verified multilingual performance. | Mel model → frozen Google speech embedding CNN → small dense or recurrent keyword head. Generate/record clips, augment, cache embeddings, train head. ONNX supported; TFLite conversion is also present. | ONNX Runtime CPU on aarch64 can run all three stages; C++ must reproduce buffering/features and head history. The MIT community C++ example was archived in October 2025; upstream's principal API is Python. |
| Heed | From-scratch binary acoustic model can consume Russian data; no language model. Its convenience TTS workflow is not proof of Russian performance or cleared voice rights. | PyTorch depthwise-separable Conv1D over 40 mel channels, global averaging, binary output. FP32/INT8 ONNX export; optional TFLite conversion. Published configurations have about 10k/27k/60k parameters. | ONNX Runtime C++ is a familiar deployment option. Need to port and verify filtering, mel features, normalization and rolling-window behavior. This is still adapter work. |
| sherpa/icefall/k2 | Custom acoustic training can support Russian, but language-specific symbols and sufficient Russian acoustic coverage are needed. This is not merely recording one word and fitting a binary head. | Streaming Zipformer/transducer encoder + decoder + joiner, keyword search. Official small Chinese recipe reports a 3.3M-parameter model trained on 10,000 hours of WenetSpeech. ONNX export is established. | Existing mature sherpa C/C++ runtime supports aarch64. Training data, tokenization, model preparation and alignment remain the expensive part. Do not resume the rejected Russian checkpoint. |

Sources: [microWakeWord training notebook](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/notebooks/basic_training_notebook.ipynb), [streaming conversion](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/microwakeword/utils.py), [openWakeWord architecture and language statement](https://github.com/dscripka/openWakeWord/blob/368c03716d1e92591906a84949bc477f3a834455/README.md), [community C++ example](https://github.com/rhasspy/openWakeWord-cpp), [Heed model](https://github.com/AndreiBulzan/heed-wakeword/blob/fbf7722adafcbb76981994d876040c48884293c2/heed/model.py), [Heed export/frontend requirements](https://github.com/AndreiBulzan/heed-wakeword/blob/fbf7722adafcbb76981994d876040c48884293c2/docs/export-and-deploy.md), [icefall KWS results](https://github.com/k2-fsa/icefall/blob/3f848bb6d0acc970c9b294a30ca0a04a7c9c78d1/egs/wenetspeech/KWS/RESULTS.md).

### Size, runtime and latency expectations

The following **CPU/RSS ranges are unbenchmarked native-process planning estimates**, on one A76 core with one inference thread. They exclude resident STT/RKLLM models. Python tooling can use substantially more RAM. They are useful for budgeting, not evidence of a measured performance ranking.

| Candidate | Model storage evidence / estimate | CPU and standalone RSS planning range | Streaming timing implications |
|---|---|---|---|
| microWakeWord | Upstream v2 example models are 52,272–60,264 bytes; custom pilot budget 50–200 KiB. Example `okay_nabu` arena declaration is 26,080 bytes, which is **not Linux RSS**. | 0.5–3% of one core; 10–40 MiB. OHF's ARM64 TFLite shared library alone is 3,607,232 bytes on disk. | 10 ms feature hop; proposed stride 3 gives a score every 30 ms. Aim for ≤300 ms p95 after word completion. Context length is historical audio, not mandatory post-word waiting. |
| openWakeWord | Released mel ONNX 1,087,958 bytes + embedding ONNX 1,326,578 bytes, before the head. Example Alexa head 854,246 bytes: approximately 3.12 MiB total. Custom head can differ. | 1–10% of one core; 30–100 MiB. | Standard score cadence 80 ms. Embedding/head windows and smoothing add context and possible delay; allow roughly 100–400 ms as an initial latency budget to test. |
| Heed | Upstream reports FP32 41–235 KB and INT8 16–94 KB by size. Not measured here. | 0.5–5% of one core; 20–80 MiB. | Re-evaluates a rolling 1-second window every 100 ms. Not fully incremental neural inference; slow pronunciations may exceed the window. Allow 100–400 ms as an initial budget. |
| Small sherpa custom transducer | A 3.3M-parameter example implies ~13.2 MB of FP32 parameter payload before graph/runtime overhead. Different architectures vary. | 2–20% of one core; 40–150 MiB. | Chunking, token emission and keyword search determine latency. Hundreds of milliseconds are plausible, but no Russian custom-training latency can be promised. |

Size sources: [microWakeWord model tree](https://api.github.com/repos/esphome/micro-wake-word-models/git/trees/05b65922cc433c9df13e98e32a7fe520758c837e?recursive=1), [example metadata](https://github.com/esphome/micro-wake-word-models/blob/05b65922cc433c9df13e98e32a7fe520758c837e/models/v2/okay_nabu.json), [Linux runtime tree](https://api.github.com/repos/OHF-Voice/pymicro-wakeword/git/trees/503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1?recursive=1), [openWakeWord release asset sizes](https://api.github.com/repos/dscripka/openWakeWord/releases/tags/v0.5.1), [Heed deployment documentation](https://github.com/AndreiBulzan/heed-wakeword/blob/fbf7722adafcbb76981994d876040c48884293c2/docs/export-and-deploy.md).

microWakeWord wins this decision because continuous low-cost streaming is the product requirement. openWakeWord remains credible, but its chief advantage is cheap head training. Heed offers ONNX familiarity at the cost of a much younger stack and additional feature-porting work. sherpa custom training would make sense for broader multilingual keyword search with a substantial acoustic corpus, which is not this experiment.

## 3. Commercial licensing and provenance

**Code licenses, pretrained weights, training data and generated-audio rights are separate inputs to the decision. Training our own model does not automatically inherit the license of an unrelated pretrained wake model. Nor does an Apache code license clear arbitrary training data.**

| Component | Verified license implication |
|---|---|
| microWakeWord trainer; OHF Linux wrapper | Apache-2.0 code. Commercial use and modification are permitted subject to its terms. |
| `rhasspy/pymicro-features` | Apache-2.0 repository; retain the embedded frontend and third-party notices when packaging native components. |
| TensorFlow/TFLite | Apache-2.0 project. Include applicable notices for the exact runtime build and its bundled dependencies. A Python wrapper's license is not a substitute for the runtime's notice inventory. |
| Our random-initialized microWakeWord weights | No pretrained wake/embedding checkpoint is required. Choose our distribution terms based on the actual data permissions and any incorporated material; using the Apache trainer does not require publishing our weights or product source. |
| openWakeWord | Apache-2.0 code; README labels included pretrained models CC-BY-NC-SA-4.0, while separately identifying Google's upstream embedding as Apache-2.0. Avoid bundled NC keyword heads. For a commercial custom head, trace the exact converted feature artifact back to its permissively licensed upstream weights; do not paper over the broad README wording. Our trained head is not automatically NC-SA. |
| Heed | Apache-2.0 code. Own weights need their own data provenance. Optional TTS dependencies/voices require separate checks. |
| sherpa-onnx, icefall, k2 | Apache-2.0 code. A chosen Russian checkpoint and its training corpus would have separate terms; none is selected or cleared here. |

Verified license files: [microWakeWord](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/LICENSE), [pymicro-wakeword](https://github.com/OHF-Voice/pymicro-wakeword/blob/503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1/LICENSE), [pymicro-features](https://github.com/rhasspy/pymicro-features/blob/96bd69cfad79aa67697e176570d3dd87052c3def/LICENSE), [TensorFlow](https://github.com/tensorflow/tensorflow/blob/v2.21.0/LICENSE), [openWakeWord license statement](https://github.com/dscripka/openWakeWord/blob/368c03716d1e92591906a84949bc477f3a834455/README.md#license), [Heed](https://github.com/AndreiBulzan/heed-wakeword/blob/fbf7722adafcbb76981994d876040c48884293c2/LICENSE), [sherpa](https://github.com/k2-fsa/sherpa-onnx/blob/v1.13.8/LICENSE), [icefall](https://github.com/k2-fsa/icefall/blob/3f848bb6d0acc970c9b294a30ca0a04a7c9c78d1/LICENSE), [k2](https://github.com/k2-fsa/k2/blob/ec31d2c96e04665eadb276ebcfd436892f74b0aa/LICENSE).

For distributed Apache components: supply the license, preserve relevant copyright/attribution and NOTICE text, and mark changed source files. Apache permits proprietary surrounding software and includes a scoped patent grant; it does not grant trademarks. [Apache-2.0 terms](https://www.apache.org/licenses/LICENSE-2.0).

The recommended initial data policy is owned human recordings with explicit permission for commercial model training/distribution, our own environmental recordings, and individually documented CC0 or suitable commercial-use sources. Record the exact license text/version, source URL, author, attribution requirements and file hash. Exclude unknown and NC assets from this product-intended experiment. Evaluation recordings also need appropriate usage rights; public availability is insufficient.

Specific traps found in the recipes:

- microWakeWord documents WHAM! under **CC-BY-NC-4.0**, and FSD50K under mixed Creative Commons licenses. Do not download the recipe's whole negative/augmentation bundle and label it Apache. Likewise, do not treat precomputed features as provenance-free. [Upstream data sources](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/documentation/data_sources.md).
- Russian Piper `ruslan` lists **CC-BY-NC-SA-4.0** data; `irina` lists **Unknown**. Exclude both from this initial commercial path. [Ruslan card](https://huggingface.co/rhasspy/piper-voices/blob/main/ru/ru_RU/ruslan/medium/MODEL_CARD), [Irina card](https://huggingface.co/rhasspy/piper-voices/blob/main/ru/ru_RU/irina/medium/MODEL_CARD).
- `denis` and `dmitri` list **CC0** data, but also say they were fine-tuned from Lessac medium. Lessac's card links a research-only data license excluding commercial speech-product development. These cards do not establish a clean end-to-end permission chain for our use. Leave these voices unapproved until that chain is resolved; do not infer that their downstream weights are necessarily NC solely from the base data license. [Denis](https://huggingface.co/rhasspy/piper-voices/blob/main/ru/ru_RU/denis/medium/MODEL_CARD), [Dmitri](https://huggingface.co/rhasspy/piper-voices/blob/main/ru/ru_RU/dmitri/medium/MODEL_CARD), [Lessac](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_US/lessac/medium/MODEL_CARD), [original Lessac terms](https://www.cstr.ed.ac.uk/projects/blizzard/2013/lessac_blizzard2013/license.html).
- `piper-sample-generator` 3.2.0 is an MIT wrapper, but its dependency is `piper-tts==1.3.0`, from the **GPL-3.0** Piper successor. Using a GPL tool offline does not by itself make generated audio or our classifier GPL; redistribution/linking of the tool and voice/output permissions are distinct. Its special many-speaker blending model is English-only, so it is not a ready supply of diverse Russian speakers. [Generator source and dependency](https://github.com/rhasspy/piper-sample-generator), [Piper GPL license](https://github.com/OHF-Voice/piper1-gpl/blob/v1.3.0/COPYING), [GNU explanation of tool output](https://www.gnu.org/licenses/gpl-faq.html#CanIUseGPLToolsForNF).

This leaves a concrete path that does not depend on resolving TTS rights: **real recordings + owned/cleared environmental augmentation + random initialization**.

## 4. RTX 3050 training feasibility

The GPU capacity is supplied by the user; it was not inspected remotely. Time ranges below are estimates for the small pilot, excluding human data collection. Actual throughput, laptop power limits, host RAM, framework overhead and preprocessing will dominate the uncertainty.

| Approach | Approximately 3.7 GB VRAM | Training time order of magnitude | CPU / cloud |
|---|---|---|---|
| microWakeWord | **Realistically yes.** Start batch 32, fall back to 16. Cache features on disk; limit GPU allocator to about 3.0 GB. | Budget roughly **1–6 hours per 20k-step run**, plus hours for feature preparation; measure 500 steps before extrapolating. | CPU pilot is practical but budget roughly 4–24 hours/run on a modern multicore development CPU. No cloud required. |
| openWakeWord | **Yes for the small classifier head** over cached embeddings. Batch feature extraction/TTS separately. | Head: minutes to a few hours; full preparation commonly dominates, so reserve several hours. | CPU head training is practical. GPU mainly helps preparation. No cloud required. |
| Heed | **Yes** for these small CNNs and short feature windows. | Tens of minutes to a few hours for a pilot; tuning/data work longer. | CPU pilot practical, usually hours. No cloud required. |
| sherpa custom acoustic training | Not a credible GPU budget for reproducing the cited large-corpus training recipe. Small-batch fine-tuning may fit a selected small model, but feasibility depends on that model and training recipe. | Fine-tuning: hours–days; corpus-scale training: days–weeks or longer. These are broad planning estimates. | CPU corpus-scale training impractical. A 16–24 GB GPU might support constrained fine-tuning; full recipes may require multiple GPUs. Not justified for 5.7. |

For microWakeWord use an isolated x86_64 Linux or WSL2 training environment on the RTX laptop, assuming it is available. Modern TensorFlow GPU support is Linux/WSL2; native Windows GPU support stopped after TensorFlow 2.10. Start from Python 3.11 and TensorFlow 2.21.0, matching the trainer's recent compatibility work; lock all resolved package versions and hashes after the installation/export smoke test. Do not install training dependencies on the voice appliance. [TensorFlow installation requirements](https://www.tensorflow.org/install/pip), [trainer requirements](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/setup.py), [TensorFlow 2.21.0 package](https://pypi.org/project/tensorflow/2.21.0/).

Prefer at least 16 GB system RAM and approximately 50 GB free disk for the pilot, with feature caches kept bounded. Forty hours of 16 kHz S16 mono PCM alone is about 4.6 GB. Do not run GPU TTS concurrently with training. If cloud is later useful for turnaround, a **T4 16 GB or L4 24 GB** is ample as a starting class for this tiny classifier; an A100 is not a prerequisite. No cloud job is proposed now.

## 5. Dataset design

### Human positives: initial feasibility cohort

Collect **1,200 fresh utterances from 20 speakers, 60 each**. Initial target: ten male and ten female adult speakers, diverse ages and regional Russian accents. This pilot cannot establish children's performance. If children will use the product, add a separately consented child cohort before that claim; do not simulate children merely by pitch-shifting adults.

For each speaker use two sessions, at least two rooms, and this 60-take matrix:

| Dimension | Allocation |
|---|---|
| Mouth-to-microphone distance | 0.3 m, 1 m, 2 m; 20 takes each |
| Mode | At each distance: 10 wake-only; 10 wake + immediate command with no deliberate pause |
| Delivery | For each mode/distance: normal, fast, slow, soft, and a comfortable natural pronunciation variation; two takes each |

Record 4–8 second clips with leading/trailing room sound, and preserve entire sessions when practical. Do not cut recording at the word's end. Capture short natural pauses as a supplementary stress set (at least 120 additional takes, allocated by the same speaker splits), reported separately from the 1,200-take no-pause/wake-only matrix.

Use the provisional spoken name naturally. Record actual pronunciation labels: two-syllable versus an inserted vowel, stress, vowel reduction and final consonant quality. Natural “Файерфлай”-like pronunciation can belong to the same acoustic product-name class if intended, but it must be declared from TRAIN/DEV before holdout. Do not adopt unrelated STT misrecognitions as new target words. The detector has no accepted textual spelling list.

Use varied immediate suffixes, including the previous light, brightness, door and weather sentences, plus arbitrary non-command follow-ons. Reserve some suffix texts for holdout. Match commands without the name in negatives, so the model cannot learn “включи свет” as the target. It must also work when the user says only the name.

H340 is one development condition. Aim for at least three **available** capture paths across the cohort: H340, an existing laptop/phone microphone, and another available microphone. Log native format, resampling and any unavoidable device processing. No hardware purchase or production-microphone selection is part of this plan. Include off-axis speech, soft furnishings, a reflective room and ordinary fan/noise conditions. Balance microphones across positive and negative labels.

Limit H340 to at most one-third of new TRAIN positive recordings. Include at least one DEV and one HOLDOUT capture-device model not used in TRAIN when practical. If only H340 data can be obtained, run an explicitly H340-only pilot and withhold microphone-transfer claims. Gain augmentation does not reproduce a different microphone's response or a distant room.

### Negatives

Initial continuous material: **20 hours TRAIN, 10 hours DEV, 10 hours HOLDOUT**, all unique source sessions. Per 10-hour block, aim for 5 hours ordinary Russian conversation, 2 hours rights-cleared TV/radio/podcast-like speech, 2 hours household noise, and 1 hour room tone/silence. Double those amounts for TRAIN. Include at least one uninterrupted 30-minute conversation section per evaluation split. Do not count a short conversational clip repeated many times as fresh negative hours.

Additionally record **600 scripted hard-negative takes, 30 per speaker**, divided with their speaker's split. Include:

- Commands without the name: “включи свет на кухне”, “открой входную дверь”, “какая сегодня погода”.
- Partial/confusable sounds: “файр”, “флай”, “файл”, “файлы”, “вай-фай”, “фейерверк”, “профиль”, “проверь файл”, “открой файл”. Include repetitions, hesitations and incomplete wake words.
- Different-speaker and overlapping speech, laughter, breath, coughs, keyboard/cutlery sounds, doors, water, vacuum/fan noise, music and television-style playback.

Audit labels by listening. A full intended pronunciation of the name is not an acoustic-negative example just because it appeared in conversation. Record quoted/mentioned names such as “я сказал Файрфлай” and exact-name TV playback as a **separate unwanted-activation challenge set**, at least 100 episodes across the cohort. Report its trigger rate prominently. A binary acoustic detector can recognize the name anywhere; it does not reproduce Milestone 5.5's textual beginning-of-transcript rule or infer whether speech was addressed to the device. If rejecting those mentions is a product requirement, that remains an additional contextual-policy problem, and a good acoustic score alone does not establish product readiness. Do not hide these events by excluding them from the household unwanted-activation report.

Existing 5.4/5.5 samples remain DEV/regression material. Retain clipping/quality metadata and report legacy failures separately; they are not fresh holdout. Any known legacy speaker must remain on the development side of new splits.

### Expansion before product claims

Plan approximately **8,000 real positives from 80 speakers**, split 50 TRAIN / 15 DEV / 15 fresh HOLDOUT speakers, 100 utterances each. Include at least 12 child/teen speakers if that is the user population, represented in all three groups; balance adult male/female coverage and collect enough examples for separate age reporting. Start with 100 hours negative TRAIN, 30 hours DEV and **at least 300 hours untouched negative HOLDOUT**. These are collection targets, not a guarantee that quantity alone will fix a confusable name.

Once a production microphone/array and its processing are known, record a new development adaptation cohort and a new untouched acceptance cohort on that hardware. Include multiple units, real installation positions and intended room distances. Revalidate the final signal path, including any later array processing. This milestone selects no microphone hardware.

## 6. Strict splits and annotations

| Pilot split | Speakers | Main real positives | Scripted hard negatives | Unique continuous negative audio | Permitted use |
|---|---:|---:|---:|---:|---|
| TRAIN | 12 | 720 | 360 | 20 h | Gradient updates, augmentation, calibration |
| DEV | 4 | 240 | 120 | 10 h | Model/checkpoint choice, threshold and smoothing selection, diagnosis |
| TEST / HOLDOUT | 4 | 240 | 120 | 10 h | One evaluation of the frozen candidate; no selection |

Assign speakers **before** extracting clips or creating augmentations. All sessions, microphones, cropped segments, retransmissions and augmented derivatives of a source follow its split. For public audio, group by known speaker and recording/program; where speaker identity cannot be separated reliably, keep that source out of fresh speaker-generalization claims. Group noise and impulse responses by recording/session/room; do not use HOLDOUT environments as TRAIN augmentation sources.

Do not use the notebook's convenient random clip split as the authoritative partition. Store an immutable manifest with `sample_id`, SHA-256, source/parent IDs, speaker/session/device/room IDs, split, license record, distance, mode, pronunciation, duration, quality flags, and wake/command annotation sample indices. Keep held-out recordings unavailable to the training/checkpoint-selection scripts, including automatic post-training `testing` paths.

Manually annotate wake start and completion, and the first command phoneme for no-pause cases. Mark uncertainty intervals for coarticulated boundaries; do not pretend a frame index is a perfect phonetic boundary. Review clipped/inaudible recordings and preserve the exclusion reason; record replacements in the same split. No full-STT transcription or forced correction is needed.

For positive training crops, include the complete name and end at or shortly after its completion, with varying preceding context and 0–150 ms of following audio. Do not label a random crop positive when it omitted the wake word. Include partial-word crops as hard negatives and long uncut streams for evaluation. Do not train the model to wait for a silence after the name.

Choose every operating parameter from TRAIN/DEV, then record the model/config/manifest hashes before opening HOLDOUT. If holdout failures inspire changes, that holdout becomes historical regression data; collect a new one for the revised model. Final product holdout is separate from the pilot holdout and is never used for threshold selection.

## 7. Audio and augmentation

Retain **16 kHz, mono, S16_LE** for inference and canonical WAVs. Keep original recordings when captured at another native rate; use one documented resampling/downmixing operation to create the canonical derivative. Preserve raw PCM for the rolling buffer. One second is 16,000 samples / 32,000 bytes.

For the wake branch use the selected framework's exact feature extractor, consistently in training and inference. microWakeWord's selected recipe uses a 30 ms analysis window, 10 ms hop and 40 features, including its frontend's gain/noise processing. These internal wake features do not change the samples or settings supplied to the existing VAD/STT branch. [Frontend reference](https://github.com/rhasspy/pymicro-features), [feature adapter](https://github.com/OHF-Voice/pymicro-wakeword/blob/503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1/pymicro_wakeword/microwakeword.py).

Initial TRAIN-only environmental augmentation proposal:

| Transform | Starting range / discipline |
|---|---|
| Keep clean examples | At least 25% of positive presentations remain unaugmented |
| Gain | Approximately −12 to +6 dB, reject newly clipped results; do not normalize all real distances to the same level |
| Background mix | Owned/cleared speech and household noise, SNR 5–25 dB; retain audibility and matched negative examples |
| Reverberation | Owned measured or documented simulated responses spanning approximately RT60 0.15–0.8 s; no evaluation-room responses |
| Speed / pitch | Modest independent variation, roughly 0.9–1.1× speed and ±1 semitone; useful variation, not new speakers |
| Device variation | Mild bounded EQ/bandwidth variation derived from development conditions; validate against real cross-device recordings |
| Timing | Random leading context and wake placement; preserve the annotated complete wake and natural command continuation |

Avoid aggressive time masks that erase this short word, destructive clipping, or augmentation so strong that the label becomes wrong. Generate equivalent environmental variations of negatives. Leave real DEV/HOLDOUT audio intact for primary metrics; report synthetic stress tests separately.

Optional later TTS arm: **5,000–20,000 TRAIN-only clips** from explicitly cleared Russian-capable voices/output terms, varied rates and suffixes. Keep synthetic examples to at most half of positive batch presentations. Two TTS voices plus pitch shifts do not constitute a multi-speaker validation set. Compare against the real-only baseline at matched seeds/budgets. Do not make TTS licensing resolution a dependency of the first training run.

## 8. Future runtime contract and command-audio recovery

Design only:

```text
persistent ALSA capture ──> rolling original-PCM buffer
                       └─> wake feature frontend + streaming classifier
                                            │ detection event
                                            v
                           future command-audio handoff
                                            │
                               existing VAD → FULL STT → CommandEngine
```

Before wake, full STT and RKLLM perform no inference. “Idle” can mean their model objects remain resident; their existing memory does not disappear merely because the wake model is small. The standalone 5.7 evaluator must not link or call CommandEngine/RKLLM or run the production voice executable. The wake detector consumes continuous PCM including silence; it does not depend on VAD opening first and potentially clipping leading fricatives.

Use a **5-second PCM ring** as an initial design budget: 80,000 samples / 160,000 bytes. Capture keeps writing while detection and later processing run. Rechunk audio inside the wake adapter as needed; model cadence must not depend on the incoming ALSA block size.

Proposed event fields:

- `model_id` and model/config hash; `stream_epoch` to distinguish capture restarts.
- Absolute `decision_sample_index`, defined as the exclusive end of captured audio consumed for that decision; monotonic capture timestamp derived from the same sample clock.
- Raw model score and the smoothed trigger score, both with a documented range. A sigmoid score is not a calibrated probability of user intent.
- Optional estimated wake start/end with uncertainty, only if an estimator has been validated. Do not fill these fields with the decision timestamp and call that alignment.

A user may start “включи” before the threshold is crossed. The ring must retain those samples. Start conservative audio replay before the decision, and measure retained command onset against manual annotations in 5.7. **A ring proves recoverability; it does not solve wake removal.** Replaying the name into STT and trying the rejected exact-prefix spelling again is not a solution. The future integration milestone must establish a command-start/boundary handoff that both preserves the first command phoneme and handles the wake segment. Do not change the existing 300 ms VAD padding to cover up this unresolved boundary issue.

Proposed event logic for evaluation: a short score average, one event per activation episode, minimum 1-second cooldown, then rearm only after the score stays below a lower threshold for 500 ms. Keep updating features and convolution state during cooldown. Test deliberate repeated wakes two seconds apart to ensure suppression does not make the device unresponsive. Report raw threshold episodes as well as emitted events, so cooldown cannot conceal an unstable model.

Reset all frontend, network and smoothing state on capture discontinuity, model change or explicit stream reset. Do not reset after every chunk or silently retain state across an ALSA overrun. OHF's current reference recreates its interpreter to reset model state; a future native wrapper must verify its chosen reset mechanism and suppress detections until context is valid. Keep the global capture clock independent of model reset. Check runtime statuses and expose failures without emitting a stale detection. [Current reset/stride handling](https://github.com/OHF-Voice/pymicro-wakeword/blob/503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1/pymicro_wakeword/microwakeword.py).

Wake-only is a positive event with no required command suffix. A beep/listening window and command-state machine remain later integration work.

## 9. Acceptance metrics

These are **proposed targets**, evaluated on real untouched audio at the locked operating point. Report numerator/denominator and uncertainty, not just percentages.

| Metric | Development feasibility target | Stricter product target |
|---|---|---|
| Timely wake recall | ≥95% overall; ≥90% separately for wake-only, no-pause and each 0.3/1/2 m distance | ≥98% overall and for each interaction mode; ≥95% in each supported distance, microphone and adequately sampled demographic group |
| False activations without the target sound | ≤0.5/hour, with hours and confidence bound | ≤0.01/hour, supported by sufficient independent continuous holdout exposure |
| Household unwanted activations | Report all unintended emitted events, including exact-name mentions/playback, and their exposure | Same ≤0.01/hour desired for representative use; do not claim this is solved merely by meeting acoustic-negative FAR |
| Detection delay from annotated wake completion | p50 ≤150 ms; p95 ≤300 ms | p50 ≤100 ms; p95 ≤200 ms |
| Duplicate events | <1% of positive episodes | <0.1%, with enough episodes to substantiate it |
| CPU, native wake process including frontend | Average ≤5% of one core | Average ≤2%; 1-second-bin p95 ≤5% of one core |
| Standalone peak RSS | ≤50 MiB | ≤32 MiB goal, including runtime/frontend/ring; report actual whole-application incremental memory later |
| Silence / sustained conversation | No STT/RKLLM calls; no dropped audio or unbounded growth; zero silence triggers in the run | Same, plus long soak tests on final hardware/signal path |
| Immediate-command PCM retention | Every evaluated timely detection preserves the annotated first command phoneme in the replayable ring | Same; future handoff must also demonstrate correct command boundary handling |

Match at most one detection to each real wake. Report recognition within a fixed window from annotated completion −50 ms through +500 ms, and separately the fraction meeting the stricter p95 latency deadline. Earlier detections are premature and may indicate partial-word recognition; late/missing detections are failures, not discarded latency samples. Report latency distributions for matched events together with miss/late counts. Measure both audio-domain detection time and real-time wall-clock event delivery, including capture queuing and compute.

For negative audio, report emitted events/hour, raw threshold episodes/hour, exposure by category, and inhibited/cooldown time. Keep continuous speech and quiet-time rates separate. For scripted confusables and quoted names report triggered episodes / total episodes as well as any representative continuous-use rate.

With zero false activations over T hours, the one-sided 95% Poisson upper rate is `−ln(0.05)/T`, approximately `3/T`. Therefore 10 hours with zero events only supports an upper bound around **0.30/hour**; it cannot establish 0.01/hour. Approximately **300 independent hours with zero events** support that stricter upper bound under the Poisson assumption. Repeated identical audio and correlated room conditions weaken the generalization. For recall use binomial intervals plus speaker-grouped reporting; four held-out pilot speakers are feasibility evidence, not broad population validation.

On RK3588S record total user+system CPU seconds / elapsed seconds ×100, where **100% means one fully occupied core**, not all eight cores. Record thread count, affinity if any, governor/frequencies, temperature, board load, peak RSS/PSS where available, overruns and model/runtime hashes. Compare silence and speech with the same configuration. Do not convert desktop inference timing into an RK3588S measurement.

## 10. Exact proposed Milestone 5.7 experiment

This is the next milestone plan, **not execution authorization in 5.6**. Keep all future tools under a standalone experiment directory and outside production targets.

1. **Freeze the experiment contract and manifests.** Use the 20-speaker / 1,200-positive matrix and 20/10/10-hour negative split above. Keep the existing speaker/material on DEV only. Define intended pronunciations, separate mention/playback challenges, annotate wake completion and command onset, and audit source rights. If recordings are missing, stop for recording instructions rather than substitute TTS or relabel old DEV as holdout.

2. **Set up the RTX laptop in isolation.** Pin the trainer to `4665173cd35f1cff9a61e06fc427f124766c488e`, Python 3.11, TensorFlow 2.21.0 and `pymicro-features==2.0.2`; lock the rest of the resolved dependency set, including NumPy 2.x and LiteRT, with hashes. Verify GPU detection, driver/runtime compatibility, peak VRAM and a tiny export/inference round trip. Current trainer requirements are broad; this exact environment has not been installed or proven here. Preserve the lock only after the smoke test passes.

3. **Build features from TRAIN/DEV manifests only.** Use 16 kHz input, 30 ms / 10 ms / 40-feature frontend. Cache bounded features on disk. Apply the declared augmentation and crop policy. Use matched positive/negative microphones and positive examples ending at wake completion or shortly after, including real no-pause speech. No automatic dataset download or public mixed-license feature bundle.

4. **Train one fixed architecture, two seeds.** Use the upstream notebook's MixedNet shape: initial 32 filters, kernel 5, stride 3; four pointwise blocks of 64 filters; repeats `1,1,1,1`; mixed kernels `[5], [7,11], [9,15], [23]`; residuals disabled; `spatial_attention=0`, `pooled=0`, `max_pool=0`. Start `clip_duration_ms=1500`, feature hop 10 ms, batch 32 (16 if VRAM requires it). Set Adam learning rates 0.001 for 10,000 steps then 0.0003 for 10,000; seeds 17 and 29. Use 1:3 positive:negative sampling; initially equal class penalty weights, with half of negative presentations speech/confusables. Save DEV checkpoints every 1,000 steps. This is a declared pilot schedule, not a claimed optimal recipe.

   Check the generated input shape rather than treating `clip_duration_ms` as literal context. At the inspected revision, this configuration computes 50 final-layer slices + 154 dropped slices = **204 input feature frames**, corresponding to about **2.06 seconds of input PCM**. That follows the trainer's actual formula; the shape and streaming receptive history must be verified at export. The warm-up history is not a demand for two seconds of silence after a wake. Preserve slow examples and report failures when the complete word exceeds usable context. [Configuration calculation](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/microwakeword/model_train_eval.py), [architecture context calculation](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/microwakeword/mixednet.py).

   Measure the first 500 steps to estimate completion time. Do not launch a broad architecture/hyperparameter search. An optional cleared-TTS arm may repeat these two seeds with the same step budget; if no voice is cleared, omit it and finish the real-data pilot.

5. **Export and establish numerical/streaming parity on DEV.** Export float and quantized streaming TFLite; use only TRAIN examples for representative INT8 calibration (the upstream conversion uses 500 examples). Inspect input/output shapes, stride, quantization scale/zero point, tensor types and state. Do not assume input is unsigned: the trainer requests INT8 input and UINT8 output. Feed identical PCM with multiple chunk sizes, verify frontend/state/reset parity, and report quantization recall/latency changes. Reject export if it needs full TensorFlow/Flex at inference or cannot preserve streaming behavior. [Conversion implementation](https://github.com/OHF-Voice/micro-wake-word/blob/4665173cd35f1cff9a61e06fc427f124766c488e/microwakeword/utils.py).

6. **Run a standalone Linux/aarch64 evaluator.** Reference `OHF-Voice/pymicro-wakeword` **v2.4.1**, commit `503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1`, and `rhasspy/pymicro-features` 2.0.2 (inspected HEAD `96bd69cfad79aa67697e176570d3dd87052c3def`). The Linux reference has fixes for stride and INT8 handling; use those semantics. Its bundled C-runtime source link describes TensorFlow **v2.17.1**, so compatibility with a 2.21 exporter must be tested, not assumed. Record the actual `TfLiteVersion()`, library hash and notices; if a native runtime build is needed, build a pinned compatible TFLite C library in isolation and repeat DEV parity before locking. Use C++17 plus the same C frontend for trustworthy native resource measurements; Python may serve as the numerical reference, with its RSS reported separately. [Reference changelog](https://github.com/OHF-Voice/pymicro-wakeword/blob/503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1/CHANGELOG.md), [runtime source pointer](https://github.com/OHF-Voice/pymicro-wakeword/blob/503f1b1e4cf9eac047ca0117c16f5bac85fa8cb1/lib/SOURCE), [runtime build recipe](https://github.com/tphakala/tflite_c), [TFLite native API](https://developers.google.com/edge/api/tflite/cc).

7. **Select the operating point on DEV only.** Candidate thresholds: 0.50, 0.60, 0.70, 0.80, 0.90, 0.95, 0.98; score averaging windows of 3 or 5 inference steps. At a 30 ms stride these are 90/150 ms windows. Fixed cooldown 1 s; rearm below half the chosen threshold for 500 ms. Select the model/checkpoint and operating point with highest timely recall satisfying DEV false-activation and latency limits; tie-break on lower false activations, then lower latency. Replay DEV continuously to score actual events. The trainer's overlapping-window ambient statistic is a selection aid, not a substitute for event/hour measurement. If no candidate meets the joint targets, report failure at this budget without viewing holdout to select a compromise.

8. **Lock, then evaluate HOLDOUT once.** Save the selected model, weights, environment lock, frontend configuration, threshold, smoothing, rearm rules and split hashes. A separate evaluator reads the 240 real positives, 120 scripted negatives and 10 continuous negative hours. Include pause, mention/playback, unseen-suffix and distance cohorts separately. No threshold changes after this point. Report every sample's IDs, expected target/interaction label, event count, scores, timestamps, latency, distance/device/mode and command-onset retention. No transcript, action or RKLLM output is required.

9. **Measure runtime under sustained input.** On RK3588S run at real-time pacing: at least 30 minutes silence/room tone, 30 minutes conversation, a mixed positive/negative session, and a two-hour stability run. Process the full held-out negative corpus to compute event rates without padding it with repeated silence. Record CPU, RSS, temperature, backlog, overruns and duplicates; use a streaming reader that does not preload hours of WAV into RAM. Full STT and RKLLM call counts remain zero. Do not launch the Milestone 4 assistant as a benchmark harness. A future integrated comparison can account for resident baseline memory without altering Milestone 4 here.

10. **Stop with a feasibility report and artifacts.** Deliver split/provenance manifests, configurations, environment lock, seed results, model sizes/hashes, float/INT8 parity, per-event results and native resource measurements. Classify the outcome as suitable for a later integration experiment, needing more data/validation, or unsuitable at this training budget. A pilot pass does not satisfy the final microphone or 300-hour product acceptance requirement. No production wake integration, command state, CommandEngine/RKLLM invocation or production code changes belong in 5.7's standalone training evaluation.

## 11. Work performed in 5.6

Inspected repository state and prior DEV findings; checked upstream source, release metadata, license files, model-file size metadata, training/export code and Linux inference behavior. Research downloaded only small text/metadata files into ignored `build/wake-design-56/research/`; no model weights, large datasets or training dependencies were downloaded or installed. This report is the only new repository document for this milestone. No training, inference benchmark, production build/test execution or integration was performed.
