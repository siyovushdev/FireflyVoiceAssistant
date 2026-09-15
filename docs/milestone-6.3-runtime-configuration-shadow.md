# Milestone 6.3 — startup configuration shadow integration

Date: 2026-09-15. Base HEAD: `eedf6bc9353f32f6e390aa2f108f028fa5dd5470`.
Pre-check verified that exact commit and a clean `master` working tree before editing.
This increment follows the user's requested **first runtime shadow integration**;
it supersedes the different offline-adapter next-step proposal at the end of the
[6.2 report](milestone-6.2-configuration-foundation.md). No generic action/codec
migration is included.

## Integration point and architecture

The text and voice application assembly functions now construct a `RuntimeConfig`
after checking CLI arguments and before reading the prompt or initializing any
RKLLM, speech model or capture device. The owner remains in that function's scope
until shutdown, outliving all models and the voice worker. The voice state machine
and text command loop are unchanged.

Before: application startup constructed the frozen models/engines directly;
configuration was available only to offline tools and tests.

After:

```text
selected JSON file
  -> unchanged ConfigLoader / ConfigValidator
  -> one Catalog = shared_ptr<const ValidatedCatalog>
  -> RuntimeConfig owns a const DomainView and startup comparison result
       -> read-only ID / alias / target / effective-domain lookups
       -> synthetic comparisons with frozen components -> stderr diagnostic

original text or raw STT
  -> unchanged CommandEngine -> parser / RKLLM -> SafetyGate -> ActionValidator
  -> unchanged compact stdout
```

There is no edge from the shadow result to command decisions. No user command is
sent to the new view. **The entire comparison runs once at startup**, not per
request. This demonstrates real production snapshot loading and use without
adding hooks to CommandEngine, the gate, validator, parser or speech pipeline.
Synthetic parser probes initialize its existing static regexes earlier; startup
and first-request timings can therefore differ without changing its decisions.

`firefly_runtime_config` depends on `firefly_config` and, only for comparison, on
`firefly_core`. Neither foundation nor core depends on the new runtime library.
RKLLM and speech libraries retain their original dependencies. nlohmann/json
**v3.12.0**, its MIT license, pinned checksums and private configuration include
paths are unchanged. Public runtime interfaces contain no JSON-library types.

## Snapshot selection, ownership and startup failure

Both application modes accept one optional `--config PATH`. Without it, they use
the single absolute path compiled from CMake's `FIREFLY_CONFIG_FILE`, defaulting
to this checkout's `config/current-m4.json`. This preserves existing invocation
forms and works independently of the process working directory. An explicit path
replaces that selection; it never falls back to the compiled path. Duplicate or
empty `--config` arguments are errors. Help exits without loading configuration.

The development default is a file path, not embedded JSON or a deployment/install
facility. A relocated installation must select its actual file with `--config`
or configure an absolute `FIREFLY_CONFIG_FILE` when building. No directories,
services or production store are provisioned.

`RuntimeConfig` calls the existing bounded `ConfigLoader::fromFile` exactly once.
It retains the resulting validated `Catalog`, the actual immutable snapshot API
introduced in 6.2. There is no second snapshot model, mutable configuration alias,
assignment setter, reload API or process-global configuration. `DomainView`
returns const borrowed pointers and reads the snapshot's existing indexes and
effective actions; their lifetime is documented. A null snapshot is rejected.
Replacing, corrupting or deleting the source file cannot change a running owner.
Constructing a new owner explicitly rereads the selected file.

Missing/unreadable/nonregular/symlink files and all schema/semantic validation
failures throw before the owner is available. Diagnostics identify configuration
startup failure and preserve the structured validation code/path. There is no
partial catalog, schema migration, automatic backup selection or default repair.
Text mode retains its existing fatal-error convention: exit 1 with `U` on stdout
and error text on stderr. Voice mode exits 1 with no compact stdout. Neither
reaches READY/LISTENING, model initialization or capture after a configuration
failure. The existing command components remain directly constructible without
configuration for their unchanged deterministic tests.

This increment reads a selected regular JSON file, not `ConfigStore::current`.
It does not open a store lock, reconcile transactions or mutate a saved revision.
Store management remains the explicit offline 6.2 tool's responsibility.

## First consumer and exact comparison

`DomainView` provides:

- room/device lookup by stable ID;
- exact canonical room-mention and location-phrase lookup through snapshot indexes;
- legacy light target and entrance target lookup through snapshot compatibility references;
- effective device/action metadata and permitted brightness bounds;
- bounded brightness membership and entrance capability availability.

Alias lookup accepts exact, already canonical keys. It does not tokenize requests,
add morphology, fold input text, infer a room from a display name or repair STT.
That boundary deliberately leaves request normalization in the old components.
The returned capability availability is catalog data, **not safety authorization**.
Mandatory door/room/original-input policy remains compiled and enforced only by
the frozen production gate in this milestone.

`compareLegacyDomain` performs deterministic synthetic probes:

1. Reuse the 6.2 versioned legacy-compatibility result to detect changed entity
   sets, missing/extra aliases and enablement/range drift, without creating another
   hardcoded installation table.
2. Examine all 256 room-code bytes against the existing ActionValidator and the
   snapshot's compatibility target lookup. For every recognized/configured light
   code, compare all integer values -1 through 101 with effective catalog bounds.
3. Cross-check every configured mention against every supported room using the
   unchanged SafetyGate's explicit-room evidence on synthetic light candidates.
4. Resolve every configured location phrase and compare every brightness 0..100
   against the unchanged deterministic parser using its existing `location свет N`
   form. These probes are not a replacement grammar.
5. Compare the catalog's selected roomless entrance capability with the existing
   validator's `D` action. Stable target identity is enforced by the unchanged
   foundation compatibility validation; the old action has no explicit target ID.

For the shipped current domain this is **1,364 startup probes**, with zero
mismatches. The report is deterministic: sorted snapshot traversal, fixed probe
order, stable mismatch keys, no timestamps, paths, command text or model calls.
The current domain contains four rooms, 23 mention aliases, five location phrases,
404 light values and one entrance target. The unchanged 6.2 tests additionally
compare the exact old source alias/enum tables with the sample, catching additions
that cannot be enumerated through the old components' public APIs.

One stderr line reports `CONFIG_SHADOW`, revision, canonical SHA-256, check count,
mismatch count and `authoritative=legacy`; a mismatch also reports its first key.
Full ordered mismatch details remain available in `ShadowComparison`. Test
harnesses call `requireEquivalent()` to throw on mismatch.

**A storage-valid but different catalog remains observational.** It produces a
nonzero mismatch diagnostic and is retained exactly as loaded; it cannot alter,
repair or reject production results. In particular a catalog disable/deny or
tighter range is not an activated security control here. Tests deliberately
disable the entrance target and tighten brightness, verify mismatches, and verify
the old outputs remain `D` and `L|K|100`. Invalid safety definitions still fail
schema/semantic startup validation. Production cutover and activation enforcement
are outside this milestone.

## Files

Modified existing files only:

- `CMakeLists.txt`: build the foundation/runtime bridge for the assistant; link
  the bridge only into application assembly and its existing mock CLI executable.
- `src/config/CMakeLists.txt`: keep offline CLI/tests behind their existing
  default-OFF option while allowing the foundation library to build for runtime.
- `src/main.cpp`, `src/VoiceMode.cpp`: config path selection, scoped startup owner,
  one diagnostic, and help text. Command loops/state machine remain unchanged.

Added:

- `include/firefly/runtime/{DomainView,ShadowComparison,RuntimeConfig}.h`.
- `src/runtime/{DomainView,ShadowComparison,RuntimeConfig}.cpp`,
  `RuntimeConfigDefaults.h.in`, `CMakeLists.txt`.
- `tests/runtime/RuntimeConfigTests.cpp`, `RuntimeConfigCliTests.py`,
  `RuntimeConfigVoiceStartupTests.py`.
- This report.

The sample/schema, foundation C++ sources/interfaces, all existing tests and DEV
fixtures, compact protocol, parser, gate, validator, CommandEngine, prompt/model,
RKLLM settings, STT/VAD, voice worker/state machine and hardware absence are
unchanged. No new dependency is downloaded.

## Verification

Both clean build directories below were absent before configuration. Build with
the existing, read-only RKLLM and previously provisioned sherpa dependencies:

```sh
cmake -S . -B build/config-63-clean-core \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build/config-63-clean-core -j2
ctest --test-dir build/config-63-clean-core --output-on-failure

cmake -S . -B build/config-63-clean-full \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_VOICE=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DSHERPA_ONNX_ROOT="$PWD/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu" \
  -DSHERPA_ONNX_INCLUDE_DIR="$PWD/build/stt/vad-ab" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/config-63-clean-full -j2
ctest --test-dir build/config-63-clean-full --output-on-failure
```

| Check | Result |
|---|---|
| Clean core build | Passed, no compiler warnings; **12/12** CTest suites |
| Clean full voice/RKLLM build | Passed, no compiler warnings; **16/16** CTest suites |
| All eight unchanged production suites | Action, ActionValidator, DeterministicParser, SafetyGate, CommandEngine, RkllmEngineFailures, CliRobustness, VoicePipeline: all passed |
| All unchanged 6.2 suites | Validation 98 checks; Snapshot 13; Store 56; ConfigShadow 1,389; ConfigCli six Python tests: all passed |
| New RuntimeConfig suite | **2,337 checks**, including complete room/alias/value domain, startup ownership, const-interface assertions, unsupported identifiers, file replacement/deletion, nine kinds of valid drift, deterministic diagnostics, no repair, invalid startup and missing-file rejection |
| New RuntimeConfigCli suite | **Seven Python tests**, including all 404 light outputs plus door/rejections, configured/default paths, invalid schema/safety/reference/alias cases, no fallback, duplicate flags, valid drift with unchanged outputs and no reread after READY |
| New RuntimeConfigVoiceStartup suite | **Four checks**: three config failures before any model/capture initialization; valid config precedes VAD initialization, stopped safely at a deliberately absent VAD file |
| Startup shadow comparison | **1,364 probes, zero mismatches** for the unchanged current-M4 sample |
| Production build without offline tooling | Assistant also builds with `FIREFLY_BUILD_CONFIG_TOOLS=OFF` and `BUILD_TESTING=OFF`; runtime bridge remains linked and no offline CLI target is required |
| Offline config inspection | Matches unchanged normalized golden summary exactly; 6.2 source-domain/sample checks passed |

The unchanged real NPU regression harness passed **48/48** frozen DEV cases:
24 valid actions and 24 unsupported/rejected cases. False accepts, false rejects,
wrong actions, invalid model/final output and runtime/input errors were all zero.
Routes remained **17 fast path / 31 LLM fallback / 0 input rejected**. Mean command
latency was 490.088 ms overall, 1.33817 ms fast path and 758.112 ms fallback,
excluding initialization. These are observations from this run, not a latency
equivalence claim.

The **integrated production assistant** was separately run with all 48 unchanged
DEV inputs, the same model/prompt, `--config config/current-m4.json` and `--verbose`.
All 48 stdout lines matched the original expected compact outputs exactly. It
reported the same 17/31/0 routes and zero false accepts, false rejects or wrong
actions. Exactly one startup shadow diagnostic reported zero mismatches. The
legacy regression harness itself remains configuration-free and unchanged.

The unchanged native model-backed speech replay passed **eight streamed speech
WAVs and two silence WAVs**, with all eight expected transcripts unchanged, one
persistent STT instance and exactly eight STT calls. Silence invoked no STT.
The recorded `открытвер на входе` → `U` rejection is preserved by the old and new
shadow tests. No fresh microphone/live-human accuracy measurement is claimed;
the [Milestone 4 baseline](milestone-4-baseline.md) remains the recorded baseline.

Commands used for the existing harnesses and inspection:

```sh
build/config-63-clean-full/firefly-regression \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --data tests/data/dev.tsv
build/config-63-clean-full/speech_replay_tests \
  build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
build/config-63-clean-full/firefly-config inspect --file config/current-m4.json
```

Evidence remains in ignored `build/config-63-validation/`: `core-configure.log`,
`core-build.log`, `core-ctest.log`, `full-configure.log`, `full-build.log`,
`full-ctest.log`, `runtime-without-tools.log`, `text-dev.stdout.log`,
`text-dev.stderr.log`, `integrated-dev.stdout.log`, `integrated-dev.stderr.log`,
`integrated-dev-summary.json`, `speech-replay.stdout.log`,
`speech-replay.stderr.log`, and `config-inspect.json`.

Final source/build audit:

- Everything after the inserted startup owner in each application's model
  assembly, command processing and shutdown is byte-for-byte unchanged.
- Built `firefly_core`, `firefly_rkllm`, `firefly_voice_core` and `firefly_speech`
  archives are byte-for-byte identical to their clean 6.2 counterparts. They have
  no new configuration/runtime-bridge symbols. The configuration archive has no
  parser, gate, CommandEngine, RKLLM, voice or runtime-bridge dependencies.
- Compile-command inspection confirms that only configuration sources/tests
  receive the private vendored JSON include path.
- Production prompt SHA-256 is still
  `d1c977f28ec4f8e08e30ee362d108fed1482a690667d6af88776ce7c8442d02f`.
  Current config canonical SHA-256 is still
  `6f7d116d7cdb0f95bce447f4b98d0de104cf08056aa0694e17aaa6b12efbdff2`.
- Checkpoint review confirmed only the four modified and twelve new intended
  files listed above, with no existing tests modified or unrelated changes.
  Authored whitespace checks pass. The three runtime configuration suites were
  rerun successfully, again confirming 1,364 startup probes and zero mismatches.

## Stop boundary

Milestone 6.3 was accepted and reviewed for its implementation checkpoint on
`master`, based on `eedf6bc9353f32f6e390aa2f108f028fa5dd5470`. No production cutover, next-consumer
migration, generic Action migration, generated prompt, HTTP/UI, live reload,
hardware, wake-word/KWS or training work was started.
