# Milestone 6.8 — Generic Action foundation and compact-v1 adapter

Base checkpoint: `1e622281e867f3f113ef2d6cd0c422727c470654`.
The exact HEAD and clean working tree were verified before editing. The
[6.7 audit](milestone-6.7-generic-action-readiness-audit.md), relevant 6.1–6.6
design/implementation reports, actual Action/validator/CommandEngine flow,
configuration model/schema, runtime assembly and build targets were inspected.

This milestone is **offline only**. The new libraries are opt-in and no existing
production or test target links them. Existing production continues to use its
legacy Action, configured domain bridges, deterministic SafetyGate and compact
output. There is no runtime shadow integration or production cutover here.

## Model and typed arguments

The new `firefly::actions::GenericAction` is a value-semantic candidate with:

```text
actionId: string identifying an existing configuration ActionDefinition
targetId: string identifying an existing configuration Device
arguments: ordered map<parameter ID, IntegerArgument>

IntegerArgument: exact signed int64 value
```

For example, current M4 decodes to:

```text
L|K|40 -> action_set_brightness, device_kitchen_light, {brightness: int64(40)}
D      -> action_open_entrance, device_entrance_door, {}
```

There is no Room enum, room letter, compact string, display name, separately
supplied operation selector, JSON value, snapshot owner or safety decision inside
GenericAction. The action ID resolves to the schema's registered operation;
target room and display metadata remain derived data. No parallel ID namespace
was introduced.

Construction takes owned values; accessors expose const references. Copy/move
and whole-value assignment are supported, with exact equality over both IDs and
the ordered argument map. Mutation of constructor inputs cannot change a candidate.
There is no default GenericAction or default numeric argument that could invent
a target/brightness. A candidate can still contain unknown IDs or invalid argument
sets for deterministic rejection at a boundary: the type is deliberately **not**
named or represented as a validated/executable action.

Argument design alternatives were evaluated:

| Option | Decision |
|---|---|
| `map<string,string>` | Rejected: numbers would require coercion and a second string syntax. |
| Separate brightness/door payload variant | Strongly typed, but duplicates operation selection beside the configured action ID and requires central action alternatives for extension. |
| Broad scalar variant including string/bool/double | Not needed by v1; would add unsupported types solely to reject them. |
| Ordered parameter map with an exact integer value wrapper | Chosen: matches v1's only parameter type, deterministic equality, no JSON or coercions, and a clear future place for a separately reviewed type expansion. |

`IntegerArgument` requires `std::int64_t` explicitly. Compile-time assertions
reject bool, float/double, unsigned integers, strings, character pointers and
null. This is the argument-type-mismatch proof: those values cannot enter the
map, rather than being converted to a plausible brightness. Signed extreme
values can be represented as candidates and are rejected by the compact adapter.
The map has unique parameter keys; any future named-argument wire decoder must
reject duplicate fields **before** inserting them. Compact v1 has no named-field
syntax and does not need such a decoder in this milestone.

## Unsupported and conversion errors

```text
CandidateResult = variant<Unsupported, GenericAction>
CompactDecodeResult = variant<CandidateResult, CompactV1Error>
CompactEncodeResult = variant<string, CompactV1Error>
```

`CandidateResult` defaults to Unsupported. Exact `U` decodes to that alternative,
never to an action ID or an executable placeholder. A malformed compact string
instead returns `InvalidSyntax`. Both cases contain no GenericAction.

Encoding accepts only GenericAction. Unknown action/target identities, invalid
arguments and unrepresentable target/action tuples return typed errors; they
cannot become a successful encoded `U` or substitute a default action. The error
categories are `InvalidSyntax`, `UnknownAction`, `UnknownTarget`,
`InvalidArguments` and `Unrepresentable`. Unsupported-to-U rendering is a future
presentation concern, not action serialization.

## Adapter and snapshot mapping

`CompactV1Adapter` requires an explicit `config::Catalog`, the existing
`shared_ptr<const ValidatedCatalog>` snapshot. It retains that immutable mapping
for its own lifetime, rejects null construction, and has no loader, file path,
reload, mutable global or production RuntimeConfig dependency. GenericAction
itself neither queries nor retains configuration; decoded values survive the
adapter's destruction.

```text
offline fixture / existing ConfigLoader + ConfigValidator
    -> immutable Catalog
    -> CompactV1Adapter
         compact v1 <-> GenericAction / Unsupported / typed error

firefly_generic_action: model only
firefly_compact_v1_adapter: model + config; privately reuses frozen core codec
dedicated new tests: opt-in consumers of these libraries

existing production targets: unchanged, no dependency on either new library
```

Decoding first uses the **unchanged** `ActionValidator::validate` for exact wire
syntax. The existing serializer supplies canonical code spelling. The adapter
then reads the snapshot's `compatibility` references; it does not contain a second
room/target ID table or another hand-written integer decoder.

| Compact representation | Snapshot mapping used | Current resulting identity |
|---|---|---|
| `L\|K\|N` | `lightTargets['K']`, `lightActionId`, configured parameter ID | `device_kitchen_light`, `action_set_brightness`, brightness N |
| `L\|B\|N` | `lightTargets['B']`, same action/parameter relation | `device_bedroom_light`, same action/argument |
| `L\|G\|N` | `lightTargets['G']`, same action/parameter relation | `device_living_light`, same action/argument |
| `L\|C\|N` | `lightTargets['C']`, same action/parameter relation | `device_corridor_light`, same action/argument |
| `D` | `doorTargetId`, `doorActionId` | `device_entrance_door`, `action_open_entrance`, no arguments |
| `U` | No target/action lookup | Unsupported |

Encoding verifies that IDs exist, that the target supplies the action, that the
argument key set exactly matches the definition, and that the target/action tuple
is uniquely representable through the configured compact mapping. A second light
in a room or an additional entrance cannot be substituted for the selected target.
Integer-to-decimal conversion preserves int64 without narrowing; the existing
strict decoder/serializer then enforces the frozen wire envelope. Nothing clamps,
guesses, retries, strips whitespace or switches mappings after a miss.

Accepted noncanonical legacy input remains compatible: `L|K|00050` decodes to
integer 50 and encodes canonically as `L|K|50`. Signs, floats, Unicode lookalike
digits/codes, suffixes, embedded NULs and whitespace remain invalid. CommandEngine's
model-output whitespace normalization stays outside this codec and is unchanged.

### Representation is not supported-domain or safety validation

This adapter checks **representation and identity**, not whether the installation
supports the requested value or the original text is safe. The existing 6.5
production range decision and 6.6 target selection remain where they are.

For an isolated 10..90 snapshot, `L|K|0` is still valid compact syntax and maps to
a candidate with brightness zero. Encoding it also remains possible; a later
domain evaluator must reject it as unsupported by that snapshot. It is not a
malformed-wire error and must not be rewritten to 10. The tests preserve all 405
wire tuples under the narrower fixture while confirming the actual snapshot
contains 10..90 bounds. They also check 9/10/40/90/91 boundaries without clamping.

Enabled/deny metadata and mandatory policy metadata are not activated by this
codec. A syntactically encodable door candidate is not permission to open a door.
Tests call the real unchanged SafetyGate to demonstrate that `дверь` + D becomes
U, a valid explicit entrance request preserves D, and contradiction02 plus U,
`L|K|50` or `L|K|0` remains U. No safety rule was copied into the model or adapter.

## Configuration identity proof and schema limits

All 405 decoded candidates are checked against independent expected stable IDs
and typed values, not merely against their own re-encoded strings. Adapter source
contains no hardcoded current installation action/device IDs. Parameter identity
is read from the configured definition as well.

The isolated fixture checks establish:

1. Renaming human metadata and reversing action/device order changes the validated
   snapshot hash but leaves all 405 semantic identities unchanged. Display text
   and record order are not identity.
2. A valid extra light, entrance and brightness-action definition use fresh schema
   IDs. GenericAction stores those IDs directly, even though compact v1 cannot
   address them. An ID absent from the original snapshot produces UnknownTarget
   or UnknownAction; in the supplied extra-entity snapshot it is recognized and
   returns Unrepresentable. A missing required argument on the extra action is
   also detected from its configured definition. These distinct results prove
   that membership/definition lookup reads the supplied catalog.
3. All 405 mapped tuples remain unchanged with extra entities present. The adapter
   never chooses the first device of the same kind/room or replaces D with another
   entrance.
4. Invalid/missing mappings fail whole-configuration validation. Attempts to remap
   pinned legacy IDs also fail. A null adapter context fails explicitly.
5. Adapter snapshot retention and independent candidate value lifetime are tested;
   modifying fixture copies cannot mutate the original catalog.

Schema v1 intentionally pins the compact action/target IDs, so this is **not** a
claim that arbitrary compact-code reassignment was tested or enabled. It is a
stable-identity and configuration-membership proof within the real schema. Neither
`config/current-m4.json` nor schema v1 was modified or weakened. Extra fixture
objects are storage-only test data, not newly activated product functionality.

## Build and deterministic verification

The baseline full build directory was created and built from the clean checkpoint
**before any code/CMake edits**. The core/full implementation directories were
also initially absent. All commands use the existing local dependencies without
downloads. Baseline configure used the full command below with
`baseline-full` instead of `clean-full` and without the new foundation option.

```sh
cmake -S . -B build/milestone-6.8/clean-core \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_GENERIC_ACTION_FOUNDATION=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/milestone-6.8/clean-core -j2
ctest --test-dir build/milestone-6.8/clean-core --output-on-failure -V

cmake -S . -B build/milestone-6.8/clean-full \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_GENERIC_ACTION_FOUNDATION=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab
cmake --build build/milestone-6.8/clean-full -j2
ctest --test-dir build/milestone-6.8/clean-full --output-on-failure -V
```

| Check | Result |
|---|---|
| Baseline full build | Passed before implementation |
| Clean core build / CTest | Passed, **18/18 suites**, 14.53 s |
| Clean full build / CTest | Passed, **28/28 suites**, 20.40 s |
| Standalone foundation build / new suites | Passed with assistant/config tools OFF, **2/2 suites**, 0.08 s |
| Compiler warnings/errors | None in baseline/core/full build logs |
| GenericAction | **22 runtime checks** plus compile-time type/const-access/constructibility assertions, passed in both builds |
| CompactV1Adapter | **72,819 checks**, passed in both builds |
| Exhaustive current-M4 space | **404 lights + one entrance = 405**; exact Generic -> compact -> Generic and compact -> Generic -> canonical compact |
| Leading-zero compatibility | All 404 light tuples with a leading-zero form, preserving canonical output |
| Malformed wire matrix | **18,684 rejected cases**, checked against the frozen decoder: explicit malformed forms, every whitespace byte at every position in all canonical actions, all 256 code bytes, numeric boundaries and extra fields/NULs |
| Existing core suites | Action 408, ActionValidator 431, parser 707, SafetyGate 98, CommandEngine 170 checks; unchanged expectations passed |
| 6.4.1 SafetyHardening | **157,283 checks**, unchanged and passed |
| Configuration validation/snapshot/store/shadow | **98 / 13 / 56 / 1,389 checks**, CLI suite also passed |
| Runtime config / room / brightness / entrance cutovers | **2,337 / 51,484 / 28,385 / 4,263 checks**, existing CLI/startup suites passed |
| Existing shadow probes | Baseline 1,364; brightness 668; entrance two, zero current-M4 mismatches |
| RKLLM failure wrapper / CLI / voice unit tests | Passed unchanged; no real inference involved |

The existing brightness voice-startup suite retains its one documented text-only
skip; no new test was skipped and no existing expectation or DEV label changed.
This CTest run does not imply a model-backed speech replay.

The foundation option is OFF by default. A separate default-OFF configuration
was checked to contain no Generic Action/adapter targets. The standalone
foundation configuration (assistant OFF, config tools OFF, foundation ON) also
exercises automatic availability of the existing configuration library without
requiring the production runtime bridge.

```sh
cmake -S . -B build/milestone-6.8/standalone \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_CONFIG_TOOLS=OFF \
  -DFIREFLY_BUILD_GENERIC_ACTION_FOUNDATION=ON
cmake --build build/milestone-6.8/standalone \
  --target generic_action_tests compact_v1_adapter_tests -j2
ctest --test-dir build/milestone-6.8/standalone \
  -R '^(GenericAction|CompactV1Adapter)$' --output-on-failure -V
```

### Production build isolation and hardware regression decision

The root CMake change only appends the default-OFF option and a guarded new
subdirectory; no existing target's sources, dependencies or options changed.
Normalized baseline-versus-full comparison found **all 40 existing compile
commands and all 22 existing link commands unchanged**. Only the four new source
compilations were added. Normalization replaced the two build-directory prefixes;
source paths, flags, definitions, include directories and link order were compared.

All six existing libraries (`firefly_core`, `firefly_rkllm`, `firefly_voice_core`,
`firefly_speech`, `firefly_runtime_config`, `firefly_config`) and the assistant,
frozen regression executable, mock CLI executable and speech-replay executable
are **byte-for-byte identical** between the clean checkpoint build and the full
build with the new foundation enabled. Representative SHA-256 values on both sides:

| Artifact | Identical baseline / foundation-enabled SHA-256 |
|---|---|
| `firefly-assistant` | `8d093227ee3d835755d7bd418e1b29ba5ee84b4b43234d4970b16f278c81580f` |
| `firefly-regression` | `8563285dd7987ba4271aa4234be4ed49791df3e5358cfa63dec5b6ef60b104c7` |
| `speech_replay_tests` | `009c456237f78d2854e0c5599888ed6b8a121fb177c4494375d6c54710b4f8fb` |

The production assistant has no `firefly::actions` symbols. ELF dependency/ldd
output and the complete compile/link/hash comparison are retained in the ignored
`build/milestone-6.8/` alongside configure/build/CTest logs and precheck source
hashes. No generated binaries or large logs are proposed for git.

Under the user's explicit isolation condition, real frozen NPU, integrated DEV
and model-backed voice regressions were **not rerun**: no shared production target
was changed, and the actual production binaries are identical. This is not a new
48/48 inference claim, a lucky-run selection, or a weakened acceptance criterion.
All requested existing deterministic suites did run unchanged. Hardware regression
must resume when a later milestone wires the foundation into production.

## Files and unchanged boundaries

| File | Change |
|---|---|
| [CMakeLists.txt](../CMakeLists.txt) | Nine appended lines: opt-in offline foundation; existing target declarations unchanged |
| [GenericAction.h](../include/firefly/actions/GenericAction.h) | Candidate value, integer-only argument type, Unsupported result |
| [GenericAction.cpp](../src/actions/GenericAction.cpp) | Owned construction and semantic equality |
| [CompactV1Adapter.h](../include/firefly/actions/CompactV1Adapter.h) | Explicit snapshot dependency and typed conversion results/errors |
| [CompactV1Adapter.cpp](../src/actions/CompactV1Adapter.cpp) | Frozen codec reuse and snapshot identity mapping |
| [actions/CMakeLists.txt](../src/actions/CMakeLists.txt) | Two isolated static libraries and two dedicated test targets |
| [GenericActionTests.cpp](../tests/actions/GenericActionTests.cpp) | Value semantics, Unsupported and compile-time argument-type rejection |
| [CompactV1AdapterTests.cpp](../tests/actions/CompactV1AdapterTests.cpp) | Exhaustive codec, identity, invalid arguments, lifetime and safety-separation tests |
| This report | Implementation, verification, scope and next-step recommendation |

The only pre-existing tracked file modified is the root CMake file. Precheck
hash comparison confirms every other tracked file is unchanged, including
production C++, existing test expectations, DEV data, configuration/schema and
vendored dependencies. The production parser output, RKLLM output/normalization,
SafetyGate, ActionValidator, authoritative CommandResult, voice result and CLI
output are deliberately not migrated. Model/prompt, lifecycle/settings, STT/VAD,
room/brightness/entrance cutovers and contradiction hardening are unchanged.

Final scope review found exactly one modified tracked file and eight new files,
all listed above. Of the 111 files tracked at the checkpoint, the other 110 have
identical SHA-256 hashes. Tracked and untracked whitespace checks pass; all nine
local report links resolve. There are no staged changes, unrelated modifications
or commits from this milestone.

## Recommended Milestone 6.9 boundary

Start with one read-only observer at CommandEngine's **shared final-result
boundary**, after the existing safety/domain checks and before presentation.
That is where both routes have an old typed Action, final compact representation
and (for D) the 6.6 configured entrance metadata. Supply the same process-owned
snapshot explicitly; do not load another configuration or invoke RKLLM again.

Compare adapter-decoded final compact output with an independently projected
generic tuple from the actual legacy Action fields and snapshot relationships.
For D, compare the retained production target ID as well; for lights, compare
the configured target/action IDs and the actual integer value. U must correspond
to no action and no stale entrance target. Merely decoding and re-encoding the
same string twice is not an independent semantic oracle.

Keep old production output authoritative. Inject deliberate tuple/mapping
disagreements in tests and require deterministic observable mismatches with no
repair/fallback. Preserve route, original-input safety, configured-range decisions,
failure classifications and snapshot lifetime; do not activate enable/deny policy
or change parser/LLM/voice representations. Rerun full deterministic and actual
NPU/integrated/replay regressions when that observer is introduced. Candidate-level
typed domain evaluation can follow as a separately bounded shadow increment.

No observer or 6.9 implementation is included now. Changes are left uncommitted
for review; the checkpoint HEAD remains unchanged.
