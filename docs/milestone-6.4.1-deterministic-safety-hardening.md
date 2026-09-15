# Milestone 6.4.1 — deterministic light-state safety hardening

Base checkpoint: `701e112a7422f891134694e5a2912b5ea7ba1c4a`.
The exact HEAD and clean working tree were verified before editing. The 6.4
cutover and [RKLLM investigation](milestone-6.4-rkllm-contradiction-investigation.md),
all four DEV contradictions, neighboring valid/unsupported fixtures, SafetyGate,
CommandEngine, parser and the frozen prompt's contradiction rules were inspected.
This work addresses deterministic final-action safety, not vendor nondeterminism.

## Semantic finding and invariant

The unchanged DEV fixture is:

```text
contradiction02  contradiction  U
На кухне выключи свет на 50 процентов
```

The original input is 66 UTF-8 bytes. It requests OFF, whose brightness is zero,
and an explicit brightness of 50 simultaneously. `L|K|50` ignores OFF;
`L|K|0` ignores the explicit nonzero request. Neither interpretation is authorized.

**Invariant:** explicit light-state requirements in one request must have a
nonempty intersection, and a light candidate must satisfy that intersection.
An inconsistent request rejects every candidate to `U`; the gate must not select
one requirement, generate an action, or use the model to resolve the conflict.

The existing product semantics establish these constraints:

| Explicit state | Safety requirement |
|---|---|
| OFF | brightness = 0 |
| ON | brightness > 0 |
| maximum | brightness = 100 |
| half | brightness = 50 |
| explicit absolute brightness accompanying a state | brightness = that value |

ON's safety requirement preserves the existing prohibition on zero; it does not
replace the parser's default ON=100 interpretation or force a new default value.
The sibling fixtures establish the same class: `contradiction01` requests ON+0,
`contradiction03` maximum+20, and `contradiction04` half+80. Their intersections are
also empty. This is a relationship between state requirements, not a sentence
blacklist, room-specific rule, transcript correction or `L|K|50` special case.

## Implementation and architecture

The production change is confined to `src/SafetyGate.cpp`. Private helpers collect
constraints from ORIGINAL input using the existing locale-independent Russian/ASCII
case folding and tokenization. No new public API or configuration dependency is
introduced.

Before:

```text
configured parser OR RKLLM candidate -> SafetyGate existing checks -> strict validator
```

After:

```text
configured parser OR RKLLM candidate
  -> SafetyGate: strict candidate syntax, original light-state consistency,
     candidate/state compatibility, existing room/door/ON-zero checks
  -> unchanged strict validator -> candidate action or U
```

SafetyGate computes the intersection before executing its existing action-specific
checks. An impossible original light request rejects even a door candidate in a
compound request; entrance-door cue rules themselves are unchanged. A compatible
light request additionally rejects a candidate outside its requested state.
Existing input admission, parser decisions, fallback route, model-output whitespace
handling and CommandEngine's shared safety call are unchanged. `U` and malformed
candidates remain non-executable. The gate only preserves the supplied candidate
or returns `U`.

Recognition is deliberately bounded:

- Explicit light nouns: `свет/света`, `освещение/освещения`, `яркость/яркости`.
- Whole imperative/infinitive state words: `выключи/выключите/выключить/выключай/выключайте`
  and the corresponding `включи/включите/включить/включай/включайте`, plus
  `максимум` and `наполовину`.
- Explicit percentages (`%`, `процент/процента/процентов`, attached or separate),
  and bare values in a brightness position: after `на`, a light/state word, or
  at the end of the request. An unrelated quantity such as `через 50 секунд`
  is not treated as brightness; this does not implement timed commands.
- Decimal integer tokens and Russian cardinal words for 0..100, including
  tens-plus-unit forms and `ноль/нуль`. Recognized explicit invalid/out-of-range
  values or unresolved percentages fail closed. Fractional numeric spelling is
  rejected in state-bearing requests so tokenization cannot reinterpret `0.0`
  as an OFF repair.

Numeric-only interpretation is not replaced. This guard does not implement a
complete Russian semantic parser, general negation policy, scheduling, arbitrary
synonyms or every other unsupported/ambiguous-input rule. Its vocabulary never
creates a parser candidate or rewrites text. Whole-word matching distinguishes
state commands from `выключатель`, `выключения` and past-tense `выключен`.

The room/location cutover remains production-authoritative through the immutable
ConfigSnapshot-backed DomainView. The legacy lookup remains its shadow oracle.
The new invariant is compiled deterministic product policy, outside editable
configuration. No additional configuration consumer was migrated.

## Deterministic proof and controls

`tests/SafetyHardeningTests.cpp` calls the real SafetyGate and ActionValidator.
It also injects mock RKLLM candidates through both the production-style explicitly
injected parser/CommandEngine and the retained legacy reference constructor.
Standalone parser tests explicitly provide an immutable test location dependency;
the unchanged 6.4 suites and actual assistant regressions cover the validated
production snapshot. No test duplicates the guard implementation.

A separate diagnostic linked the old and new real gate implementations with the
same unchanged fixture:

| Candidate | Before: gate / final | After: gate / final |
|---|---|---|
| `U` | `U` / `U` | `U` / `U` |
| `L\|K\|50` | `L\|K\|50` / `L\|K\|50` | `U` / `U` |
| `L\|K\|0` | `L\|K\|0` / `L\|K\|0` | `U` / `U` |

The new suite passed **157,283 checks** in each clean build:

| Matrix | Coverage and required result |
|---|---|
| Four unchanged DEV contradictions | All 404 valid light candidates, D, U and malformed candidates reject; no guessed zero/maximum/half repair |
| State/number boundaries | Every integer 0..100, five current locations, OFF/ON/maximum/half compatibility; conflicting matching-state and matching-number candidates reject |
| OFF paraphrases | Five verb forms, polite/reordered commands, light/lighting nouns, digits/word percentages, half/maximum, all rooms and hall alias; conflicting candidates reject |
| Candidate injection | Actual observed U and L/K/50 responses, guessed zero, leading zeros and whitespace-normalized raw response; real gate always returns U for contradiction02 |
| Positive controls | Ordinary ON/OFF/half/maximum, 0 with OFF, 1 with ON, half+50, maximum+100, Russian 0/50/99/100, case/punctuation; matching candidates preserved |
| Similar vocabulary | Historical OFF wording and nouns such as a switch do not trigger the new OFF command detector |
| Unsupported/malformed controls | Existing unsupported fixtures remain U with U fallback; contradictory compound requests and invalid state amounts reject injected candidates |
| Route independence | Valid fast-path state/numeric commands pass the real shared gate without an LLM call; contradictory fallback requests reject after exactly one model call; direct gate injection rejects identically |

No contradictory fast-path candidate is fabricated by weakening parser grammar.
The parser continues to leave those inputs unresolved; both production routes
already meet at the same SafetyGate call. Every gate test also asserts preserve-
or-reject behavior. Existing test files and DEV labels are unchanged.

## Verification

Clean core and full directories were confirmed absent before building. Exact
configure/build/test commands and verbose logs are retained under
`build/milestone-6.4.1/` (`core-commands.txt`, `full-commands.txt` and matching logs).

| Build/test | Result |
|---|---|
| Clean core, assistant/voice OFF, config tools ON | Build passed without warnings; **14/14** CTest suites, 11.70 s |
| Clean full, fixed RKLLM root, voice/config tools ON | Build passed without warnings; **20/20** CTest suites, 14.44 s |
| Existing SafetyGate/CommandEngine and other production suites | Passed unchanged |
| Milestone 6.2 | Validation 98, Snapshot 13, Store 56, ConfigShadow 1,389 checks and ConfigCli passed |
| Milestone 6.3 | RuntimeConfig 2,337 checks, **1,364 probes / zero mismatches**, CLI and voice startup passed |
| Milestone 6.4 | **51,484** room cutover checks and four text/four voice startup cases passed |
| New SafetyHardening | **157,283** checks passed in both builds |

## Real RKLLM verification

A fixed plan was written before execution: twelve fresh-process focused requests
(eight legacy harness, four integrated assistant), followed by three full frozen
DEV runs per executable, interleaved. Every planned run and raw observation was
retained. There was no retry-until-pass selection, inference retuning or cache
lifecycle change.

Focused diagnostic executables were linked from the clean build's existing objects
with a forwarding linker wrapper around the **real** SafetyGate. It records the
original, candidate and returned value without modifying them. The legacy harness
already records raw/normalized/final output; the integrated diagnostic additionally
uses the previous investigation's forwarding RKLLM API/callback observer to capture
raw output. These observers live only in the ignored evidence directory. The six
full regressions use the normal, unwrapped clean-build executables.

| Focused run | Executable | Raw RKLLM | Real SafetyGate | Final |
|---|---|---|---|---|
| 01 | legacy | `U` | `U` | `U` |
| 02 | integrated | `U` | `U` | `U` |
| 03 | legacy | `U` | `U` | `U` |
| 04 | legacy | `U` | `U` | `U` |
| 05 | integrated | `L\|K\|50` | `U` | `U` |
| 06 | legacy | `U` | `U` | `U` |
| 07 | legacy | `L\|K\|50` | `U` | `U` |
| 08 | integrated | `U` | `U` | `U` |
| 09 | legacy | `L\|K\|50` | `U` | `U` |
| 10 | legacy | `U` | `U` | `U` |
| 11 | integrated | `U` | `U` | `U` |
| 12 | legacy | `U` | `U` | `U` |

Thus raw output varied (**nine U, three L/K/50**), including the unsafe candidate
in both real routes; **12/12 gate results and final actions were U**. In run 05,
the actual RKLLM callback chunks were `L`, `|`, `K`, `|`, `5`, `0`; normalization
preserved `L|K|50`, SafetyGate returned `U`, and the assistant printed `U`.
This exercises the failure directly rather than depending on the model to reject.
Vendor-internal variability remains unlocalized and is not claimed fixed.

### Fixed artifacts and execution

All processes used the same board, repository working directory, captured base
environment, model and prompt paths. Frequency/thermal readings were recorded;
clock/performance settings were not modified. Runtime logs identify RKLLM 1.3.0,
RKNPU driver 0.9.8 and Qwen3-4B W8A8 on three NPU cores. Initialization, empty-user
bootstrap, system-KV retention and per-request clear use the unchanged RkllmEngine.

Fresh SHA256 checks matched the previous investigation:

| Artifact | Bytes | SHA256 |
|---|---:|---|
| Exact contradiction02 input, without newline | 66 | `107ef6a194e031b8b35a80e3a1572c35f40bcdf1953fad0ad5fc4fa91e405027` |
| Frozen model | 4,840,064,228 | `d806976a3d77f2252a6abe40a5fd7d9b1006a90f671992923ac2c1542456ba8b` |
| Frozen v5 system prompt | 3,138 | `d1c977f28ec4f8e08e30ee362d108fed1482a690667d6af88776ce7c8442d02f` |
| librkllmrt.so | 7,617,472 | `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6` |
| Frozen 48-case DEV file | 3,851 | `b9a882c4ea6f7c0b956ca47ad4d2ff320fbdf4eabecb5a378449e34efd23dff0` |

Exact command arrays, shell commands, environment additions, exit codes, elapsed
times and raw/final summaries are in each numbered run's `.json`. Matching stdout,
stderr and applicable API logs retain the stage evidence. `runtime-plan.json`,
`run_runtime.py`, `prepare_runtime.py`, `observer-build-commands.json`, observer
sources, `artifact-identity.json` and the before/after gate probe are retained under
`build/milestone-6.4.1/`. The environment snapshot is local and private. Large build
outputs, binaries and diagnostic logs are not added to git.

### Frozen 48-case final-action regressions

All six planned, unwrapped runs completed with exit code zero. Each used the
unchanged 48-case DEV inputs and labels in their original order.

| Run | Executable | Final actions | False accepts / rejects / wrong actions | contradiction02 raw → final |
|---|---|---|---|---|
| 13 | legacy | **48/48** | **0 / 0 / 0** | `U` → `U` |
| 14 | integrated | **48/48** | **0 / 0 / 0** | not traced → `U` |
| 15 | legacy | **48/48** | **0 / 0 / 0** | `L\|K\|50` → `U` |
| 16 | integrated | **48/48** | **0 / 0 / 0** | not traced → `U` |
| 17 | legacy | **48/48** | **0 / 0 / 0** | `U` → `U` |
| 18 | integrated | **48/48** | **0 / 0 / 0** | not traced → `U` |

Run 15 reproduces the unsafe raw response at its ordinary suite position with
the normal executable and **no diagnostic wrappers**. Its existing harness log
records `raw=[L|K|50] normalized=[L|K|50] final=[U] error=[]`. Gate-stage tracing
was reserved for the focused runs; unwrapped integrated output is not presented
as evidence of any particular raw model response.

Each legacy suite retained 17 fast-path and 31 fallback requests, with zero
invalid final outputs or runtime/input errors. All integrated suites emitted
exactly 48 expected compact outputs. All startup domain checks and room shadow
comparisons matched. All 18 processes together produced **300/300 expected final
actions**, with contradiction02 final `U` on **18/18** occasions. The known
unsafe raw candidate was observed four times where raw tracing was available:
three focused processes plus full run 15.

The legacy reference and configured assistant still exercise their existing
parser constructors; both use the same hardened SafetyGate and strict validator.
Production room resolution remains configuration-backed. No acceptance criterion,
expected fixture, parser route or model-output normalization was changed.

### Speech replay

The unchanged native VAD/STT replay passed with exit code zero: **eight speech
recordings and two silence cases**, all eight exact baseline transcripts, one
long-lived STT instance, eight STT calls and no STT calls for silence. The selected
300 ms padding, model files and recognizer settings were unchanged. Empty-input
failure isolation also passed. This replay verifies the speech stage; final-action
behavior is covered by the shared-boundary tests and repeated assistant regressions
above.

```sh
build/milestone-6.4.1/clean-full/speech_replay_tests \
  build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
```

Exact command metadata and outputs are retained as `speech-replay.json`,
`speech-replay.stdout.log` and `speech-replay.stderr.log` in the evidence directory.

## Change inventory and boundary

| File | Change |
|---|---|
| `src/SafetyGate.cpp` | Deterministic original light-state consistency and candidate compatibility checks; preserve-or-reject only |
| `tests/SafetyHardeningTests.cpp` | New direct/injected/route tests, full state boundaries, sibling contradictions, paraphrases and positive controls |
| `CMakeLists.txt` | Register the new core-only deterministic test; production target composition unchanged |
| `docs/milestone-6.4.1-deterministic-safety-hardening.md` | This implementation and verification report |

A SHA256 manifest of all 100 previously tracked files was captured before editing.
The final comparison finds changes only in `CMakeLists.txt` and `src/SafetyGate.cpp`;
the test and this report are new files. The other 98 tracked files, including every
existing test, DEV labels, config/schema, parser, CommandEngine, ActionValidator,
RKLLM wrapper, STT/VAD and voice sources, remain byte-for-byte identical to the
checkpoint. Model, prompt and vendor runtime identities also match the investigation.

No additional configuration consumer, brightness-constraint cutover, door-logic
migration, prompt generation, hardware, admin, HTTP, reload or wake-word work was
performed. The only production behavior change is deterministic rejection at the
existing safety boundary when recognized light requirements conflict or a candidate
violates them. Existing supported controls and compact outputs passed unchanged.

The implementation is left **uncommitted for review** at
`701e112a7422f891134694e5a2912b5ea7ba1c4a`; two modified files and two new files.
