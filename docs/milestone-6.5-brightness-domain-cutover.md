# Milestone 6.5 — brightness domain production cutover

Base checkpoint: `df0d30ff9e3a3506da384050ebc3e15e58606980`.
That exact HEAD and a clean working tree were verified before editing. The
[6.1 design](milestone-6.1-dynamic-configuration-design.md),
[6.2 foundation](milestone-6.2-configuration-foundation.md),
[6.3 runtime shadow](milestone-6.3-runtime-configuration-shadow.md),
[6.4 room cutover](milestone-6.4-parser-room-cutover.md) and
[6.4.1 safety hardening](milestone-6.4.1-deterministic-safety-hardening.md) were
read alongside the actual implementation. The preimplementation dependency map
and checkpoint file hashes are preserved in `build/milestone-6.5/`.

## Exact responsibility and discovered dependencies

Only **installation support for a decoded light brightness** is cut over.
ConfigSnapshot-backed bounds now determine whether a structurally valid, safe
light candidate may become a final action. The current sample remains 0..100.
The schema already permits tighter integer bounds within the intrinsic percentage
envelope; neither schema nor production sample was edited.

| Existing dependency | Classification and outcome |
|---|---|
| `DeterministicParser.cpp`, `compileRules` numeric regexes | Independent 0..100 canonical-decimal grammar envelope; unchanged. `parseRules` converts only matched digits with `stoi`. Negative, fractional, oversized and noncanonical numbers still remain unresolved. The parser proposes a candidate; it no longer establishes installation support. |
| Parser ON/OFF/maximum/half values | Language semantics: 100/0/100/50; unchanged. Restricting the maximum to 90 does not redefine the word “maximum.” Its 100 candidate becomes unsupported. |
| `ActionValidator.cpp`, decimal decoder | Frozen compact-v1 structure and intrinsic percentage envelope: exact target codes and separators, unsigned ASCII integer encoding, magnitude at most 100, existing leading-zero compatibility. Unchanged; successful decoding alone no longer authorizes installation support. |
| `Action.cpp`, `toCompact` | Frozen serializer checks valid Room and integer percentage 0..100. Unchanged representational invariant, not an installation capability lookup. |
| `SafetyGate.cpp`, `requestedLightState` and candidate checks | Compiled safety meanings: OFF=0, ON excludes zero, maximum=100, half=50, compatible explicit values, original room evidence and door cues. Entire source and compiled object unchanged. These are not editable bounds/defaults. |
| `config/schema-v1.json`, `ConfigValidator.cpp` | Intrinsic capability/type/unit constraints and compatibility references. Required integer min/max must be within 0..100 and ordered. Exact device/action/parameter definitions must validate before a snapshot exists. Unchanged. |
| `Configuration.h`, `Parameter` defaults | Typed construction defaults, not JSON field fallback. The loader requires both bounds; unchanged. |
| `DomainView::brightnessBounds` / `allowsBrightness` | Existing observational helper also checks `EffectiveAction::permitted`. Reusing it directly would additionally activate device/action enablement and deny restrictions. It remains unchanged. |
| `CommandEngine::process`, final-action publication | **Migrated boundary:** after existing SafetyGate and strict decoding, a light must satisfy `DomainView::supportsBrightness`. Both fast-path and RKLLM candidates pass this one authoritative support check. |
| `ShadowComparison.cpp` / catalog compatibility differences | Existing legacy comparison remains observational. A new range-only comparison reuses the actual frozen decoder, with no copied range/room table. |
| Frozen prompt, tests and DEV data | Prompt still describes the intrinsic current-M4 0..100 domain; no context generation or tuning. Existing expected DEV outputs remain unchanged. Tests remain independent oracles, with one obsolete 6.3 CLI assertion updated below. |

The remaining 0..100 checks constrain **what the legacy protocol and language can
represent** or **what explicit states mean**. They do not read or override the
installation's selected bounds. Configuration cannot widen the intrinsic percent
capability or weaken the deterministic safety rules. A narrower configuration is
enforced after those checks; there is no second hardcoded installation range.

## Architecture and ownership

Before:

```text
ConfigSnapshot -> DomainView -> configured parser room locations
                           -> brightness bounds observed only at startup
parser / RKLLM -> SafetyGate -> strict ActionValidator -> final Action
```

After, in both text and voice application assembly:

```text
RuntimeConfig owns one immutable ConfigSnapshot and DomainView
  -> unchanged configured parser + legacy room shadow guard
  -> DomainView implements the read-only LightBrightnessDomain interface

original input -> parser / RKLLM -> unchanged SafetyGate -> strict decoding
  -> light: configured range support -> final Action or U
  -> door: existing result, without a brightness lookup
```

The production constructor is now
`CommandEngine(llm, parser, runtimeConfig.domain())`. The parser and domain view
outlive CommandEngine and the voice worker. Neither dependency is optional on this
constructor. There is no file path, JSON parsing, reload or mutable global state
in CommandEngine. Snapshot loading/validation and process-lifetime ownership are
unchanged.

`LightBrightnessDomain` is one const predicate accepting an already decoded
compact room code and integer value. This minimal consumer interface preserves
the core's independence from the runtime/configuration libraries; it introduces
no second configuration model or persistence abstraction. `DomainView` implements
it alongside the existing `ParserRoomDomain` interface.

The range view resolves the code through the snapshot's existing compatibility
target mapping, then selects the effective action using the configured
`lightActionId`. It reads that action's validated `brightnessBounds`. All four
current targets refer to the one shared brightness action; there are no new
per-room hardcoded ranges or IDs. A missing target/action/bounds returns false,
never 0..100 defaults.

This new method deliberately reads only range metadata, not `permitted`, device
enablement, action enablement or deny restrictions. Those fields remain
observational as before. Tests explicitly distinguish the unchanged
permission-aware shadow helper from the production range-only view. This is not
a claim that configuration permissions have been activated.

The previous one-argument and parser-only CommandEngine constructors remain
explicitly documented historical reference entry points for the unchanged legacy
harness and standalone suites. They are not fallback branches of the production
constructor. Both actual application entry points use all three arguments. The
legacy NPU harness retains its original layer; integrated regressions exercise
the new production wiring.

## Failure and shadow comparison

Malformed, missing, inverted, out-of-intrinsic-range or incompatible brightness
definitions fail the existing whole-document loader/validator before model
initialization and before any snapshot is published. No fallback file or partial
configuration is introduced. A valid narrower range is accepted as data and used
exactly as validated.

At request time, the unchanged SafetyGate runs first. `ActionValidator` then
establishes structural validity; the gate's returned string supplies a validated
room code to the range check. A configured miss clears the candidate and records
`Brightness outside configured domain` in the existing error diagnostics. Final
output is `U`. A lookup exception uses existing fail-closed exception handling.
Neither path clamps, selects a legacy answer, repairs an OFF command nor invokes
RKLLM again. Door actions do not query the brightness interface.

`compareLegacyBrightness` probes all 256 room-code bytes and values -1..101 for
every current light target, using `ActionValidator::validate` as the existing
legacy oracle. This is **668 deterministic probes** for current M4. No new table
of legacy ranges is used. The separate 6.3 baseline comparison remains **1,364
probes** and retains its `scope=legacy_domain_baseline` label.

RuntimeConfig retains both immutable comparison results and emits a concise
startup line:

```text
BRIGHTNESS_DOMAIN authoritative=config checks=668 mismatches=0 legacy_shadow=matched
```

For a valid 10..90 range there are 80 differences: 20 legacy-supported values
outside the new range on each of four lights. The diagnostic says
`mismatches=80 legacy_shadow=mismatch` and identifies the first stable mismatch
key; the full ordered list remains accessible. Tests call `requireEquivalent()`
on current M4 and assert that it throws for intentionally remapped fixtures.

Unlike the existing 6.4 room-alias activation guard, a valid tighter brightness
range is not vetoed globally merely for differing from legacy. In-range values
use the new domain; out-of-range values reject. The legacy comparison never
supplies the decision. This permits the requested isolated configuration proof
while retaining deterministic, visible mismatch evidence. Room alias drift still
fails startup exactly as in 6.4.

## Configuration-driven proof and boundary matrix

`BrightnessCutoverTests.cpp` builds validated test-only snapshots for **0..100,
10..90, 0..0, 100..100 and 50..50** using the unchanged schema/validator. It injects
each real view into the production CommandEngine constructor. The parser source
and `config/current-m4.json` are unchanged throughout.

| Check | Result |
|---|---|
| Current-M4 0 and 100 | Accepted across kitchen, bedroom, living room/hall and corridor |
| Configured minimum/maximum, their adjacent values and every integer -1..101 | Membership agrees with each independent test range; no endpoint fallback |
| Isolated 10..90, candidates 10 / 40 / 90 | Accepted through both fast path and injected fallback |
| Isolated 10..90, candidates 0 / 9 / 91 / 100 | Final U, terminal range diagnostic; no repair or second inference |
| Singleton ranges | Only their sole numeric value is supported; maximum/half/OFF are never reinterpreted to fit |
| All current locations | Configured room lookup composes with the same range decision, including hall alias equivalence |
| Unknown target codes and signed integer extremes | Every unsupported code and negative/overflow-scale value rejects without defaults |
| Malformed/missing brightness and unsupported input | Existing grammar/strict decoding/rejection behavior preserved; no syntax broadening |
| Injected lookup miss or exception | U on either route, with no legacy fallback or retry |
| Original contradiction02 + U / L/K/50 / L/K/0 | U; unchanged SafetyGate rejects before range lookup |
| All four frozen contradictions | Injected candidates across all current rooms and representative brightness values reject for every tested range |
| Ordinary OFF/zero and ON/nonzero controls | Existing semantics preserved; configured support can further reject but cannot redefine the state |
| Door candidate | Unchanged gate/validator result; no brightness predicate call |
| Disabled/denied metadata | Old permission-aware helper observes it; range-only production check does not activate another consumer |
| Source file replaced after READY | Running process retains original narrower bounds; a new process sees the replacement |

The direct suite passed **28,385 checks** in each clean build. An instrumented
test dependency counts exactly one shared range check per safe light candidate
on each route and zero for a prior safety rejection or door action. Actual
ConfigSnapshot-based tests prove that the result comes from data, not that spy.

Text CLI tests use the real entry point with the unchanged fake RKLLM API. They
exercise current/narrow/singleton bounds, malformed capabilities before model
initialization, retained snapshot state, room guard composition and unchanged
permission scope. Voice startup tests run the same configuration scenarios and
stop safely at a deliberately absent VAD model; the text-only request/lifetime
method is explicitly skipped there. They do not claim live microphone coverage.

One historical assertion required a legitimate update:
`RuntimeConfigCliTests.test_valid_drift_reports_mismatch_without_changing_outputs`
expected max=99 to be ignored and `L|K|100` to pass. It now expects `U` and is named
`test_valid_drift_changes_only_migrated_brightness_support`. Door results, mismatch
diagnostics and all other assertions remain unchanged. Keeping its old expectation
would contradict the authorized cutover. No DEV labels or production unit tests
were rewritten to hide a regression.

## Verification results

Both build directories were absent before configuration. Exact commands and
verbose logs are retained under `build/milestone-6.5/` in `core-commands.txt`,
`full-commands.txt` and corresponding configure/build/ctest logs.

| Build/suite | Result |
|---|---|
| Clean core, assistant/voice OFF, configuration tools ON | Build passed; **15/15** CTest suites, 13.99 s |
| Clean full, fixed RKLLM root, voice/configuration tools ON | Build passed; **23/23** CTest suites, 17.98 s |
| Existing production unit suites | Passed with unchanged source and expectations |
| Milestone 6.2 | Configuration validation/snapshot/store/shadow/CLI suites passed |
| Milestone 6.3 | Runtime snapshot/shadow/startup suites passed; the single superseded CLI expectation is identified above |
| Milestone 6.4 | **51,484** room cutover checks, text and voice admission suites passed |
| Milestone 6.4.1 | **157,283** safety-hardening checks passed unchanged |
| Milestone 6.5 | **28,385** C++ checks; five text CLI methods; four voice startup methods passed (one text-only method skipped) |
| Current-M4 comparisons | Old baseline **1,364 / zero mismatches**; new brightness **668 / zero mismatches** |

### Real NPU and integrated final-action regressions

A fixed plan ran three legacy harness and three integrated assistant processes,
interleaved, on the original 48-case DEV order. All six use the normal clean-build
executables, without diagnostic wrappers. Model, prompt, RKLLM parameters/cache
lifecycle, environment and clock/performance settings were not changed. Every
planned run completed with exit code zero; there was no retry-until-pass selection.

| Run | Executable | Final actions | False accepts / rejects / wrong actions | contradiction02 raw → final |
|---|---|---|---|---|
| 01 | legacy | **48/48** | **0 / 0 / 0** | `L\|K\|50` → `U` |
| 02 | integrated | **48/48** | **0 / 0 / 0** | not traced → `U` |
| 03 | legacy | **48/48** | **0 / 0 / 0** | `U` → `U` |
| 04 | integrated | **48/48** | **0 / 0 / 0** | not traced → `U` |
| 05 | legacy | **48/48** | **0 / 0 / 0** | `U` → `U` |
| 06 | integrated | **48/48** | **0 / 0 / 0** | not traced → `U` |

All **288/288 final actions** matched the unchanged expectations. Every run kept
17 fast-path and 31 fallback commands. The integrated runs each reported exactly
one matching brightness diagnostic, one matching room-authority diagnostic and
zero baseline shadow mismatches. The legacy harness remains configuration-free;
it independently confirms that the unchanged 6.4.1 gate still rejects the actual
unsafe raw response. No particular raw response is inferred from untraced
integrated outputs. The known RKLLM variability is not claimed fixed.

Command forms (executed from the repository root with equivalent absolute paths
recorded in each run's metadata):

```sh
build/milestone-6.5/clean-full/firefly-regression \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --data tests/data/dev.tsv
# All 48 original DEV texts supplied on stdin:
build/milestone-6.5/clean-full/firefly-assistant \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --config config/current-m4.json --verbose
```

Exact command arrays, environment additions, host readings, exit codes and output
summaries are preserved in numbered `*-full-*.json` metadata and matching stdout/
stderr logs under `build/milestone-6.5/`; `runtime-plan.json`, `run_runtime.py`
and `runtime-summary.json` retain the fixed plan and aggregate results. The local
environment capture is private and is not added to git.

### Speech replay

The unchanged native replay passed with **eight speech WAVs and two silence
cases**, all eight exact baseline transcripts, one long-lived STT instance and
exactly eight STT calls. Neither silence case invoked STT. Empty-input failure
isolation also passed. This verifies the speech stage; action behavior is covered
by the shared-boundary suites and repeated integrated regressions above. No fresh
live microphone result is claimed.

```sh
build/milestone-6.5/clean-full/speech_replay_tests \
  build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
```

Command metadata and transcripts are retained in `speech-replay.json`,
`speech-replay.stdout.log` and `speech-replay.stderr.log` in the same evidence
directory. No model/runtime/audio artifact is added to git.

## Files and scope audit

Modified:

- `include/firefly/CommandEngine.h`, `src/CommandEngine.cpp`: required production
  brightness dependency and one shared final-action support boundary.
- `include/firefly/runtime/DomainView.h`, `src/runtime/DomainView.cpp`: range-only
  implementation over existing immutable effective-action metadata.
- `include/firefly/runtime/ShadowComparison.h`, `src/runtime/ShadowComparison.cpp`:
  separate deterministic range oracle using the existing decoder.
- `include/firefly/runtime/RuntimeConfig.h`, `src/runtime/RuntimeConfig.cpp`:
  retained comparison and scoped startup diagnostic.
- `src/main.cpp`, `src/VoiceMode.cpp`: one constructor argument at each existing
  assembly site; command loops, speech processing and state machine unchanged.
- `src/runtime/CMakeLists.txt`: three new test registrations.
- `tests/runtime/RuntimeConfigCliTests.py`: the one superseded assertion/name above.

Added:

- `include/firefly/LightBrightnessDomain.h`.
- `tests/runtime/BrightnessCutoverTests.cpp`.
- `tests/runtime/BrightnessCutoverCliTests.py`.
- This report.

Build comparison with clean 6.4.1 verifies byte-identical RKLLM, voice-core and
speech archives and Action, ActionValidator, parser, SafetyGate, Text and
InputPolicy objects. The legacy NPU harness has no DomainView/ConfigLoader symbols.
Vendored JSON include paths remain private to configuration sources/tests; no
dependency was added or downloaded. Fresh SHA256 checks matched the prior model,
prompt, runtime library, header and frozen DEV identities. Build/link and artifact
identity evidence is retained in the milestone's ignored directory.

Only the brightness-domain responsibility was cut over. The 6.4 configuration-
backed room path remains authoritative; its legacy guard is unchanged. The 6.4.1
deterministic safety code remains intact. No entrance-target, safety policy,
enablement/deny, generic Action, prompt/context, hardware, HTTP/admin, live reload
or wake-word migration was started.

Changes remain uncommitted for review at
`df0d30ff9e3a3506da384050ebc3e15e58606980`.
