# Milestone 6.2 — offline configuration foundation

Implemented on 2026-09-15 against HEAD `881e21ca9fc3d3343301849db7ebd852caef05e6`, following the [Milestone 6.1 architecture](milestone-6.1-dynamic-configuration-design.md). This is an offline library, inspection tool and configuration store. Existing production consumers do not load or link it. No production parser, SafetyGate, CommandEngine, prompt/model, compact protocol, STT/VAD or live voice behavior changed.

## Implementation and files

| Files | Responsibility |
|---|---|
| [config/current-m4.json](../config/current-m4.json) | Exact structured sample from the authoritative report: four rooms, 23 room mention forms, five location phrases, four logical lights and one roomless entrance-door target; no physical bindings |
| [config/schema-v1.json](../config/schema-v1.json) | Closed JSON Schema Draft 2020-12, storage version **1**; required fields and bounded collections; only the two existing capabilities |
| [Configuration.h](../include/firefly/config/Configuration.h) | Typed domain objects, immutable validated catalog, effective capability metadata and structured errors |
| [ConfigLoader.h](../include/firefly/config/ConfigLoader.h), [ConfigValidator.h](../include/firefly/config/ConfigValidator.h) | Public load/validation interfaces; return a complete `shared_ptr<const ValidatedCatalog>` or throw `ConfigError` |
| [ConfigStore.h](../include/firefly/config/ConfigStore.h) | Offline initialize/save/current/revisions/restore/reconcile interfaces and explicit test fault seams |
| [ConfigLoader.cpp](../src/config/ConfigLoader.cpp), [ConfigSchema.h.in](../src/config/ConfigSchema.h.in), [JsonInternal.h](../src/config/JsonInternal.h) | Strict parser callbacks, embedded local schema checks and typed serialization; JSON library types remain private |
| [ConfigValidator.cpp](../src/config/ConfigValidator.cpp) | Semantic validation, indexes, legacy compatibility comparison, canonical JSON and deterministic summary |
| [ConfigStore.cpp](../src/config/ConfigStore.cpp), [Platform.h](../src/config/Platform.h), [Platform.cpp](../src/config/Platform.cpp) | RAII file descriptors, bounded reads, safe local store transactions and kernel SHA-256 |
| [ConfigMain.cpp](../src/config/ConfigMain.cpp) | `firefly-config` offline CLI, structured diagnostics and nonzero failure status |
| [root CMakeLists.txt](../CMakeLists.txt), [configuration CMakeLists.txt](../src/config/CMakeLists.txt) | Root change is only the appended default-OFF `FIREFLY_BUILD_CONFIG_TOOLS` option and guarded subdirectory; new targets and dependency checks are isolated below it |
| [ConfigTests.cpp](../tests/config/ConfigTests.cpp) | Validation, snapshot and store tests, including interrupted-process and concurrent-writer tests |
| [ConfigShadowTests.cpp](../tests/config/ConfigShadowTests.cpp) | Offline executable comparing the sample with existing enum/codec/parser/gate behavior |
| [ConfigCliTests.py](../tests/config/ConfigCliTests.py), [current-summary.json](../tests/config/current-summary.json) | CLI tests, exact source/report comparison and an independent deterministic summary oracle |
| [third_party/nlohmann](../third_party/nlohmann/README.md) | Exact vendored header, upstream MIT license, source/hash manifest and attribution |
| This report | Build/use instructions, scope, validation evidence and next-step boundary |

## Strict validation and immutable snapshot

Only `schema_version: 1`, `ru-RU`, `legacy_m4_v1`, `light.set_brightness.v1` and `entrance_door.open.v1` are recognized. The only parameter contract is required integer brightness in percent, within intrinsic 0..100. Typed enums have explicit unknown defaults that cannot validate. Door opening has no parameters and its legacy target has no invented room association. Unsupported definitions are rejected even when disabled.

The loader bounds input at 256 KiB and 16 container levels. It rejects malformed/trailing JSON, duplicate member names (including escaped-equivalent keys), invalid UTF-8, BOM, NUL/control/selected format characters, chat control sequences and decimal/exponent/overflow numeric tokens. It uses the bundled schema for closed object structure, required fields, primitive types, constants, collection limits and numeric bounds. Its schema evaluator deliberately supports only the vocabulary used in this bundled file, not arbitrary third-party schemas. There is no remote reference resolution.

Semantic validation rejects duplicate or retired IDs, dangling room/device/action references, wrong operation/device combinations, duplicate target/action references, ambiguous operation definitions, malformed restrictions, unknown safety fields, invalid ranges, and nonempty bindings or device/action alias extensions. It checks display/alias UTF-8 byte limits in addition to the schema's character bounds. Revision is a positive integer capped at `9007199254740991`; retired IDs are capped at 4096, with total document limits still applying. There is no automatic schema migration or version downgrade.

Room mention and location indexes are separate. Alias collision checks use pinned Russian/ASCII case folding and whitespace normalization, retain both source paths, reject noncanonical spellings/spacing, protect reserved grammar/security words and detect locations containing another room's mention. Explicit location/mention overlap for the same room is allowed. Display names are not automatically aliases. No transcript correction, morphology, fuzzy matching or dashboard-generated regexes were introduced.

The catalog privately owns copied typed data; callers receive only const access. It contains sorted ID indexes, room mention/location indexes, target/action enablement, deny restrictions, effective brightness bounds, mandatory policy identifiers, legacy mapping and a canonical content hash. Mutating the input object after validation cannot change an existing snapshot. The mandatory policy identifiers describe the existing capability contract; this library does **not** evaluate commands or replace the production gate.

`storage_valid` and `matches_current_legacy_domain` are separate. Valid proposed data with tighter bounds, disabled/denied actions or extra supported-kind objects can be stored offline but is explicitly reported as different from the frozen installation. Legacy code reassignment, target identity changes and invalid capability definitions are rejected. Every successful CLI result says `production_activated: false`; `activation_checks` is `not_implemented`. A matching domain is not a claim that a new prompt or dynamic runtime has been evaluated.

Canonical JSON sorts object keys, domain arrays by stable ID and unordered alias/reference lists, retains UTF-8 display text, uses four-space indentation and ends with one newline. SHA-256 covers those exact bytes. The sample's canonical hash is:

`6f7d116d7cdb0f95bce447f4b98d0de104cf08056aa0694e17aaa6b12efbdff2`

## Offline store

The store requires an explicit existing directory owned by the caller and not writable by group/others. It creates no production directory or service. File traversal rejects symlink components and `..`; committed entries must be owned regular files without hard links. Reads use the same checked file descriptor. New files use mode 0600, exclusive random temporary names and same-directory publication.

An advisory lock serializes writers. Save requires the exact expected revision and canonical hash, validates the full candidate, assigns a new revision, and preserves the previous canonical file in a hash-named backup. It writes/fsyncs the new file, atomically renames it and fsyncs the directory before acknowledging success. It never changes a runtime snapshot.

A durable transaction marker records the old/new identities before publication. An interrupted transaction blocks ordinary store operations until explicit `reconcile`. Reconciliation validates only the committed path against those identities; it never promotes temporary files or silently restores a backup. A post-rename directory-flush failure is reported as indeterminate durability. The `reconcile` CLI command makes the architecture's required recovery step concrete; it is not runtime activation.

Restore is explicit and creates a new revision. Deleted IDs remain in `retired_ids`; saves/restores cannot revive them, change an existing object's category or erase retirement history. Backups that would revive deleted IDs fail with a conflict. The initial tool retained all history. Current [M18.5 policy](m18-config-state-time-hardening.md) retains the newest 64 historical backups, excluding current and dedicated Last Known Good. Pruning happens under the store lock before the next transaction; legacy backup-only LKG is materialized durably first. Pruned revisions return `unknown_revision`; retirement history remains in the current configuration. Schema 1 has no migration to perform; unknown versions fail.

Tests inject write/fsync/publication failures, terminate child processes during partial writes and after rename, and race two writers against the same expected revision. These verify process-interruption recovery, not a physical power-cut guarantee. Filesystem/storage durability still depends on honoring successful flushes.

## Dependency

**nlohmann/json v3.12.0** is vendored in `third_party/nlohmann/json.hpp`; it is not installed as a system package. The unmodified upstream [MIT license](../third_party/nlohmann/LICENSE.MIT) and Niels Lohmann attribution are included. [provenance.json](../third_party/nlohmann/provenance.json) records exact upstream URLs, byte sizes and hashes.

Header SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.

License SHA-256: `46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603`.

CMake verifies both hashes. There are no automatic downloads, mutable system-parser dependencies or runtime network calls. The parser uses upstream callbacks to detect duplicate keys before map insertion. The Linux-only platform adapter uses the board's existing AF_ALG SHA-256 interface, verified against standard empty/`abc` vectors and Python `hashlib` over the canonical sample; no additional hashing library was downloaded.

Only configuration sources/tests receive the vendored include path. Compile-command and binary-symbol inspection verified no configuration/nlohmann symbols in `firefly-assistant` and no CommandEngine/RKLLM/voice consumers in the configuration library. The offline shadow test alone deliberately links the two domains for comparison.

## Build and CLI usage

Core-only offline build:

```sh
cmake -S . -B build/config-62-clean-core -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build/config-62-clean-core -j2
ctest --test-dir build/config-62-clean-core --output-on-failure
build/config-62-clean-core/firefly-config validate --file config/current-m4.json
build/config-62-clean-core/firefly-config inspect --file config/current-m4.json
```

`validate` and `inspect` both emit the deterministic normalized summary. Success is exit 0, configuration/I/O/conflict failure is exit 1, and CLI usage/internal failure is exit 2. Failure emits a structured diagnostic to stderr and no successful catalog on stdout.

To try persistence, create a development scratch directory and initialize it explicitly:

```sh
config_store_dir=$(mktemp -d)
build/config-62-clean-core/firefly-config init --store "$config_store_dir" --file config/current-m4.json
build/config-62-clean-core/firefly-config current --store "$config_store_dir"
build/config-62-clean-core/firefly-config revisions --store "$config_store_dir"
```

Further command forms (supply the actual revision/hash returned by `current`):

```text
firefly-config save --store DIR --file CANDIDATE.json --expected-revision N --expected-hash HASH
firefly-config restore --store DIR --revision BACKUP_N --expected-revision CURRENT_N --expected-hash HASH
firefly-config reconcile --store DIR
```

Do not edit `configuration.json` directly. A candidate file is a separate input to a full validated save. No command starts inference, microphone capture, HTTP, reload or hardware execution.

## Verification

Both build directories were confirmed absent before initial configuration. The final full build compiled from an empty directory with voice, RKLLM, existing tests and new configuration targets enabled:

```sh
cmake -S . -B build/config-62-clean-full -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 -DFIREFLY_BUILD_VOICE=ON -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build/config-62-clean-full -j2
ctest --test-dir build/config-62-clean-full --output-on-failure
```

| Check | Result |
|---|---|
| Final core-only build / CTest | Passed; 11/11 suites |
| Clean full voice/RKLLM-enabled build / CTest | Passed; 13/13 suites; no compiler warnings in the final build logs |
| Existing production unit suites | All eight passed unchanged: Action, ActionValidator, DeterministicParser, SafetyGate, CommandEngine, RkllmEngineFailures, CliRobustness, VoicePipeline |
| ConfigValidation | 98 checks passed |
| ConfigSnapshot | 13 checks passed |
| ConfigStore | 56 checks passed |
| ConfigShadow | 1389 checks passed; all 404 light values and the one entrance target agree with existing behavior |
| ConfigCli | Six Python test methods passed, including exact source-domain and report-sample comparisons |

The new C++ suites contain 1556 checks; the six Python methods contain additional assertions. These are five new CTest suites, not 1556 independently registered CTest cases. The core-only test run took 1.77 seconds and the full run 2.09 seconds on this board.

The source-shadow check verifies exact room enum membership, all 23 SafetyGate room forms and all five parser locations against the sample, not just a subset of examples. Compiled shadow checks exercise the old codec/parser/gate over the complete current numeric domain, reject wrong-room evidence, retain door fallback routing and preserve `открытвер на входе` → `U`. The sample also equals the JSON block in the authoritative design, including empty bindings/restrictions and stable identities. Display renaming and object reordering have separate identity/normalization tests.

Model-backed verification uses the unchanged runners and frozen references, separate from configuration tests:

```sh
build/config-62-clean-full/firefly-regression --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt --data tests/data/dev.tsv
build/config-62-clean-full/speech_replay_tests build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
```

The clean full-build NPU runner passed **48/48** frozen DEV cases: 24 valid actions and 24 rejections. False accepts, false rejects, wrong actions, invalid model/final outputs and runtime/input errors were all zero. Routes remained **17 fast path / 31 LLM fallback / 0 input rejected**. Mean command latency was 451.563 ms overall, 1.34637 ms on the fast path and 698.456 ms on fallback, excluding initialization/bootstrap.

The unchanged native speech replay passed **eight streamed speech WAVs and two silence WAVs**, with one persistent STT instance and exactly eight STT calls. No silence case invoked STT. No fresh live microphone session is claimed; the recorded Milestone 4 human-live baseline remains unchanged.

Final logs remain in ignored `build/config-62-validation/`: `core-build-final.log`, `full-build.log`, `core-ctest.log`, `full-ctest.log`, `config-inspect.json`, `text-dev-final.stdout.log`, `text-dev-final.stderr.log`, `speech-replay-final.stdout.log` and `speech-replay-final.stderr.log`. Model/runtime/audio files are not added to Git.

Checkpoint review confirmed exactly **one modified tracked file** (`CMakeLists.txt`, six appended lines) and **24 new intended files** listed above, with no unrelated changes. Existing tracked production source, interfaces, tests, DEV fixtures, architecture/baseline documents and build definitions before the appended option remain byte-for-byte unchanged. The production prompt SHA-256 remains `d1c977f28ec4f8e08e30ee362d108fed1482a690667d6af88776ce7c8442d02f`. Authored-file whitespace/link checks and dependency provenance checks passed; upstream header/license bytes were preserved verbatim.

The validation base is `881e21ca9fc3d3343301849db7ebd852caef05e6`. Milestone 6.2 was accepted and reviewed for its complete implementation checkpoint on `master`. Production behavior remains unchanged; the new code has no production consumer wiring or activation path.

## Proposed Milestone 6.3 boundary

Implement a **separate offline generic-action/legacy-codec adapter**, using this immutable catalog: typed target/action IDs and integer arguments, explicit rejection as no action, and exact `L|K/B/G/C|N` / `D` compatibility. Cover all 405 canonical actions, leading-zero decoding, invalid syntax, stale snapshot identities and unrepresentable targets with independent tests. Keep old `Action`, parser, CommandEngine, SafetyGate, model prompt and voice output authoritative. Do not wire any production consumer or create a new LLM wire format in that increment. Milestone 6.3 has not started.
