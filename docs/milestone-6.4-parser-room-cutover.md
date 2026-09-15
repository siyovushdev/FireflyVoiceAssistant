# Milestone 6.4 — deterministic parser room lookup cutover

Date: 2026-09-15. Base HEAD: `fb8e2ee394602e5808e0cd2e6386ad18d77c0e64`.
**Status: accepted by explicit user decision for the room/location production
cutover only.** Deterministic/config-backed verification passed. Controlled
investigation reproduced the `contradiction02` false accept on the clean baseline
and located divergence in raw RKLLM output. The separate pre-existing variability
and deterministic safety gap remains open and must be addressed before further
consumer migration. See the [tracked investigation](milestone-6.4-rkllm-contradiction-investigation.md).

Before editing, verified that exact commit and a clean `master` working tree,
read the [6.1 design](milestone-6.1-dynamic-configuration-design.md),
[6.2 foundation](milestone-6.2-configuration-foundation.md) and
[6.3 runtime report](milestone-6.3-runtime-configuration-shadow.md), and inspected
the actual parser, CommandEngine, DomainView and text/voice assembly.

## Exact production responsibility migrated

Only the deterministic parser's **location phrase → room/code mapping** now comes
from configuration. The old parser table had five entries for four rooms:
`на кухне`, `в спальне`, `в гостиной`, `в зале`, `в коридоре`. These map to the
existing `K`, `B`, `G`, `G`, `C` compact light targets.

The parser did not previously accept all 23 SafetyGate room-mention inflections
as location phrases. That distinction is preserved: it reads only
`rooms[].speech.locations`, not `speech.mentions`, display names or IDs as spoken
commands. Living-room and hall locations still identify the same configured
canonical room. No new room, alias, utterance or product capability was added to
`config/current-m4.json`; the sample is unchanged.

## Architecture before and after

Before 6.4:

```text
RuntimeConfig -> immutable Catalog -> DomainView -> startup shadow diagnostics
CommandEngine -> static DeterministicParser -> hardcoded location/code table
```

After 6.4, in both text and voice production assembly:

```text
RuntimeConfig -> immutable Catalog -> DomainView : ParserRoomDomain
  -> explicitly constructed DeterministicParser -> immutable compiled location rules
  -> startup room-mapping equivalence check (failure prevents model initialization)
  -> CommandEngine(llm, parser)
       -> parseChecked(original)
            configured rules -> NEW candidate (production authority)
            legacy rules     -> comparison candidate only
            disagreement     -> throw -> U + diagnostic, no LLM call
       -> unchanged fallback / SafetyGate / ActionValidator / compact Action
```

The new `ParserRoomDomain` interface contains only `parserLocations()`, returning
location phrases and their existing compact room codes. It contains no JSON,
file paths, persistence, brightness limits, enablement, safety rules or command
grammar. This small interface keeps the parser/core independent of the
configuration foundation and runtime assembly library.

`DomainView` implements it by walking the validated location index, joining each
canonical room ID to the configured legacy light target, and returning its code.
There is no new hardcoded installation table in the view. A location without a
unique representable target throws; it cannot be assigned a default room.
Device/action enablement and brightness bounds are deliberately not consulted
by this interface because they have not been migrated.

The parser constructor requires an explicit domain and compiles its owned copy of
the location rules once. Its rules are held as `shared_ptr<const Rules>` behind
a const member; there is no default constructor, assignment, reload or setter.
The application retains both the runtime owner and parser through model/engine
shutdown, including completion of the voice worker. No persistence is performed
by the parser and no configuration is read on the request path.

CommandEngine's production constructor explicitly receives that parser. An
immutable candidate-provider callable captures its const reference. The processing
body changes only from the static parser call to that provider; input admission,
routes on ordinary unresolved input, LLM whitespace normalization, original-input
safety checks, validation, error handling and output construction are unchanged.

## Grammar and decisions deliberately retained

The existing rule compiler and matcher were factored into shared functions so the
same grammar can use either configured locations or the old oracle's locations.
The whole-input productions, numeric regular expressions, capture groups, value
conversion, on/off/half/maximum meanings, outside trimming, Russian/ASCII case
folding and inner whitespace behavior are unchanged. Configuration phrases are
escaped as regex literals; they cannot introduce regex syntax. Duplicate or empty
location entries supplied through the minimal interface are rejected.

This increment preserves the existing per-location compiled rules for the five
current phrases. It does not implement the future generic tokenizer/grammar from
6.1 or add a grammar-editing facility. Factoring the existing rule construction
is solely to vary the room data without cloning or redesigning the parser.

Still hardcoded and authoritative:

- command grammar, intent detection, numbers, percentages, brightness 0..100 and
  on=100/off=0/half=50/maximum=100;
- SafetyGate's 23 room mentions, original-input requirements, light-zero checks
  and all entrance-door policy;
- ActionValidator, Action, compact protocol and room enum;
- LLM fallback, frozen Qwen3 prompt/context and RKLLM model/settings;
- STT, VAD, voice processing/state machine and output behavior.

The new tests intentionally tighten configured brightness and disable devices
while leaving locations intact: the parser still returns `L|K|100`. This proves
that these other configuration fields were not accidentally activated.

## Legacy oracle and fail-closed behavior

The original five-entry table remains in `src/DeterministicParser.cpp`, marked
`LEGACY SHADOW ONLY`. It has not been copied into another runtime location. The
existing static `DeterministicParser::parse(original)` uses it as the frozen
reference path. The new configured path never invokes that table to produce its
candidate or to supply a missing location.

There are two migration checks:

1. **Startup admission:** `requireLegacyRoomDomain()` compares the complete
   configured phrase/code mapping with the original table, independently of array
   ordering. Missing, added or reassigned parser locations cause
   `RoomLookupMismatch` before prompt loading, RKLLM, VAD/STT or capture. The error
   is explicit; there is no configuration or legacy-answer fallback. This check
   is scoped to the migrated location mapping. The broader 6.3 domain comparison
   remains observational for non-migrated fields.
2. **Per-command guard:** `parseChecked(original)` obtains the new configured
   candidate, compares it with the legacy candidate, and returns **the new
   candidate** only when they agree. Any difference, including `nullopt` versus
   an old match, throws `RoomLookupMismatch`. CommandEngine's existing exception
   handling resets the action and reports `U` with an error; fallback inference is
   not reached. The old answer is never returned to repair the new one.

If both parsers return unresolved, ordinary LLM fallback is unchanged. Unknown
rooms and unsupported grammar do not acquire defaults. Final decisions remain
subject to the unchanged SafetyGate and validator. A mismatch is distinct from
an ordinary unresolved command and cannot be retried through the LLM.

The production startup diagnostic is
`PARSER_ROOMS authoritative=config legacy_shadow=matched`. It is emitted only
after successful admission. The existing `CONFIG_SHADOW` diagnostic now explicitly
labels its `scope=legacy_domain_baseline`; its 1,364 synthetic baseline probes
remain separate from the new location admission and request checks.

The old static parser API and one-argument `CommandEngine(llm)` constructor remain
documented migration-reference entry points for unchanged historical tests and
the frozen `firefly-regression` harness. They are not optional fallback branches
of the injected path. Both actual assistant entry points use the two-argument
constructor. New tests explicitly construct the validated view/parser dependency;
the existing test files and expected results were not rewritten. Keeping the
reference harness enables comparison with the separately tested integrated
assistant.

`parseConfigured()` is the pure configured-candidate API used by differential
tests. It does not authorize execution or disable the application's guards.
Production CommandEngine always invokes `parseChecked()`.

## Proof of configuration authority

An isolated test copies the validated current fixture and moves `в зале` plus
its five hall mention forms from `room_living` to `room_bedroom`. The ordinary
6.2 validator accepts this document as storage-valid while reporting legacy
differences. No parser source, legacy table or production sample is modified.

For **all 101 brightness values**, the configured parser then returns `L|B|N`
while the legacy oracle returns `L|G|N`. The startup and per-request guards throw
on that difference. An injected CommandEngine produces `U` with a diagnostic and
zero LLM calls, even when the fake LLM is primed with the old answer.

Another storage-valid fixture removes `в зале` while retaining `в гостиной`.
The configured parser returns no candidate for the removed alias; the unchanged
living-room location still works. Again, the legacy answer and LLM are never
used to fill the miss. A third fixture adds `в кухне`: the isolated configured
parser observes the addition, while the migration guard rejects it. These tests
demonstrate data-driven lookup without enabling new production utterances.

## Files changed and added

Modified:

| File | Change |
|---|---|
| `include/firefly/DeterministicParser.h` | Required domain constructor, immutable rule ownership, configured/checked APIs, startup guard and typed mismatch |
| `src/DeterministicParser.cpp` | Shared frozen rule compiler/matcher; configured location data; original table retained as oracle; two comparison guards |
| `include/firefly/CommandEngine.h`, `src/CommandEngine.cpp` | Explicit production parser injection; unchanged processing body except candidate-provider call; retained reference constructor |
| `include/firefly/runtime/DomainView.h`, `src/runtime/DomainView.cpp` | Minimal location-domain interface implementation from snapshot indexes/references |
| `src/main.cpp`, `src/VoiceMode.cpp` | Construct/admit parser before models; inject it into CommandEngine; concise authority diagnostic |
| `src/runtime/RuntimeConfig.cpp` | Scope label on the existing baseline diagnostic |
| `src/runtime/CMakeLists.txt` | New core-independent-of-NPU and application-startup test targets |

Added:

- `include/firefly/ParserRoomDomain.h`: minimum read-only location-data contract.
- `tests/runtime/ParserRoomCutoverTests.cpp`: full grammar/domain equivalence,
  real config reassignment/removal/addition, rejection and no-repair tests.
- `tests/runtime/ParserRoomCutoverCliTests.py`: text and voice startup admission
  tests for valid drift and current configuration, without model/audio access.
- This report.
- `docs/milestone-6.4-rkllm-contradiction-investigation.md`: controlled A/B evidence,
  final attribution conclusion and the separate open safety issue.

No changes to the production sample/schema, foundation implementation, vendored
dependencies, any existing test/DEV fixture, SafetyGate, ActionValidator, Action,
RKLLM, speech/voice processing or model/prompt artifacts. No dependency downloads.

## Verification

Both clean build directories were confirmed absent before configuration:

```sh
cmake -S . -B build/config-64-clean-core \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build/config-64-clean-core -j2
ctest --test-dir build/config-64-clean-core --output-on-failure

cmake -S . -B build/config-64-clean-full \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_VOICE=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DSHERPA_ONNX_ROOT="$PWD/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu" \
  -DSHERPA_ONNX_INCLUDE_DIR="$PWD/build/stt/vad-ab" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/config-64-clean-full -j2
ctest --test-dir build/config-64-clean-full --output-on-failure
```

| Verification | Result |
|---|---|
| Clean core build | Passed, no compiler warnings; **13/13** CTest suites |
| Clean full build | Passed, no compiler warnings; **19/19** CTest suites |
| Eight existing production suites | All passed without editing any existing test source or expectations |
| Five Milestone 6.2 suites | All passed: Validation 98 checks, Snapshot 13, Store 56, ConfigShadow 1,389, ConfigCli six Python methods |
| Three Milestone 6.3 suites | All passed: RuntimeConfig 2,337 checks, RuntimeConfigCli seven Python methods, VoiceStartup four checks |
| Milestone 6.3 baseline comparison | Still **1,364 probes, zero mismatches** with the current sample |
| New ParserRoomCutover suite | **51,484 checks passed**, covering five locations, all 23 mention aliases without broadening grammar, the complete integer range and existing grammar families, symbolic values, unsupported/ambiguous inputs, normalization, valid config drift, terminal mismatch rejection and no repair |
| New text/voice startup suites | **Four cases each passed**: removal, addition and reassignment fail before models; current configuration passes room admission |

The new numeric matrix covers 5 locations × 101 brightness values × 3 numeric
suffix forms × 11 existing command productions, with independent expected compact
results, legacy comparison and injected CommandEngine execution. All existing
tests remain byte-for-byte unchanged. The broad old suites remain useful independent
oracles; production CLI tests now exercise the injected parser through unchanged
application test code.

Source audit additionally verifies that the parser's normalization and full
match/evaluation loop are byte-for-byte unchanged, and grammar-building expressions
match the checkpoint after indentation is ignored. CommandEngine's process body
differs only in the candidate-provider call. Text/voice assembly after the new
startup admission differs only in the explicit parser constructor argument.
RKLLM, voice-core and speech library archives are byte-for-byte identical to their
clean 6.3 counterparts. The core library has no configuration/runtime-library
dependency; the configuration library has no parser/core/runtime dependency.
Vendored JSON include paths remain private to configuration sources/tests.

### Initial model-backed regressions

The initial runs below used the frozen DEV file and the exact existing Qwen3 model, prompt,
sampling, KV lifecycle and runtime. No component was tuned between runs.

| Run | Exact results |
|---|---|
| Rebuilt 6.4 legacy reference harness, first run | **47/48**; one false accept on `contradiction02`; zero false rejects, wrong actions, invalid outputs or runtime/input errors |
| Pre-cutover 6.3 reference binary control | **48/48**; zero false accepts, false rejects, wrong actions or errors |
| Rebuilt 6.4 legacy reference harness, diagnostic repeat | **47/48**, same false accept on `contradiction02`; all other categories zero |
| Integrated 6.4 production assistant with configured parser | **48/48**; all compact outputs identical to DEV expectations; zero false accepts, false rejects, wrong actions or room-shadow mismatches |
| Unchanged native speech replay | All **eight speech WAVs and two silence WAVs** passed; exact transcripts unchanged, one persistent STT instance, eight STT calls, no STT calls for silence |

Every text run retained **17 fast path / 31 LLM fallback / 0 input rejected**.
The integrated assistant emitted exactly one successful config-authority parser
diagnostic before RKLLM initialization. The 6.3 baseline shadow count remained
1,364 with zero mismatches. No new live microphone session is claimed.

The failing DEV case is:

```text
contradiction02
original: На кухне выключи свет на 50 процентов
expected: U
rebuilt legacy harness raw/final: L|K|50
pre-cutover control and integrated assistant final: U
```

The new configured parser and the legacy parser both return unresolved for that
input. An offline replay with the recorded raw model response `L|K|50` confirms
that both CommandEngine paths then return `L|K|50` through the unchanged fallback,
SafetyGate and validator, without a room-lookup disagreement. This reproduces the
acceptance conditional on that raw response; it is not a correct-rejection test.
The gate's lack of a general off/nonzero contradiction rule was already documented
in 6.1 and was not changed here.

Subsequent controlled investigation, summarized below and preserved in the
[tracked investigation report](milestone-6.4-rkllm-contradiction-investigation.md),
reproduced both raw answers on the clean baseline and the unchanged WIP. It also
captured an integrated-assistant false accept, so the initial integrated pass is
not evidence of an additional safety rejection. The vendor-internal cause remains
unlocalized; the original failures remain recorded. No component was changed to
force this case to pass.

Reference harness mean command latency, excluding initialization: 477.441 ms on
the first rebuilt run, 449.242 ms on the pre-cutover control, and 467.534 ms on the
rebuilt repeat. Timing is recorded for context, not claimed equivalent.

Commands for the model-backed runs:

```sh
build/config-64-clean-full/firefly-regression \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --data tests/data/dev.tsv
# Control used the same arguments with build/config-63-clean-full/firefly-regression.
# The integrated assistant received all 48 original DEV inputs on stdin:
build/config-64-clean-full/firefly-assistant \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --config config/current-m4.json --verbose
build/config-64-clean-full/speech_replay_tests \
  build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
```

Evidence is preserved in ignored `build/config-64-validation/`: clean configure,
build and verbose CTest logs for core/full; `text-dev.stdout.log`/`.stderr.log`,
`pre-cutover-control.stdout.log`/`.stderr.log`, `text-dev-repeat.stdout.log`/`.stderr.log`,
`integrated-dev.stdout.log`/`.stderr.log`, `integrated-dev-summary.json`,
`speech-replay.stdout.log`/`.stderr.log`, and `fallback-replay.cpp`/`.log` plus its
offline diagnostic binary. None of these scratch artifacts is staged or tracked.

Checkpoint scope audit: **10 modified and 5 added files**, as listed above, with
no unrelated changes. Existing test sources, configuration/schema and
protected production components are unchanged. Author-written whitespace and
report links pass checks. The production prompt SHA-256 remains
`d1c977f28ec4f8e08e30ee362d108fed1482a690667d6af88776ce7c8442d02f`; vendored
nlohmann/json v3.12.0 and its MIT license retain their recorded checksums.

## Investigation conclusion and explicit acceptance

A. **The deterministic/config-backed room cutover passed.** Both production
entry points obtain location data through the immutable snapshot-backed domain
view, return the configured parser candidate, and use the legacy lookup only as
a shadow oracle. Mismatches fail closed; there is no legacy repair. No additional
consumer was migrated.

B. **The frozen 48/48 RKLLM criterion is not deterministic evidence for this
specific case.** Clean baseline and WIP each produced both `U` and `L|K|50` in
three identical fresh-process isolated runs. Normal-order uninstrumented suites
passed 48/48 four times per side; case-first WIP runs produced 47/48 then 48/48.
An API-traced integrated A/B pair produced raw `U` versus `L|K|50` with identical
init fields, template/input sequence and cache-clear calls. No unintended harness
link dependency was found, and fresh binaries matched the earlier binaries.
Baseline/WIP full CTest passed 16/16 and 19/19; ASan/UBSan core suites passed 6/6.
The user accepted this evidence as sufficient to accept Milestone 6.4 only.

C. **The safety contract is unchanged.** `contradiction02` still requires `U`.
The unchanged SafetyGate preserves `L|K|50` for that input, and ActionValidator
accepts it. This pre-existing RKLLM variability / deterministic-safety-gap issue
remains unresolved and must be addressed before another configuration consumer
is migrated. Accepting the cutover does not accept the false action.

D. **No tuning or acceptance-policy workaround was applied.** Model, prompt,
RKLLM settings/cache lifecycle, SafetyGate, production configuration and expected
DEV data remain unchanged. The vendor-internal cause remains unlocalized. The
checkpoint records the user's explicit, scoped acceptance decision and retains
all failure observations; it does not claim that the issue was fixed.

## Checkpoint verification

Finalization added the tracked investigation and updated this report only. SHA256
comparison against the pre-investigation archive confirmed that all 13 non-document
WIP files (production headers/sources, CMake and new tests) remain byte-for-byte
unchanged. Complete diff/status review found exactly the 15 files listed above,
with no unrelated changes or generated investigation artifacts included.

The existing clean WIP build reran the deterministic Milestone 6.4 suites:

```sh
ctest --test-dir build/contradiction02-investigation/build-B \
  -R '^ParserRoomCutover' --output-on-failure -V
```

All **3/3** passed in 7.31 seconds: **51,484** parser/domain checks, **four** text
startup cases and **four** voice startup cases. The log is retained locally at
`build/milestone-6.4-checkpoint/deterministic-cutover-tests.log`. Production room
resolution remains configuration-authoritative, the old lookup remains a shadow
comparison, and no additional consumer was migrated. No new NPU run was used as
a checkpoint condition; expected DEV data and all frozen inference/safety settings
remain unchanged.

## Stop boundary

This is one consumer cutover only. Brightness constraints, door logic, SafetyGate,
ActionValidator, generic Action, LLM context and all other hardcoded concepts have
not been migrated. No Web Admin, HTTP, live reload, hardware, wake-word work or
legacy cleanup was started. This checkpoint contains the accepted implementation
and its tracked investigation documentation. Finalization changes documentation
only; production code remains byte-for-byte identical to the investigated WIP.
The separate safety fix has not been started.
