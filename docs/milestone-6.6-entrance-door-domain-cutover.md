# Milestone 6.6 — entrance-door domain production cutover

Base checkpoint: `b7631d50fce13b109e68f91129c75596fffc2d4d`.
The exact HEAD and clean working tree were verified before editing. The [6.1 design](milestone-6.1-dynamic-configuration-design.md),
[6.2 foundation](milestone-6.2-configuration-foundation.md),
[6.3 runtime shadow](milestone-6.3-runtime-configuration-shadow.md),
[6.4 room cutover](milestone-6.4-parser-room-cutover.md),
[6.4.1 safety](milestone-6.4.1-deterministic-safety-hardening.md) and
[6.5 brightness cutover](milestone-6.5-brightness-domain-cutover.md)
reports and current parser, CommandEngine, SafetyGate, validator, codec,
configuration validator and runtime assembly were inspected. Checkpoint hashes
and the preimplementation dependency map are retained under
`build/milestone-6.6/`.

## Exact responsibility and dependency map

Only **resolving the installation target for an accepted entrance-door action**
has moved into production. The current target is still `device_entrance_door`;
current user input and compact output behavior are unchanged.

| Existing dependency | Classification and outcome |
|---|---|
| `Action.h`, `OpenEntranceDoor` | An empty type representing the single implicit entrance; unchanged. No generic Action conversion. |
| `ActionValidator.cpp`, `D` decoding | Accepts the exact compact door token; unchanged structural/protocol constraint. Decoding does not itself establish a configured installation target. |
| `Action.cpp`, `toCompact` | Serializes the existing door action as `D`; unchanged. No ID/name added to the protocol. |
| `DeterministicParser.cpp` | Has no door command rule. Door requests remain unresolved and take the existing fallback route. Config-backed room resolution and its shadow guard are unchanged. |
| `SafetyGate.cpp` | Requires explicit opening intent, a door stem and explicit entrance evidence in ORIGINAL input. Compiled deterministic policy, unchanged and not configurable. The 6.4.1 contradictory-light rule is also unchanged. |
| `CommandEngine::process` | Previously published a safe, decoded `D` without resolving an installation target. **This final-action publication boundary is the migrated responsibility.** |
| `DomainView::entranceTarget` | Existing snapshot-backed lookup through `compatibility.doorTargetId`; now reused by the production resolver. |
| `DomainView::effectiveAction` | Existing snapshot relation establishes that the selected target supplies the configured entrance-opening operation. |
| `DomainView::entranceAvailable` | Existing permission-aware observational helper; unchanged. It is not used for production identity because doing so would additionally activate enablement/deny policy. |
| Schema/`ConfigValidator.cpp` | Existing v1 contract pins the legacy target/action IDs, roomless entrance kind and operation. Missing/unknown/inconsistent mappings fail validation before a snapshot exists. No schema or validator changes. |
| Prompt, model, fixtures and CLI/voice rendering | Still use the same sole entrance meaning and `D` representation; unchanged. No generated prompt, new aliases or user-facing target rendering. |

Ordinary domain data is the selected device record, its stable identifier,
display name and validated relation to the entrance-opening action. Mandatory
intent/original-text evidence is safety policy. Neither a device label nor an
admin field can redefine that policy in this milestone.

## Architecture and ownership

Before:

```text
RuntimeConfig -> immutable snapshot / DomainView
  -> configured parser room lookup + configured brightness support
  -> entrance target observed only in startup checks
parser / RKLLM -> SafetyGate -> strict decoding -> D with implicit target
```

After:

```text
RuntimeConfig -> same immutable snapshot / DomainView
  -> unchanged parser room lookup and brightness support
  -> EntranceDoorDomain::resolveEntranceDoor()

original input -> unchanged parser / RKLLM -> unchanged SafetyGate -> decoding
  -> light: unchanged configured brightness boundary
  -> D: resolve configured entrance -> existing Action + owned target metadata
        missing/failed resolution -> U
```

Both text and voice assembly now construct:

```cpp
CommandEngine engine(llm, parser, runtimeConfig.domain(), runtimeConfig.domain());
```

The last argument is a minimal read-only `EntranceDoorDomain` interface, implemented
by the existing DomainView. It is one resolver, not another configuration store or
model. Core consumers do not load JSON, know file paths or depend on the full
configuration library. The existing production owner/view outlive the parser,
engine and voice worker. One validated snapshot is still loaded once, before
model initialization, with no reload or mutable global configuration.

The resolver uses `entranceTarget()` and the snapshot's `doorActionId`; it checks
roomless entrance kind and the effective opening operation. It returns the
selected device's ID and display name. It never scans for another target, uses
a display name as an identifier, or inserts a legacy ID literal.

`CommandResult::entranceTarget` owns a copy of this resolved metadata for accepted
configured door results. Rejected/light results carry no entrance target. This
makes the resolved installation identity available without changing the empty
legacy `OpenEntranceDoor` action, compact codec, CLI output or hardware behavior.
Tests verify that the result remains valid after engine/view destruction.

The previous one-, two- and three-argument constructors are explicitly historical
reference entry points for unchanged standalone tests and the frozen NPU harness.
They are not runtime fallback branches. Both actual application assemblies require
all four arguments.

Enablement, action enablement and deny restrictions remain observational, as in
6.5. The existing `entranceAvailable()` helper continues to report them. This
identity-only cutover does **not** claim to activate configuration permission
policy. Tests retain that boundary explicitly; SafetyGate always runs regardless
of those metadata fields.

## Failure behavior and legacy shadow

Invalid/missing targets, wrong kinds/room associations, absent actions, incompatible
IDs and malformed safety fields fail whole-document validation at startup, before
RKLLM or speech model initialization. No partial snapshot or fallback file is
introduced. At request time, a null resolver result records
`Configured entrance-door target unavailable` and returns `U`. Resolver exceptions
use the existing fail-closed engine handler. No replacement target, repair,
second inference or stale previous result is used.

`compareLegacyEntranceDoor` makes two startup comparison probes:

1. Configured resolution presence agrees with the actual frozen validator's `D`
   action.
2. The configured entrance entity set agrees with the single entrance represented
   by that legacy action.

The legacy Action contains no stable ID or display label to compare. Stable-ID
compatibility is enforced by the **existing** v1 validator contract, not by a
new copy of a legacy table. Cosmetic display-name changes are therefore not
semantic shadow mismatches. This is not a claim that `D` itself supplies an ID.

RuntimeConfig retains the immutable comparison result and emits:

```text
ENTRANCE_DOMAIN authoritative=config checks=2 mismatches=0 legacy_shadow=matched
```

A valid test-only additional stored entrance produces exactly
`mismatches=1 legacy_shadow=mismatch first=entrance_entity_set`. Tests assert that
`requireEquivalent()` throws and that repeated comparison yields the same ordered
mismatch list. The selected target still comes exclusively from the configured
mapping; the extra record is not addressable by `D` and is never chosen by record
order or kind. Shadow drift is observational, as for the 6.5 brightness comparison;
it cannot supply or repair a production result. An unresolved selected target
always rejects independently of shadow output.

The original 6.3 baseline retains 1,364 probes; brightness retains 668. Their
functions and existing expectations are unchanged.

## Configuration-driven proof

Schema v1 deliberately forbids renaming `device_entrance_door` to another stable
ID, even if all references are updated. It permits changing human metadata. The
user-requested proof therefore uses **valid target metadata changes**, not a
schema relaxation or an unsupported ID reassignment:

- Load current M4 and copy its typed configuration into isolated test fixtures.
- Change only the selected target's display name to `Главный вход — тест`, then
  `дверь`; also reverse device record order.
- Run the real validator, DomainView, configured parser and four-argument
  production CommandEngine.
- Observe the configured stable ID and each changed name in the actual accepted
  `CommandResult::entranceTarget`, while compact output stays `D`.
- Verify that `дверь` and `открой дверь` still reject, including when the target's
  display name is literally `дверь`. Metadata does not become language evidence.
- Reject attempts to remap the stable legacy ID, remove the target, reference an
  unknown/light target, remove its action or assign it a room.

`config/current-m4.json`, the schema and all production parsing/safety source
remain unchanged. A separate valid extra-entity fixture demonstrates observable
shadow drift and exact configured selection without enabling another door.

## Door safety matrix and deterministic proof

The C++ suite uses 40 input rows (including all six door-related frozen DEV rows)
under three validated metadata variants. Each is exercised with raw candidates
`D`, whitespace-wrapped `D`, `U`, empty output, `D|device_entrance_door` and
`D|unknown_door`. Assertions check actual SafetyGate behavior, strict syntax,
final output, target metadata, lookup counts, unchanged original/raw text and
absence of retries. There are 16 positive and 24 negative rows, including repeated
canonical controls/DEV entries.

| Original input / group | Final with a D candidate |
|---|---|
| `дверь` | `U` |
| `открой дверь` | `U` |
| `открой входную дверь` | `D` |
| `открыть дверь на входе` | `D` |
| All five existing opening forms with `входную дверь` and `дверь на входе` | `D` |
| Existing uppercase, punctuation and tab-separated entrance controls | `D` |
| `Открой дверь в спальню`, `открой дверь в ванную` | `U` |
| `Закрой входную дверь`, `Разблокируй входную дверь` | `U` |
| Missing intent, missing door or missing entrance cue | `U` |
| `аткрой входную дверь`, `открой входную тверь`, `открытвер на входе` | `U`, no transcript correction |
| `открой дверь на входеже`, `открой дверь страна входе`, `открой дверь на другом входе` | `U` |
| Malformed/non-D candidates for any door row | `U` |
| Valid door intent with missing/throwing target resolver | `U`, diagnostic, no legacy fallback |
| `На кухне выключи свет на 50 процентов` + U / L\|K\|50 / L\|K\|0 / D | `U`, before target lookup |

These are the existing frozen cue semantics, not a new general natural-language
ambiguity or negation detector. All four current rooms plus the hall alias retain
light results at 0, 40 and 100 without querying the entrance resolver.

The new C++ suite passed **4,263 checks** in both builds. Four text CLI tests and
four voice startup tests exercise actual assembly, invalid target admission,
metadata edits, drift diagnostics and unchanged permission scope. Voice startup
tests stop at a deliberately absent VAD model; they do not claim microphone or
voice request coverage. Existing configuration ownership/lifetime tests also run
unchanged.

## Verification

Both core/full build directories were absent before configuration. Commands and
verbose configure/build/CTest logs are retained in `build/milestone-6.6/`.

```sh
cmake -S . -B build/milestone-6.6/clean-core \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/milestone-6.6/clean-core -j2
ctest --test-dir build/milestone-6.6/clean-core --output-on-failure -V

cmake -S . -B build/milestone-6.6/clean-full \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT="$PWD/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu" \
  -DSHERPA_ONNX_INCLUDE_DIR="$PWD/build/stt/vad-ab"
cmake --build build/milestone-6.6/clean-full -j2
ctest --test-dir build/milestone-6.6/clean-full --output-on-failure -V
```

| Build/suite | Result |
|---|---|
| Clean core | Build passed, **16/16 suites**, 14.30 s |
| Clean full | Build passed, **26/26 suites**, 20.23 s |
| Existing production suites | All passed with byte-unchanged source/expectations |
| 6.2 validation / snapshot / store / shadow / CLI | Passed; 98 / 13 / 56 / 1,389 C++ checks |
| 6.3 runtime ownership/shadow/startup | Passed; 2,337 C++ checks; original baseline 1,364 probes, zero mismatches |
| 6.4 room cutover | Passed; 51,484 C++ checks plus text/voice startup suites |
| 6.4.1 safety hardening | Passed; 157,283 checks unchanged |
| 6.5 brightness cutover | Passed; 28,385 checks plus text/voice suites; 668 probes, zero mismatches |
| 6.6 entrance cutover | Passed; 4,263 C++ checks, four text/four voice startup methods; two probes, zero mismatches |

The existing brightness voice suite retains its one documented text-only skip;
all new entrance tests ran. No existing test or expected label was edited.

### Real runtime final-action regressions

Three frozen harness and three integrated assistant processes ran in a fixed,
interleaved plan on all 48 original DEV cases. Every planned run completed with
exit code zero; none was discarded or retried for a favorable raw result.

| Run | Executable | Final correct | False accepts / rejects / wrong actions | contradiction02 raw → final |
|---|---|---|---|---|
| 01 | Frozen NPU harness | 48/48 | 0 / 0 / 0 | `U` → `U` |
| 02 | Integrated assistant | 48/48 | 0 / 0 / 0 | not traced → `U` |
| 03 | Frozen NPU harness | 48/48 | 0 / 0 / 0 | `L\|K\|50` → `U` |
| 04 | Integrated assistant | 48/48 | 0 / 0 / 0 | not traced → `U` |
| 05 | Frozen NPU harness | 48/48 | 0 / 0 / 0 | `L\|K\|50` → `U` |
| 06 | Integrated assistant | 48/48 | 0 / 0 / 0 | not traced → `U` |

All **288/288 final actions** matched; all runs retained 17 fast-path and 31
fallback commands. Each integrated run reported exactly one
matching entrance-authority diagnostic, matching room/brightness diagnostics and
zero baseline shadow mismatches. The frozen harness retains its historical
configuration-free constructor; integrated runs exercise the actual production
cutover. The known raw RKLLM variability was observed and remained contained by
the unchanged 6.4.1 gate. No raw result is inferred for untraced integrated runs.

```sh
build/milestone-6.6/clean-full/firefly-regression \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --data tests/data/dev.tsv
# All 48 original DEV texts supplied on stdin:
build/milestone-6.6/clean-full/firefly-assistant \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --config config/current-m4.json --verbose
```

Exact command arrays, working directory, fixed plan, host clock/thermal readings,
return codes and stdout/stderr logs are retained in `runtime-plan.json`,
`runtime-summary.json` and the numbered `*-full-*` artifacts. Environment was
captured locally in a private file, unchanged for all runs and not added to git.
Model initialization, cache lifecycle, inference parameters and performance
settings were not changed.

### Speech replay

The unchanged native replay passed all eight speech WAVs and two silence cases,
with the eight exact baseline transcripts, one long-lived recognizer and exactly
eight STT calls. Silence did not invoke STT; empty-input failure isolation also
passed. In particular, the first three recordings still transcribe to `дверь`,
`открой дверь` and `открой входную дверь`, whose final actions are independently
covered by the real CommandEngine matrix above.

```sh
build/milestone-6.6/clean-full/speech_replay_tests \
  build/stt/model build/stt/vad-ab/silero_vad.onnx build/stt
```

`speech-replay.json` and stdout/stderr logs retain the command, exit code,
transcripts and timings. This verifies native VAD/STT replay; it does not claim
a new live microphone or end-to-end audio-to-RKLLM run. Shared command-boundary
and integrated text tests provide the action verification.

## Files and scope audit

Modified (11):

- `include/firefly/CommandEngine.h`, `src/CommandEngine.cpp`: explicit entrance
  dependency, shared target-resolution boundary and owned result metadata.
- `include/firefly/runtime/DomainView.h`, `src/runtime/DomainView.cpp`: production
  identity resolver using existing snapshot references.
- `include/firefly/runtime/ShadowComparison.h`, `src/runtime/ShadowComparison.cpp`:
  separate entrance presence/entity-set comparison.
- `include/firefly/runtime/RuntimeConfig.h`, `src/runtime/RuntimeConfig.cpp`:
  retained comparison and scoped startup diagnostic.
- `src/main.cpp`, `src/VoiceMode.cpp`: one added dependency at each assembly site;
  command loops, STT/VAD and voice state machine unchanged.
- `src/runtime/CMakeLists.txt`: three new test registrations.

Added (4):

- `include/firefly/EntranceDoorDomain.h`.
- `tests/runtime/EntranceDoorCutoverTests.cpp`.
- `tests/runtime/EntranceDoorCutoverCliTests.py`.
- This report.

The source audit against the initial checkpoint records exactly those 11 changed
tracked files and 95 unchanged tracked files. All pre-existing tests, DEV labels,
configuration/schema/foundation files, parser, SafetyGate, codec, validator and
RKLLM source are unchanged. No new dependency was downloaded.

Fresh SHA256 checks matched the accepted model, prompt, RKLLM library/header and
DEV identities. RKLLM and speech archives and Action, ActionValidator, parser,
SafetyGate, Text and InputPolicy objects are byte-identical to clean 6.5.
The VoiceSegmenter object is identical. The unchanged VoiceProcessor source
recompiles differently because it consumes the enlarged CommandResult and its
owned metadata; this is an expected C++ layout/destruction change, not a changed
speech or state-machine decision. The frozen legacy harness still has no
DomainView/ConfigLoader/entrance resolver symbols. Link/artifact evidence is kept
with the local build logs, not added to git.

Only entrance target identity was cut over. Room and brightness authority remain
config-backed, legacy comparisons cannot select the new target, and SafetyGate
remains compiled deterministic policy. No generic Action, permission activation,
prompt/context, hardware, HTTP/admin, reload, wake-word or further consumer work
was started.

Final `git diff --check` passed. Working tree: 11 modified files and four new
files listed above, no unrelated changes, no staged changes. Implementation and
report remain **uncommitted for review** at
`b7631d50fce13b109e68f91129c75596fffc2d4d`. No next milestone was started.
