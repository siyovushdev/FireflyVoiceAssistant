# Milestone 11 — generalized AI-assisted administration and typed mock execution

> Historical frontend report. The current Vue Web Admin opens at `http://localhost:8080/`; legacy vanilla assets are removed. See [B4 delivery and rollback](web-admin-ux-v2-b4.md). Backend contracts below remain supported.

This is the continuation of the existing, uncommitted Milestone 11, based on
`99c635d3534789d5c132b245ee53caf236383e12`. It is one product stage; no commit is
made. Earlier M11 results are historical, not verification of the continuation.
Typed-system continuation evidence is under `build/milestone-11/continuation/`.
The latest verification is in [Final UX pass](#final-ux-pass--2026-09-22), with
evidence under `build/milestone-11/final-ux/`. Earlier results and failures are
retained below and are not substituted for the fresh UX verification.

## Preservation and diagnosis

The initial tree had the reported 27 modified and 8 new files. HEAD, working tree,
AGENTS.md, reports 8–11, tracked diff and new sources were examined before edits.
An external recovery checkpoint containing sources, new files, diffs, HEAD,
manifest and SHA256 hashes was created at
`/home/firefly/firefly-recovery/milestone-11-20260921T071945Z`.
It excludes models, credentials and build products. No reset/clean was used.
The previous report is also retained in the continuation evidence directory as
`report-before-continuation.md`.

Already present: generalized Location metadata/hierarchy, Simple/Advanced,
initial AI suggestions and wizard, Playground, diff/history/restore candidate,
import/export, dashboard and the unified M10 runtime. These were extended, not
replaced by a new frontend or configuration product.

The concrete limitations were in the closed v1 operation enum and integer-only
parameter contract, GenericAction's old integer argument, decoder/domain checks,
legacy-only executor and context generation. Generic devices were inventory only.
The old Add Action cloned the brightness definition: renaming its label never
changed `light.set_brightness.v1`, yet the form still presented percentages.
Admin AI requested only five small metadata fields and had no description field.

The failed typo was reproduced against the prior binary with real RKLLM:
`гостиной` and `гостинной` both produced the correct living-light/55 candidate.
The decoder accepted both; **the old exact target grounding in SafetyGate rejected
`гостинной`**. This was not a token/output failure. The original request was never
rewritten. Raw baseline evidence: `baseline-results.json` and `baseline-server.log`.

The process on 8080 was positively identified as PID 177326, the previous M11
`build/milestone-11/clean-full/firefly-demo`, not guessed from its name. It was
stopped with SIGINT. `previous-process.json` preserves its exact executable and
arguments. All automated tests use separate temporary stores. The user's
`/home/firefly/.local/share/firefly-demo-m11` is not a test store.

## One configuration model, versioned compatibility

The authoritative graph remains `ConfigSnapshot / Catalog / DomainView`:

```text
Location (optional parent, descriptive type, speech forms)
  └─ Device (stable identity, location, language, allowed action IDs, state schema)
       └─ Action definition (identity, meaning, language, argument schema, assignments)
```

The C++ Location name is universal. `rooms` and `room_id` remain documented wire
compatibility spellings, not a requirement for a domestic room. Container locations
organize a tree without authorizing commands to their children. Parent selection,
cycle/reference checks and the tree remain available in the UI.

**Schema v2 is now necessary and implemented.** It adds typed arguments/state and
`mock.assign.v1`; it does not reinterpret a v1 brightness field. `migrateV2` copies
stable IDs, metadata, restrictions and legacy bindings deterministically. Loading
v1 preserves its canonical representation/hash; migration is an explicit candidate
transition, with a different semantic hash. The UI requests that transition into
its local draft, without writing the store. Save uses the existing revision/CAS,
atomic store and backups. Historical v1 revisions still load and can be restored
as candidates. `config/current-m4.json` remains the historical compatibility fixture.

Old light/entrance profiles retain their compiled semantics and protected stable
bindings. Changing a name, ID label, metadata or suggested type cannot convert a
brightness operation into power control. Unknown executable profiles, code and
hardware handlers are rejected. New operations need no compact representation:
GenericAction is authoritative; CompactV1 is only the old CLI/fixture adapter.

### Scalar contracts

Arguments and state fields support `boolean`, `integer`, `number`, `string`, and
string-valued `enum`. Each field has an ID/name, description, required flag, unit
and applicable constraints. Integers must be exact and within the schema range;
all JSON/browser integers are additionally bounded to ±9007199254740991. The C++
representation is int64, but values outside that exact browser range are rejected,
not rounded. Strings are valid UTF-8 with a code-point length bound and no control
characters. Enum choices are exact strings; there is no fallback choice.

Number means finite binary64, with at most 15 significant decimal digits at the
input boundary. Overflow, underflow and excess decimal precision are explicit
errors. Browser numeric controls apply the same boundary before `Number()` can
lose information. JSON integer notation for a declared number is normalized to
double only when exactly representable. Strings, booleans and fractional numbers
are never converted into integer arguments. Normal binary64 decimal approximation
is stated beside number fields.

Unknown/missing/extra arguments, duplicate JSON keys, wrong types, invalid enum
choices and bounds violations fail closed. Configs are at most 256 KiB; locations,
devices and actions are bounded to 64/128/32, respectively. Argument/state and
assignment lists are bounded to 16. Command input retains the 192-byte UTF-8 limit.

## Generic mock execution and Apply

The only new executable primitive is an atomic set of checked assignments:

- `field = fixed scalar`;
- `field = argument.argument_id`.

The target must exist and bind the action. Domain/argument validation and each
state-field/type/range check complete on a copy before any state is committed.
Failure leaves the entire state unchanged. Missing optional setter arguments leave
their fields unchanged. Repeated setters are idempotent; before/after remain visible.
There is no eval, script, shell, network call, GPIO or driver dispatch.

Templates are configuration data in `config/admin-templates.json`: TV power,
volume/channel; switch; conveyor running/speed/direction; numeric/enum actuators;
plus the existing light and protected door profiles. There is no TV/conveyor C++
executor. The custom-action wizard creates the same definitions without a template.
Operations requiring another algorithm/handler are unsupported, not simulated as
successful hardware control. All execution here is **virtual MOCK**.

Reconciliation policy: **preserve compatible values or reject Apply**. New devices
receive declared valid initial state; new legacy lights start at their valid minimum.
Inventory becoming executable receives explicitly declared initial state, preserving
its device ID. Removed devices disappear. A retained state with missing fields,
an incompatible state variant or values outside the new constraints rejects Apply; no
silent clamp/reset/discard occurs. State diff reports initialization/removal or the
conflict before Apply. Mock state is process-local and is initialized on restart.
Within typed state, compatibility is checked against every new field schema; an
exact integer value can satisfy a number field, while a fractional value cannot
satisfy an integer field. Adding/removing a retained device's state field rejects
instead of silently initializing/discarding just that field.

A generation owns its immutable snapshot/domain, resolver, action schemas, parser,
generated contexts and executor. A request holds that generation through inference,
SafetyGate, validation and execution. Apply waits for the runtime lease, reconciles
under that lease and atomically publishes the complete generation. HTTP/Text/Voice
share this owner. Voice still acquires its generation before STT. A failed Apply
leaves the active generation published; concurrent HTTP runtime mutations return
`runtime_busy`. No second voice or command pipeline was introduced.

## Grounding, typos and safety

Configured device aliases now participate in recognition, together with location,
explicit action phrases and permitted target/action bindings. There is no first-target
fallback. A location can disambiguate a shared device alias; otherwise two matching
TVs are Unsupported. Specific longer aliases take precedence on the same span;
separate explicit object mentions are retained and may make the request ambiguous.

The shared resolver tokenizes the original text with byte spans. Exact matches have
priority. Fuzzy matching is restricted to one insertion/deletion/substitution in one
word of a configured Location/Device phrase, with **both words at least six Unicode
letters and total phrase distance at most one**. Short aliases, digit-containing
phrases, numbered namespaces, ordinal/number words and safety/intent words are not
aggressively corrected. No transposition shortcut or undefined confidence score is
used. Equal competing matches reject. `Цех 1`/`Цех 11` and numbered pumps stay distinct.
Protected door aliases do not receive fuzzy matching.

Evidence records original span/byte offsets, configured alias, stable entity ID,
method/distance, catalog hash and request generation. SafetyGate recomputes that
server evidence against its immutable domain and the original input. A model or
HTTP client cannot supply a grounding authorization. The Playground explains, for
example, that “гостинной” matched “Гостиная”; it does not alter the transcript.
The resolver does not repair `включии`, `проццентов`, numbers, operation direction or
negation. In the full user example Qwen interprets intent/value/unit, while the
resolver grounds the location and SafetyGate checks the original command.

Existing compiled safety remains mandatory: explicit entrance opening cues in the
original input; light target grounding; contradictory on/off/value rejection;
ambiguous/multiple targets or operations; negative/alternative requests. Generic
mock operations require an exact configured action phrase, unique target and
literal argument grounding. Multiple explicit numeric values, fractional-to-integer
mismatch, invented values, unknown enum values and unconsumed numbers reject.
This is validation of virtual declarations, not a safety certification for equipment.
After the real HTTP finding below, light candidates also require a specified
brightness/state independently of Qwen: numeric/cardinal values must agree exactly,
on without a value means 100, off means 0, half means 50. “Brighter”/“comfortable”
without a level reject every candidate. The full typo example still has the explicit
`на 55` requirement, so no correction of its intent or unit spelling is needed by
this gate.
Coordinated requests cannot execute just one branch: an input containing `и` must
be completely accounted for by the conservative declared-operation parser and yield
the exact same candidate. A declared compound device name such as “панель и экран”
still works; “открой дверь и включи телевизор” does not. This check applies to the
protected door as well as generic and light candidates.

## AI drafts, real budgets and one model

Stable IDs are deterministic **transliteration**, not English translation:
Russian letters map to fixed Latin sequences, Latin is lowercased, separators
collapse, IDs are bounded to 64 ASCII characters, and occupied/retired IDs get
predictable `_2`, `_3` suffixes. Examples: `location_detskaya_komnata`,
`device_tv_dlya_detey`, `device_konveyer_podachi_2`. IDs remain editable before
creation/save; changing a saved display name does not regenerate its ID.

AI provides an editable proposal: description, descriptive type, aliases, nominative
and prepositional variants/full location phrases, and a suggested configuration
template with capabilities. Device-language generation uses the selected template's
noun from configuration data, not a per-device branch in the command prompt. The
user chooses unknown parent/location and hardware facts are not invented.

Drafts use bounded sequential identity/language/capability requests. Identity now
contains only description and descriptive type. Type follows the existing config
metadata UTF-8 string contract; it is not an ASCII stable ID or an executable profile.
The language group has six explicit slots. Its three nominative names supply aliases;
the decoder also preserves full phrases and derives mentions by removing the stated
preposition. Repeated equivalent slot values are deduplicated explicitly. This uses
one linguistic proposal instead of merging unrelated synonym lists from two model
requests. It is a declared deterministic expansion, not silent repair of malformed
JSON. Duplicate list entries, missing keys,
unknown templates or truncated JSON invalidate the proposal. All raw group outputs,
decoded fields, prompts and actual token/latency metrics are retained in diagnostics.
A malformed group does not partially save a configuration.

An action assistant uses the selected device's actual state schema, proposes checked
fixed/argument assignments, and derives editable argument types/bounds from that
schema. A bounded natural-language plan supports one new location and 1–4 devices,
allocates IDs, checks the entire graph and shows a review dialog. Both paths are
proposals only. AI provenance and explicit user confirmation are visible; requesting
replacement after manual edits requires confirmation. Manual forms/Validate/Save
remain usable when AI is unavailable.

The actual model export reports `max_context_limit: 1024`; no new export/model or
runtime context increase was made. Commands use 128 output tokens. Entity draft
groups use 256; action/graph proposals use 384. Unified runtime reserves 128
input tokens, giving system ceilings 768/640/512. The legacy command CLI retains
its existing 256-token input reservation and 640-token system ceiling. All admissions use vendor token
metrics. Admin empty bootstrap generates only one discarded token; the established
full-clear → set template → empty bootstrap → keep-system clear lifecycle remains.
Every real request clears conversation KV while retaining its system context.
Admin inference has a 60-second deadline, commands retain their existing deadline.
Vendor FINISH and token counts are recorded; the SDK does not expose a more specific
EOS reason, so the report does not invent one.

One heavy RKLLM instance serves all tasks. Instructions/output budgets switch under
the same runtime lease and restore afterward. Acquisition waits at most 45 seconds;
HTTP accepts at most 16 concurrent clients and overlapping runtime mutations fail
busy. There is no persistent serialized prompt/KV cache.

A full v2 catalog need not fit in a single 1024-token request. The server first
resolves the target from the complete catalog, then an exact generic action phrase.
The model receives the **complete selected binding** (target language/location,
action meaning, typed arguments/constraints and assignments), with exact stable IDs
in its output shape. Protected legacy target contexts retain their existing policies.
All possible scopes are measured/admitted before Apply. No scope is silently cut.
Successful admission metadata is bounded to 256 prompt strings; it stores neither
model output nor KV. The full catalog context is an Advanced diagnostic, bounded to
64 KiB; individual prompts are bounded to 16 KiB and the stricter real token ceiling.
The actual context used by an RKLLM command is included in command diagnostics.

## UI and user flow

The existing dependency-free HTML/CSS/JS UI remains. Desktop/tablet navigation has
zones/tree, devices, actions, Playground, states, revisions and diagnostics.
Simple uses names, forms, meanings, before/after states and human errors. Advanced
adds stable IDs, raw schemas, revision/generation/hash, build source fingerprint,
actual context/raw model output, typed GenericAction, stage results and timings.
Stages not reached display no action, rather than PASS.

To configure a TV: create/select its zone, enter its name, request AI if useful,
review aliases and choose the TV template. Review power on/off, integer volume
0..100 and channel 1..999, and the initial virtual state. Confirm the draft, Validate,
Save, inspect diff, then Apply. An existing inventory TV can be configured with the
same stable ID via its template button. Add Action opens a genuine typed action
wizard; it no longer silently clones brightness. Percent controls appear only for
percent fields/the explicit legacy light profile.

Preview uses the production understanding/validation pipeline and previews the same
executor's assignments without changing state or execution history. Execute invokes
that executor and records before/after and the event. Import and revision restore
only populate a candidate; Validate/Save/Apply remain explicit. User/AI/imported
strings are rendered with textContent and DOM controls, not unescaped HTML.

Build assets are copied into each CMake build. A binary therefore serves its own
UI snapshot instead of silently mixing with edited source assets. Advanced/status
exposes HEAD, build time and source SHA256 to identify the running version.

## Verification ledger

No earlier green result is substituted for the final clean/regression runs.
All intermediate failures remain
in the local evidence tree:

- Initial core run: 18/27, including an overbroad numbered-alias guard and version
  expectation/startup issues. Fixed; subsequent intermediate core 28/28.
- Full build initially caught an overlong C++ raw-string delimiter; fixed.
- Early AI grammar output repeated entries or included misplaced prepositions;
  strict decode rejected malformed proposals. Raw outputs are retained.
- A full catalog with TV initially used 881 system tokens, beyond 768: admission
  rejected it and retained generation 1. Complete target/action scopes replace that
  oversized inference strategy; no truncation was introduced.
- `typed-probe-2` and `typed-ui-1`: model selected power-on for a channel command;
  SafetyGate rejected it. Zero state changes from that candidate. Configured action
  resolution and scoped contexts address the ambiguity.
- `typed-ui-2`: TV, both typo commands, ambiguity and all conveyor actions passed.
  Qwen misspelled the custom action ID (`obrazats` instead of `obrazets`); the strict
  decoder rejected it. The generated response shape now includes exact stable IDs;
  decoder repair remains forbidden.
- `full-8`: 39/40; a legacy CLI diagnostic string changed while rejection remained
  correct. Restored the CLI message, retaining separate human UI reasons.
- `full-9`: 40/40, including typed mock, configuration/runtime/admin/execution and
  task-budget/KV tests. Final clean verification and live results are recorded below.
- `typed-ui-3` passed TV, both typo requests, ambiguity, conveyor, a custom string
  action through real Qwen, and an AI action draft. It did not yet include the
  multi-entity-plan browser assertion and is not claimed as that proof.
- `final-ui-typed` passed those operations but rejected the initial natural-language
  plan: zone aliases included device concepts, location phrases were inconsistent,
  and fan suggestions used the conveyor template. No candidate was saved/applied.
  The admin plan instructions now specify zone/device separation, paired location
  phrases and the supported template meanings. Command instructions were not tuned.
- `plan-probe-1`: the ventilation-zone wording produced the correct kinds but an
  extra closing `}`; strict decoding rejected it without repair. The original task
  example, “Добавь цех окраски с двумя вентиляторами и освещением”, produced a valid
  location, light and two switch-template fans. Both raw results are retained.
- **`accepted-http-typed`: actual incorrect mock execution**, not a false reject.
  It completed 36 DEV cases (35 correct) before stopping at `ambiguous04`:
  “На кухне сделай свет ярче”. Scoped Qwen emitted brightness 100, the old gate
  passed it, and the isolated test kitchen changed **40 → 100**. The remaining 12
  cases and concurrency/shutdown checks did not run in that failed attempt.
  Raw output, before/after and all completed results remain in
  `accepted-http-typed/evidence.json`. No user-store state was involved.
  The cause was a real gap: the old gate checked numeric contradictions only when
  an on/off/half/maximum word existed and did not require a specified brightness.
  SafetyGate now validates numeric requirements independently and requires an
  explicit value/state for light candidates; unqualified on remains 100, off 0.
  Relative/unspecified phrases cannot obtain an invented value from the model.
  Added direct all-0..100 candidate tests for unresolved/invalid inputs in both v1
  and v2, plus numeric/cardinal/on/off controls. DEV labels and command prompts
  were not changed. Fresh clean builds and every final live suite follow this fix.
- `verified-http-typed` then passed the pipeline/48 DEV/lifetime/shutdown assertions,
  but manual raw-output review caught a **semantic AI draft failure**: “Ванная” got
  the identity-group example's pump description/type/aliases while its language
  group correctly described a bathroom. Nothing was saved/applied. The initial
  smoke assertion checked structure/completeness, not enough semantic relevance;
  it now explicitly checks this acceptance input. `verified-ui-typed` was deliberately
  interrupted at 148.42 s to address this finding; its partial evidence and reason
  are retained, not counted as a completed UI pass.
- `identity-probe-1`: removing the concrete pump example stopped that copying, but
  the independent alias request generated awkward synonyms and a mixed-script
  `ванный室`. The final contract instead derives aliases from the same six-slot
  language group used for mentions/location phrases. No bad raw JSON is repaired.
- `identity-probe-2`: description/type-only output returned `type: "ванная"`; the
  draft decoder rejected it because of an unnecessary ASCII restriction. The real
  configuration already permits UTF-8 descriptive types, so the decoder contract
  was aligned with it. This does not relax stable-ID rules, scalar schema type
  enums or executable profile validation. A deterministic test covers the distinction.
- A subsequent direct-candidate audit reproduced another gate gap: a door candidate
  could select only its branch of “Открой входную дверь и включи телевизор в детской”.
  `coordinated-candidate-test.log` records that failing assertion (no executor was
  called for that candidate). The gate now requires complete conservative parsing for coordinated
  input; direct tests cover door/TV/light partial candidates, an unknown second
  object and a valid compound entity name. `coordinated-fix-test.log`: 1555 checks.
- `review-http-typed` stopped during startup, before any command: its old helper
  assumed an error body was JSON and lost the original status/body. The environment
  had HTTP/HTTPS proxies on loopback port 10809 and no bypass for `127.0.0.1`.
  The exact error body is unavailable, so its origin is not asserted retrospectively.
  Local HTTP smoke now explicitly bypasses environment proxies and records raw
  non-JSON errors instead of hiding them behind a JSON parser exception. The
  production server already emits JSON error bodies. No runtime generation or
  state failure was observed in this incomplete startup attempt.

### Baseline AI measurement

The old API returned only `type`, `aliases`, `mentions`, `locations`, `capabilities`.
For “Детская комната” it produced two aliases, one mention and one location phrase,
without a description (19,508.6 ms API-reported latency). For “ТВ для детей” it
produced two aliases, no mentions or location phrases, and four advisory capability
names (19,811.4 ms). Its response budget was 128 tokens. The old response did not
expose actual token counts; no retrospective counts are invented.

The new grouped requests record system/input/output token counts and inference
latency separately from end-to-end draft latency. A larger draft is intentionally
slower: it includes several independently decoded field groups and context switches.
Neither the 256/384 output budget nor the number of groups increases the export's
1024-token context. Malformed output is an error even when generation finishes below
the token limit. No truncated JSON or heuristic completion is accepted.

### Reproducible clean builds

The final build directories were configured independently and rebuilt with
`--clean-first` after the last production change:

```sh
cmake -S . -B build/milestone-11/continuation/verified-core \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build/milestone-11/continuation/verified-core --clean-first -j3
ctest --test-dir build/milestone-11/continuation/verified-core --output-on-failure

cmake -S . -B build/milestone-11/continuation/verified-full \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_ASSISTANT=ON -DFIREFLY_BUILD_VOICE=ON \
  -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DSHERPA_ONNX_ROOT="$PWD/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu" \
  -DSHERPA_ONNX_INCLUDE_DIR="$PWD/build/stt/vad-ab"
cmake --build build/milestone-11/continuation/verified-full --clean-first -j3
ctest --test-dir build/milestone-11/continuation/verified-full --output-on-failure
```

Before the additional brightness safety fix, clean core was **28/28** (25.07 s)
and full **40/40** (30.24 s).
Logs: `final-core-build.log`, `final-core-tests.log`, `final-full-build.log`,
`final-full-tests.log`. This includes TypedMockConfiguration (110 checks),
GeneralizedAdmin (77 checks), RkllmContextRefresh (17 checks), the existing
configuration/foundation/structured decoder/unified runtime/execution suites,
HTTP transport tests and the JS draft model tests. No compiler warnings were found.

The new tests cover all scalar types, migration/reload/hash/metadata/stable identity,
state schemas/bindings/atomic rejection, optional/missing/extra arguments,
enum/overflow/precision boundaries, direct rejected SafetyGate candidates,
numbered/short/ambiguous/fuzzy grounding, forged evidence rejection, generation
leases/reconciliation, preview immutability, manual fallback and structured AI
drafts. Empty string arguments require an explicitly quoted empty literal; a
device alias cannot consist solely of control/intent words.

The unchanged v1 generated system prompt is byte-identical to the M10 artifact:
SHA256 `a8ffb93d4111edb39f6a8d483d6b0386e17f6b4ec2ea372c524e5820779ba80c`.
DEV SHA256: `b9a882c4ea6f7c0b956ca47ad4d2ff320fbdf4eabecb5a378449e34efd23dff0`.
Historical current-M4 fixture SHA256:
`110e92d62e677eba03caec2bdc131bfec0203d7c462971af5c07636ccc4e1133`.
No DEV labels, model, STT/VAD configuration or frozen prompt-v5 file were changed.

### Typed Chromium and real AI evidence before the final safety fix

`accepted-ui-typed`: PASS, 413.98 s, PID 243316, no JavaScript exceptions.
The single process published generations 1 → 2 → 3 → 4 → 5. Preview left both
state and execution history byte-for-byte unchanged. Every rejected test also
checked unchanged state/history. Its SIGINT exit was 0.

| UI request | Actual result |
|---|---|
| TV on/off | `power: false → true → false` |
| TV volume/channel | `volume: 0 → 35`, `channel: 1 → 7` |
| Volume 120 / channel 1000 | Unsupported; no execution |
| Both `гостинной` examples | `device_living_light`, brightness 55; RKLLM + distance-1 location evidence |
| Two TVs / contradictory on+off | Unsupported; no first-target choice |
| Conveyor | `running: false → true → false`, `speed: 0 → 45`, `direction: forward → reverse` |
| Custom UI action | `action_pometit_obrazets`, string `label: "" → "готово"`; real RKLLM |
| Multi-entity AI plan | Paint workshop, light, two switch-template fans; reviewed and validated as candidate, stored hash and generation unchanged |

The custom sample/action/state were created through forms with no template for that
operation and no C++/prompt modification between configuration and execution:

```json
{"actionId":"action_pometit_obrazets","targetId":"device_obrazets_upakovki","arguments":{"label":"готово"}}
```

Actual entity-draft measurements from this run (tokens are vendor counts; group
latency excludes surrounding context switches, total UI wall time includes them):

| Input / group | System / input / output tokens | Output budget | Inference ms | Total draft ms |
|---|---|---|---|---|
| Детская комната / identity | 337 / 18 / 38 | 256 | 6480 | 28357 |
| Детская комната / language | 374 / 18 / 49 | 256 | 8322 | 28357 |
| ТВ для детей / identity | 337 / 16 / 34 | 256 | 5810 | 36747 |
| ТВ для детей / capabilities | 223 / 16 / 18 | 256 | 3184 | 36747 |
| ТВ для детей / language | 435 / 16 / 70 | 256 | 11906 | 36747 |
| Цех упаковки / identity | 337 / 19 / 44 | 256 | 7509 | 27862 |
| Цех упаковки / language | 374 / 19 / 74 | 256 | 12539 | 27862 |
| Конвейер подачи №2 / identity | 337 / 22 / 44 | 256 | 7505 | 36473 |
| Конвейер подачи №2 / capabilities | 223 / 22 / 34 | 256 | 5763 | 36473 |
| Конвейер подачи №2 / language | 435 / 22 / 73 | 256 | 12564 | 36473 |
| Action draft | 275 / 20 / 67 | 384 | 11186 | not separately measured |
| Paint-workshop plan | 438 / 33 / 120 | 384 | 20479 | not separately measured |

All these generations ended with vendor FINISH below the assigned output limit.
The child draft now has a description, type, three aliases, nine mention forms
and three location phrases; TV has a description, type, three aliases, six mention
forms and an editable TV template with four actual mock actions. AI capability
names remain advisory: e.g. `SetParentalControl` was suggested but was **not**
registered as an executable operation. The wizard explicitly labels such ideas
as suggestions and separately lists actual action definitions/state effects.

Raw child-room language output:

```json
{"name":"детская комната","name_location":"в детской комнате","short":"детская","short_location":"в детской","synonym":"комната для детей","synonym_location":"в комнате для детей"}
```

Full raw identity/language/capability outputs, decoded drafts, instruction text,
metrics and final form configuration are in `accepted-ui-typed/evidence.json`
and `accepted-ui-typed/review-config.json`. The latter is a review artifact, not
the user’s active store. The AI action and multi-entity plan include their own raw
JSON and decoded proposals in the same evidence file.

Desktop/tablet captures: `accepted-ui-typed/simple-overview.png` and
`tablet-overview.png`; Playground/custom action and Advanced captures are in that
directory. Chromium surface capture omitted sticky navigation on some scrolled
images; an independent displayed-window capture (`sidebar-display-probe.png`) and
DOM geometry confirmed that navigation remained visible. The capture helper now
uses the displayed window (`fromSurface:false`); production CSS was unchanged.

### Existing Web Admin regression before the final safety fix

`accepted-ui-general`: PASS, 168.81 s, PID 245647, no JavaScript exceptions.
The original Factory/Bathroom workflows were rerun against that intermediate binary:
Factory → Packaging Workshop hierarchy, editable AI draft, wizard, Validate/Save/
Apply, preview and execution. “В цехе упаковки поставь освещение на 60 процентов”
resolved to `device_packaging_light`, brightness **0 → 60**. Bathroom retained
`ванная`/`ванна`, with **0 → 40 → 30** on generation 3. Generation 2/3 stayed in
the same process. The contradictory kitchen request was Unsupported.

This suite also exercised both UI modes, human diff, revision list, view diff,
restore-as-candidate, versioned downloadable export, import-as-candidate, dashboard
and Advanced diagnostics. Restore/import did not automatically Apply. Evidence and
screenshots: `accepted-ui-general/` (`factory-playground.png`,
`advanced-diagnostics.png`, `evidence.json`).

`accepted-ui-offline`: PASS, 1.92 s. In actual Chromium with the model absent, the
AI error was visible while manual creation, Validate, Save and stable ID after rename
continued to work. Initial Apply showed its diff and failed closed at model admission;
no pretend active generation was reported. Screenshot: `accepted-ui-offline/offline-manual.png`.

### Verification after the brightness safety fix, before final follow-up fixes

Fresh `--clean-first` builds after the last production change:

- Core **28/28**, 25.75 s (`post-safety-core-build.log`, `post-safety-core-tests.log`).
- Full **40/40**, 30.17 s (`post-safety-full-build.log`, `post-safety-full-tests.log`).
- TypedMockConfiguration **1548 checks**, including injected candidates independent
  of model behavior; GeneralizedAdmin **77**; RkllmContextRefresh **17**.
- No compiler warnings. Build/source/asset fingerprint was recalculated after the fix.

`verified-http-typed` completed 48/48 DEV with zero false rejects/wrong results,
preserved state/history for all Unsupported cases, rejected oversized context with
422 and kept generation 1. A concurrent Apply returned 409 while the old request
completed on generation 1; Apply then published generation 2 and the next request
preserved/updated the string state. One RKLLM load/destroy, SIGINT exit 0 in 6.29 s.
Its semantic Bathroom draft issue and deliberately interrupted UI follow-up are
recorded above. Final outcomes after all follow-up fixes are reported separately.

### Final AI contract probe

`identity-probe-3`: all **5/5** requested drafts decoded and the explicit Bathroom
semantic control passed. This probe uses the final production AI instructions and
one real local Qwen instance; it saves/applies/executes nothing. All raw prompts,
outputs, decoded drafts and metrics are retained in `identity-probe-3/results.json`.

| Name | Aliases from the language proposal | Type metadata | Description | Draft wall ms |
|---|---|---|---|---|
| Ванная | ванная, комната для душа | ванная | Зона для использования ванной комнаты. | 23340 |
| Детская комната | детская, детская комната, комната для детей | детская зона | Зона для развлечения и отдыха детей | 23201 |
| ТВ для детей | детский телевизор, телевизор для детей | ТВ для детей | ТВ для детей используется для предоставления контента, подходящего для маленьких детей. | 33240 |
| Цех упаковки | цех для упаковки, цех упаковки | цех упаковки | Зона для упаковки товаров и их подготовки к отправке. | 28834 |
| Конвейер подачи №2 | конвейер подачи, конвейер подачи №2 | конвейер подачи | Конвейер подачи №2 используется для транспортировки изделий по линии производства. | 37876 |

| Name / group | System / input / output tokens | Output budget | Inference ms |
|---|---|---|---|
| Ванная / identity | 200 / 15 / 25 | 256 | 3950 |
| Ванная / language | 374 / 15 / 50 | 256 | 7964 |
| Детская комната / identity | 200 / 18 / 25 | 256 | 3988 |
| Детская комната / language | 374 / 18 / 49 | 256 | 7807 |
| ТВ для детей / identity | 195 / 16 / 34 | 256 | 5374 |
| ТВ для детей / capabilities | 223 / 16 / 18 | 256 | 2923 |
| ТВ для детей / language | 435 / 16 / 70 | 256 | 11106 |
| Цех упаковки / identity | 200 / 19 / 37 | 256 | 5786 |
| Цех упаковки / language | 374 / 19 / 74 | 256 | 11658 |
| Конвейер подачи №2 / identity | 195 / 22 / 45 | 256 | 6976 |
| Конвейер подачи №2 / capabilities | 223 / 22 / 34 | 256 | 5366 |
| Конвейер подачи №2 / language | 435 / 22 / 73 | 256 | 11721 |

Final identity instructions use **200 tokens for locations / 195 for devices**,
down from 337, and ask only description/type. Language remains 374/435 tokens;
capability proposals use 223. This removes a redundant synonym-generation task,
not fields from the finished draft. The final child draft includes the three
requested aliases, nine mention forms and three full location phrases. TV includes
both “детский телевизор” and “телевизор для детей” plus their prepositional forms.

Description quality remains a limitation: Bathroom’s description is generic, and
the TV proposal unnecessarily narrows “children” to “small children”. These texts
are editable suggestions, not facts about installed equipment or safety properties.
The final alias source avoids the earlier unrelated pump/mixed-script synonym list;
it cannot guarantee perfect semantic generation for every future name.

## Final verification of the complete implementation

After the final coordination-policy and AI-contract changes, both builds were
rebuilt with `--clean-first`:

- `complete-core-tests.log`: **28/28**, 25.75 s.
- `complete-full-tests.log`: **40/40**, 30.84 s.
- TypedMockConfiguration **1555** checks; GeneralizedAdmin **77**;
  RkllmContextRefresh **17**; existing MockExecutionUnifiedRuntime **127**.
- No compiler warnings; source fingerprint and all five build UI assets match.

`complete-http-typed`: **PASS**, 431.13 s. This final suite includes the strengthened
Bathroom semantic control, real admin → light → TV via RKLLM → unsafe → admin →
custom action sequence, and **48/48 DEV, 0 false reject, 0 wrong/unsafe**. Door+TV
and door+light combined requests also reject with state/history unchanged. The model
still proposed 100 for the relative-brightness case; SafetyGate rejected that actual
candidate, so this is a gate proof rather than reliance on a model refusal.

Oversized scope admission returned 422 and preserved generation 1; the next real TV
command still worked. Apply during a real custom-action request returned 409/busy;
the request executed on generation 1. A subsequent Apply published generation 2,
and the next request preserved `label: "проверено"` before changing it to `"готово"`.
The PID stayed unchanged, with one RKLLM load and one destroy. SIGINT during a real
request exited with code 0. Full raw stages, state/history comparisons, token counts
and shutdown timing are in `complete-http-typed/evidence.json` and `server.log`.

`complete-ui-typed`: **PASS**, 369.81 s, PID **272361**, **0 JavaScript exceptions**,
SIGINT exit 0. The final Chromium run created all acceptance entities through the
forms, using real local AI and a separate store:

| UI scenario | Observed result |
| --- | --- |
| Child location AI | Description/type, 3 aliases, 9 mentions, 3 location phrases; editable, not saved/applied |
| TV creation and Apply | Four real typed mock actions, generation 1 → 2, same PID |
| Preview | No state or execution-history change |
| TV commands | `power: false → true → false`, `volume: 0 → 35`, `channel: 1 → 7` |
| Invalid volume/channel and contradictory command | Rejected, state/history unchanged |
| `гостинной` and full `включии … проццентов` variant | Living light 55, server matching evidence visible |
| Second TV | Generation 3; insufficiently specified target rejected |
| Packaging workshop/conveyor | Generation 4; running true, speed 45, direction reverse, running false |
| User-defined string action | Generation 5; real RKLLM produced `label: "готово"`; no template-specific executor |
| AI action proposal | Typed argument/assignment inferred from selected device schema; cancelled without saving |
| Multi-entity description | Paint workshop, lighting and two fans; valid editable candidate only, then discarded |

The completed UI draft used `детская`, `детская комната`, `комната для детей` and
their location forms. TV aliases were `детский телевизор`, `телевизор для детей`;
the useful mentions include `на детском телевизоре`. The model also suggested
`SetParentalControl` as an advisory capability, but it was **not registered or
executable**: the reviewed template contained only its four declared operations.
This distinction is displayed in the wizard.

The final UI AI calls used 18–120 output tokens, all ending below their 256/384
task limits. Entity system prompts were 195/200 (identity), 223 (capabilities),
374/435 (language) tokens. The custom-action proposal used 275 system, 20 input
and 67 output tokens (10,393 ms inference); the multi-entity proposal used
438 system, 33 input and 120 output tokens (19,156 ms inference). These are vendor counts, not estimates
from string length. `complete-ai-summary.json` collects the raw/decoded final UI
proposals and per-group metrics without the repeated active-config payloads.

Final raw AI instructions/output, group token counts, decoded drafts, configuration
diffs, command stages and before/after states are in
`complete-ui-typed/evidence.json`; the resulting review fixture is
`complete-ui-typed/review-config.json`. Screenshot evidence in the same directory:
`simple-overview.png`, `tablet-overview.png`, `tv-playground.png`,
`typo-evidence.png`, `conveyor-mock.png`, `custom-action.png`,
`advanced-typed-context.png`, and `multi-entity-draft.png`. The screenshots were
visually checked: navigation remains visible at desktop/tablet sizes, command
meaning and virtual state are readable, and the multi-entity proposal explicitly
states that it is not saved.

`complete-ui-general`: **PASS**, 164.93 s, PID **274340**, SIGINT exit 0.
Factory → Packaging Workshop → Packaging Light **0 → 60** and Bathroom Light
**0 → 40 → 30** work after live Apply. The suite also exercised Simple/Advanced,
hierarchy/parent selection, AI/wizard, Preview/Execute, config diff, revision history,
restore-as-candidate, file export/import and status/diagnostics. Evidence and
screenshots are in `complete-ui-general/`.

`complete-ui-offline`: **PASS**, 1.92 s. With no model, AI reports unavailable while
manual forms Validate/Save and stable-ID rename work. Initial Apply presents a diff
and model admission fails closed. Evidence: `complete-ui-offline/`.

### Final NPU and integrated DEV regression

The six sequential runs in `final-hardware/` used the final verified binaries,
unchanged `tests/data/dev.tsv`, `config/current-m4.json` and the fixed local model.
No run was replaced with a later success:

| Run | Exact result | False reject | False accept / wrong action | contradiction02 |
| --- | --- | --- | --- | --- |
| 01 NPU | 48/48 | 0 | 0 / 0 | U |
| 02 Integrated | 48/48 | 0 | 0 / 0 | U |
| 03 NPU | 48/48 | 0 | 0 / 0 | U |
| 04 Integrated | 48/48 | 0 | 0 / 0 | U |
| 05 NPU | 48/48 | 0 | 0 / 0 | U |
| 06 Integrated | 48/48 | 0 | 0 / 0 | U |

Each run had 17 deterministic and 31 fallback cases. Among fallbacks, one
structurally invalid result was rejected by the decoder and three wrong model
candidates were rejected by SafetyGate. Thus 48/48 describes the complete guarded
pipeline, not perfect raw Qwen output. Every process exited 0. Per-run stdout,
stderr, parsed stage traces, exact commands, model/binary hashes, runtime environment,
temperature/frequency observations and timings are preserved in
`final-hardware/hardware-summary.json`, `hardware-artifacts.json` and the six
numbered run files. The HTTP suite independently verified that Unsupported cases,
including contradiction02, leave actual mock state and execution history unchanged.

### Final speech replay and physical voice limitation

All existing applicable replay suites completed on the same final build:

| Suite | Speech | Silence | Exit | Wall time |
| --- | --- | --- | --- | --- |
| `speech_replay_tests` | 8/8 | 2/2 | 0 | 10.89 s |
| `generic_speech_replay_tests` | 8/8 | 2/2 | 0 | 49.88 s |
| `unified_speech_replay_tests` | 8/8 | 2/2 | 0 | 60.68 s |

Transcripts matched the existing expectations. Silence produced zero utterances
and commands. Unified replay then applied a Bathroom configuration, retained kitchen
state 40, published generation 2 with six devices and one model instance, and ran
the next voice-pipeline request against generation 2. Full diagnostics are in
`final-hardware/unified-speech-replay.stdout.log`; each suite has its exact command,
exit code and timing JSON alongside stdout/stderr. The complete live orchestrator
exited 0 (`complete-live-summary.json`).

This is recorded-audio verification. **Live H340 capture after Apply was not
performed because H340 is physically absent from USB/ALSA**, as recorded in
`usb-final.txt` and `alsa-final.txt`. No implementation failure or live microphone
success is inferred from that absence.

Across the final HTTP/UI/DEV/replay runs there were **zero wrong/unsafe executions**.
The earlier incorrect mock execution and all other failed/interrupted attempts
remain explicitly recorded above; this final statement does not erase that history.

## Final UX pass — 2026-09-22

This continues the same uncommitted M11; it does not replace the typed runtime.
The previous 64-file source manifest matched byte-for-byte at pre-check. HEAD is
still `99c635d3534789d5c132b245ee53caf236383e12`. Recovery sources, the complete
tracked diff and the untracked files were preserved outside the repository at
`/home/firefly/firefly-recovery/milestone-11-final-ux-20260922T073324Z`.
The exact prior listener (PID 278227, continuation/verified-full/firefly-demo) was
stopped with SIGINT. All new smoke work uses temporary stores. Evidence for this
pass is under `build/milestone-11/final-ux/`; older “final” results above describe
only the previous continuation, not this new pass.

The final comparison with this recovery manifest is recorded in `ux-pass-delta.json`:
**16 existing files changed and 3 added during this UX pass** (the complete M11
review contains 67 files). Typed schema/migration, GenericAction/scalar decoder,
DomainView/entity resolver, SafetyGate and MockDeviceExecutor match their resumed
WIP hashes exactly. The existing universal system was preserved. Backend edits
are limited to admin draft/retry handling and the evidence-driven generated-context
correction described below; no per-device executor was added.

### Simple and Advanced are separate editing flows

Simple renders human questions and descriptions, not the low-level action editor.
The TV wizard expands its template, lets the user select four capabilities, shows
power on/off effects and editable volume/channel limits, and supplies valid initial
state. No argument keys, field keys, assignment sources or profiles are required.
Existing cards show “Что умеет устройство”, “Допустимые значения” and human state
names/values. The brightness-only add button is replaced by “Добавить действие”
and “Добавить ограничение”. The latter selects an action or device characteristic.

The custom Simple wizard asks for the action name, device, intent phrases, whether
the user supplies a value, its kind/bounds/unit and what should change. It compiles
these answers to the same `mock.assign.v1` definitions. New properties receive
stable, collision-checked local keys (`value_1`, `value_2`, ...); there is no second
runtime model. Advanced retains the existing exact state/argument/assignment editor
and raw schemas. Complex multi-assignment actions can be inspected in Simple and
edited in Advanced. Low-level fields are not constructed in the Simple action flow;
this is more than CSS hiding of the old editor.

Templates remain data in `config/admin-templates.json`. TV, switch, conveyor,
numeric and enum templates expand into ordinary typed state/actions; custom starts
with no invented capability. Template descriptions are human-readable. Light/door
retain their compiled protected profiles: no new execution route, alternate door
binding or changed light contradiction semantics was introduced. Scalar validation,
reconciliation, entity grounding, typo thresholds and the command pipeline are
unchanged by this UX pass.

Generic constraints include integer/number bounds, units, enum options, string
length and required/optional command values. Simple represents `%` as a familiar
unit; Advanced retains exact `percent` schema data. Backend validation remains
mandatory. Updating a candidate's initial value does not reset active mock state.

### AI proposals and bounded retry

Existing grouped full drafts still supply description/type, aliases, grammatical
forms and useful template suggestions. Template expansion fills action/state schemas
and argument bounds deterministically. Original device names are also retained as
normalized aliases. Unknown parents/hardware details remain for manual selection.
AI action suggestions are checked against the selected device's actual state.

Every public admin suggestion endpoint now strictly decodes each response before
accepting it. A failed parse/schema/graph validation permits **one automatic retry
per user request**. For grouped drafts this allowance is shared across groups;
successful groups are not regenerated. Retry requests a **complete JSON response
in the same schema**, with the original user input and an explicit syntax reminder.
It never strips markdown, trims extra braces, extracts a JSON substring or repairs
invalid output. All attempts retain prompt/input/raw output, decode error or decoded
value, actual tokens, finish status and latency. If a later group still fails,
there is no accepted partial draft. Neither attempt performs Save/Apply/Execute.

The shared generation lease covers all attempts and restores the command context
before release. One model instance, serialized inference and the existing KV
lifecycle remain. Model failure/unavailability is not retried as a formatting error.
Manual editing remains available. Simple gives a useful retry/fallback message;
Advanced shows every attempt in the persistent latest-AI diagnostics block.

The multi-entity contract is bounded to **one location and 1–4 devices**. AI emits
names, descriptions, aliases and template IDs; the backend allocates IDs and expands
actual typed actions/state from those templates, then validates the complete graph.
The preview shows descriptions and supported effects before candidate confirmation.
No save or apply happens automatically.

### Actual malformed JSON and budgets

The original reported failure was reproduced against the previous verified binary
with a separate store and the exact request:
“Добавь зону упаковки, освещение, два конвейера и вентилятор”. The response had an
extra final `}`: HTTP 422, **438 system / 38 input / 162 output tokens**, output
budget **384**, vendor FINISH below limit, inference **25,907 ms**. This is a syntax
failure, not truncation. `baseline-ai.json` contains the complete raw response and
error; `baseline-server.log` contains the actual runtime diagnostics.

Budgets remain measured and separate: model export **1024** context; commands
**128** output, entity groups **256**, action/plan proposals **384**. No blanket
increase, model replacement or larger runtime context is used. Retry instructions
are admitted by the real tokenizer within the same context reservation. Final
measurements and results are recorded below when completed.

### Executor contract and future binding boundary

Advanced Playground has “Результат команды для исполнителя”: the exact canonical
`{actionId, targetId, arguments}` GenericAction, including typed scalar values.
No artificial `version` member is inserted into the existing semantic model just
for presentation, and Unsupported has no executable contract. Preview shows the
same predicted before/after without changing state/history; Execute uses the
existing mock executor.

Every device has an Execution section. **Mock is available** and means only local
virtual-state changes. Advanced lists HTTP, MQTT, TCP, UDP, RS-485, CAN, GPIO and
Custom adapter as visibly disabled **Planned** choices. No binding is stored or
selected, and no network/hardware executor was added. A future adapter would
translate GenericAction to a protocol; protocol-specific fields are absent from
GenericAction and this configuration change.

### Verification and attempt ledger for this pass

- Baseline helper initially failed before startup because its temporary store
  directory had not been created. Fixed the helper; both logs retained under
  `baseline-startup-failed-*`. No model inference or user-store change occurred.
- First core build/test: **29/29**.
- First full test: **40/41**. Switching the legacy admin endpoint to the common
  retry path exposed a stale model-ready flag on early unavailable return, causing
  offline validation to try runtime admission. Both suggestion paths now update
  readiness before returning unavailable. `full-tests-2.log`: **41/41**;
  `full-tests-3.log`: **41/41**. The failed run remains recorded.
- `ui-1`: real child/TV AI, all four TV commands, preview immutability, invalid
  volume, contradiction02, both typo requests and the Simple custom number action
  succeeded. It stopped on a test assumption that the first constraint selector
  entry was TV; canonical ordering put the new doser first. The test now explicitly
  selects TV. Raw results/screenshots and exit-0 shutdown are retained; this is not
  counted as a completed UI pass.
- Screenshot review corrected duplicated boolean effect text, stale live range
  labels and visibility of disabled planned bindings. These presentation changes
  are included in subsequent verification.
- `ui-2`: **PASS**, PID 512244, no JavaScript exceptions; SIGINT exit 0.
  Child/TV drafts were complete and required no retry. TV on/off/volume/channel,
  preview immutability, both typo commands, Simple custom number action, four-device
  plan, mode separation, executor contract, disabled planned bindings, human errors,
  diff and revision history all passed. The extra plan stayed a candidate; its
  confirmation did not change stored hash or active generation.
- In `ui-2`, the new four-device plan used **386 system / 38 input / 186 output**
  tokens at budget **384**, **29,470 ms** inference, vendor FINISH below limit.
  Its complete JSON passed on attempt 1. Child identity/language used 200/374 system,
  18 input, 25/49 output tokens; TV groups used 195/223/435 system, 16 input,
  34/18/70 output. End-to-end drafts took 23,434 ms and 33,389 ms respectively.
- A direct retry-context admission test preserves the first malformed response and
  records the skipped second inference with null metrics. It never invents a PASS
  or starts another formatting retry after a context/runtime failure.
- Final `verified-http`: DEV **48/48**, zero false rejects/wrong/unsafe DEV actions.
  The subsequent generation-continuity scenario stopped after Apply at generation
  2: `Пометь образец "готово"` returned correct IDs but `arguments:{}`. Strict
  decoder reported `invalid_arguments`; no execution occurred. Generation 1's
  preceding in-flight request had successfully stored `label="проверено"`, and
  concurrent Apply had returned `runtime_busy`. This is a **false reject outside
  DEV**, not an execution with an invented/default value. The test assertion then
  hit a null execution; the SIGINT-during-inference part was not reached. The
  process was cleaned up by its finally block. Raw output and stage evidence are
  retained in `verified-http/` and `verified-http-failure-summary.json`. A second
  complete, unchanged HTTP run is recorded separately, never replacing this run.
- `verified-http-repeat`: the unchanged complete run again scored **48/48 DEV**,
  then reproduced the same missing-argument false reject after Apply. Both failures
  are retained. Source inspection found a contradictory generated instruction:
  every selected action advertised `arguments:{}` as its **exact response**, even
  when its schema required a string. This was corrected generically: parameterized
  actions now get exact identity plus the required key list from their schema and
  an explicit ban on empty arguments when required keys exist. No-argument actions
  keep their valid empty-object contract. No entity IDs, values or special cases
  were added to C++; v1/frozen prompts and SafetyGate are unchanged.
- Ten deterministic checks cover parameterized versus empty contracts, all scalar
  keys, invalid action/target binding and stable instructions after rename.
  Focused structured/context/typed tests: **4/4**.
- `context-fix-probe`: **PASS** with real RKLLM. The original command extracted
  `label="готово"`, an in-flight request set `label="проверено"` on generation 1,
  concurrent Apply returned busy, and the identical command after rename/Apply
  used generation 2 and changed retained state back to `"готово"`. Clean SIGINT.
  The full suite is rerun on new clean `rechecked-core/full` builds below; the
  preceding successful runs are not presented as verification of this correction.
- The old scheduler was stopped by its exact owned PID 524779 before scheduling
  further suites. Its already-running Factory/Bathroom Chromium child 526854
  completed successfully and cleaned up its own model process. Evidence remains
  `verified-ui-general/`; `supervisor-stop.json` records this scheduling change.

The UI-created custom operation produced this actual executor contract through
real RKLLM, with no C++ or prompt edit between creation and execution:

```json
{"actionId":"action_ustanovit_dozu","targetId":"device_dozator_reagenta","arguments":{"value_1":12.5}}
```

The virtual state changed **Доза: 0 мл → 12.5 мл**. The generated property key is
visible only in Advanced. The original free-form command remains unchanged.

### First clean UX verification, before the context correction

Fresh independent `final-ux/verified-core` and `final-ux/verified-full` builds were
compiled with `--clean-first`, using the same CMake options documented above:

- Clean core: **29/29**, **26.75 s**.
- Clean full: **41/41**, **31.20 s**.
- AdminSuggestionRetry: **51** checks; TypedMock: **1555** checks;
  GeneralizedAdmin: **77**; MockExecutionUnifiedRuntime: **127**;
  RkllmContextRefresh: **17**. The JS tests include the Simple compiler for every
  scalar kind, generic constraints, human errors and exact executor-contract display.
- No compiler warnings. All six UI assets match the build copies.

The first production-source SHA256, before the context correction, was
`45082832398fc1d9abfa6eecd1077ab784f2bc7d7e28f5a8e61c4a0d24a095b9`.
`production-frozen.json` and `verified-build-audit.json` record its input file hashes
and independently matched CMake header/assets. The status product identifies this
version as **Firefly Milestone 11 · Final UX · typed mock**.

First Simple Chromium acceptance (`verified-ui-final`): **PASS**, **310.07 s**,
PID **518632**, no JavaScript exceptions, SIGINT exit 0. The UI created the child
location and complete TV template, validated/saved/applied it without restart,
then exercised on/off, volume 35 and channel 7. Preview predicted power OFF → ON
while device state and execution history remained unchanged; Execute changed it.
The exact new volume wording used real RKLLM. Invalid volume and contradiction02
were rejected without state changes. Both original typo commands reached the
living-room light at 55 with visible server-produced entity evidence.

The same Simple flow created a template-free doser and its custom number action:
`Установи дозу на дозаторе реагента 12.5 мл` produced the canonical contract above
and state 0 → 12.5. The constraint editor selected TV by identity and displayed
volume limits; planned bindings were visible and disabled in Advanced. The
four-device AI plan passed strict decoding/validation, remained editable, and did
not change saved configuration or active generation when confirmed as a candidate.
Screenshots and complete AI/raw/command results are in `verified-ui-final/`.

First Advanced Chromium acceptance (`verified-ui-typed`): **PASS**, **377.40 s**,
PID **520239**, no JavaScript exceptions, SIGINT exit 0. It independently repeated
child/TV AI creation, four TV actions, invalid volume/channel, preview immutability,
both typo commands, and rejection with two indistinguishable TVs. The UI-created
conveyor changed running false → true → false, speed 0 → 45, direction forward →
reverse. The template-free string action `Пометь образец "готово"` ran through
real RKLLM and set `label="готово"`. Six AI requests (four entities, one action proposal and one
three-device plan) all decoded on their first attempt. The plan used 154/384 output
tokens. Advanced low-level editing remains available alongside the Simple flow.

Real AI measurements from that run (one request each, **0 retries**, all
vendor FINISH below limit):

| Draft/group | System / input / output tokens | Output budget | Inference ms |
|---|---:|---:|---:|
| Детская комната — identity | 200 / 18 / 25 | 256 | 4089 |
| Детская комната — language | 374 / 18 / 49 | 256 | 7940 |
| ТВ для детей — identity | 195 / 16 / 34 | 256 | 5416 |
| ТВ для детей — capabilities | 223 / 16 / 18 | 256 | 2999 |
| ТВ для детей — language | 435 / 16 / 70 | 256 | 11323 |
| Zone + four devices | 386 / 38 / 186 | 384 | 30017 |

These are inference times; complete entity requests also switch/restore contexts
and validate their merged result. End-to-end child and TV drafts took **23,859 ms**
and **33,921 ms**, respectively. The final four-device JSON is 186 output tokens,
well below 384; raising the budget was unnecessary. Its phrasing “на зоне упаковки”
is grammatically imperfect and deliberately remains editable. Strict validity does
not imply perfect Russian. The previously reproduced malformed 162-token response
and deterministic retry success/exhaustion tests remain in the ledger.



### Final verification after the required-argument context correction

Fresh `final-ux/rechecked-core` and `final-ux/rechecked-full` builds, both
`--clean-first`: core **29/29 (26.54 s)**, full **41/41 (31.61 s)**. TypedMock now
has **1565 checks**; AdminSuggestionRetry remains **51**. The final production-source
SHA256 is `27afcabdc9bde4c541848deb1e5b1c4e6a8a4106a1d6b12462d480f696c2f8ce`.
`production-final.json` and `rechecked-build-audit.json` independently record it
and all six UI assets. No further production edits were made after this freeze.
The corrected renamed string-action scope uses **577** system tokens at the
unchanged 128 command-output budget, inside the measured 1024-token export.
All earlier failed and successful attempts remain separate evidence.

`rechecked-http`: **PASS**, **465.20 s**, PID **533474**. DEV **48/48**, zero
false rejects/wrong/unsafe executions. Admin → light → TV → rejected unsafe
commands → admin → custom string execution all share one model load. Excessive
context validation rejects without publishing. Concurrent Apply returns 409 while
the request completes execution against generation 1. The later Apply publishes
2 without process restart and preserves `label="проверено"`; the previously
failing command now returns `arguments:{"label":"готово"}` and executes with
before/after `проверено → готово`. SIGINT during a further real inference exits
cleanly with code **0** in **7.54 s**, one model destruction. Evidence includes
per-command state/history immutability for every Unsupported DEV case, including
contradiction02. The preceding two failed full HTTP attempts remain documented;
this successful corrected run does not replace them.

`rechecked-ui-final`: **PASS**, **305.52 s**, PID **535852**, no JavaScript
exceptions, SIGINT exit 0. All Simple TV/custom-number/typo/preview/execute,
generic-constraints, canonical JSON, disabled planned bindings and multi-entity
candidate-only scenarios passed on the final source fingerprint. Screenshots:
`rechecked-ui-final/simple-tv-capabilities.png`, `simple-tv-playground.png`,
`advanced-execution.png` and the other named captures in that directory.

Final child and TV AI requests took **23,548 ms** and **33,522 ms** end to end.
Their token counts exactly matched the earlier table; inference groups took
**4022/7811 ms** (child) and **5324/2935/11247 ms** (TV). The final four-device plan
used **386 system / 38 input / 186 output**, budget **384**, **29,490 ms** inference.
All returned on attempt 1 with vendor FINISH below limit. Complete prompt/raw JSON,
decoded proposal and populated form values remain in the browser evidence.

`rechecked-ui-typed`: **PASS**, **391.47 s**, PID **537444**, no JavaScript
exceptions, SIGINT exit 0. All four TV actions, out-of-range channel/volume refusal,
ambiguous TVs, contradictory operations, both typo variants, conveyor
start/stop/speed/direction and Advanced custom string execution passed again.
Configuration was created through the UI and applied without restart. Generic
custom execution needs no CompactV1 representation; typed JSON remains canonical.

`rechecked-ui-general`: **PASS**, **165.19 s**, PID **539554**, no JavaScript
exceptions, clean SIGINT. Factory → Packaging Workshop → Packaging Light,
created via forms/AI, validated/saved/applied, executed both the “свет” and
“освещение” commands at **60** without C++ or prompt edits for this domain.
Bathroom `ванная/ванна` retained **0 → 40 → 30**. Generations changed without
restart. Tree/parent selection, preview immutability, human validation errors,
config diff, revision list, restore-as-candidate, import/export, status and
diagnostics passed. `rechecked-ui-offline`: **PASS**, **1.92 s**; unavailable AI
has a human message and manual form/validation remains usable.

Across the final HTTP and three online Chromium suites, **14 real local-AI
requests / 29 grouped generation attempts** completed with **0 retries and 0
decode errors**, all with `saved=false` and `applied=false`. This includes the
child zone, TV, workshop, Bathroom, conveyor, a custom action proposal and both
bounded multi-device plans. `rechecked-ai-summary.json` has every input/group and
its metrics. The earlier real malformed JSON is retained as evidence; retry
success, exhaustion, whole-request allowance and mutation isolation are covered
by the **51-check deterministic backend suite**, not claimed as a spontaneously
observed successful retry in these 14 final requests.

Final real RK3588S DEV runs, all on `rechecked-full` with unchanged labels/config:

| Run | Result | Whole process seconds | False accept / false reject / wrong action |
|---|---:|---:|---:|
| NPU 1 | 48/48 | 94.48 | 0 / 0 / 0 |
| Integrated 1 | 48/48 | 88.56 | 0 / 0 / 0 |
| NPU 2 | 48/48 | 88.58 | 0 / 0 / 0 |
| Integrated 2 | 48/48 | 87.98 | 0 / 0 / 0 |
| NPU 3 | 48/48 | 88.24 | 0 / 0 / 0 |
| Integrated 3 | 48/48 | 89.57 | 0 / 0 / 0 |

All six retained complete runs exited 0. Each had 17 deterministic routes and 31
fallbacks: 10 decoded actions, 20 model Unsupported results, one decoder rejection,
three SafetyGate rejections, zero malformed JSON. Raw candidates are therefore
not claimed perfect: three semantic candidates per run were rejected by safety.
The final result is **0 wrong/unsafe actions** and contradiction02 **Unsupported**
in all six runs; real HTTP also checks unchanged mock state/history. NPU average
request times were **1492.85 / 1395.59 / 1393.94 ms**. Exact commands, environment,
model/runtime/data hashes, thermal/frequency snapshots, raw logs and per-stage
traces are in `final-ux/hardware/`; no unsuccessful run was replaced or omitted.

Final speech replay, all exit **0**:

| Suite | Speech | Silence | Whole process seconds |
|---|---:|---:|---:|
| Native speech replay | 8/8 | 2/2 | 10.84 |
| GenericAction speech replay | 8/8 | 2/2 | 48.22 |
| Unified runtime speech replay | 8/8 | 2/2 | 60.90 |

The unified suite also exercises Apply and a subsequent voice request through the
shared generation/runtime. These are WAV replay results, **not live microphone
proof**. H340 remains absent in recorded USB/ALSA diagnostics; no live H340 claim
is made. All three stdout/stderr logs and exact commands are retained.
The final sequential hardware runner completed successfully in **669.80 s**.


## Инструкция для пользователя

1. Откройте «Зоны и места» → добавление зоны. Введите «Детская комната»,
   запросите локальное AI-предложение и просмотрите описание, тип и обозначения.
   Уберите неудачные синонимы; родителя выберите самостоятельно, если он нужен.
2. В «Устройствах» добавьте «ТВ для детей», выберите эту зону и шаблон
   «Телевизор». Для уже существующей карточки без действий используйте
   «Настроить по шаблону»: постоянный ID сохраняется.
3. На шаге «Что умеет устройство» оставьте нужные галочки: включение, выключение,
   громкость и канал. В «Допустимых значениях» проверьте громкость 0–100 % и канал
   1–999, при необходимости измените границы. Внутренние поля, аргументы и
   присваивания создаются автоматически. Это возможности виртуального устройства,
   а не обнаруженные возможности настоящего телевизора.
4. Подтвердите черновик → «Проверить» → «Сохранить» → «Применить…».
   Просмотрите diff и подтвердите Apply. Перезапуск не нужен.
5. В Command Playground сначала «Проверить · Preview», затем при необходимости
   «Выполнить · Execute»: «Включи телевизор в детской», «Выключи детский телевизор»,
   «На телевизоре в детской установи громкость 35 процентов»,
   «На детском телевизоре включи канал 7». При двух подходящих телевизорах нужна
   однозначная уточняющая фраза/обозначение.
6. Для собственной операции в Simple выберите «Добавить действие», устройство
   и название. Ответьте, нужно ли значение, какого оно типа, какие значения
   допустимы и что должно измениться. Например, «Установить дозу»: числовое
   значение 0–50 мл → новая характеристика «Доза». Ключи аргумента и состояния
   создаются автоматически. Для точного редактирования схемы и нескольких
   присваиваний переключитесь в Advanced.

Итоговая проверенная сборка:
`build/milestone-11/final-ux/rechecked-full/firefly-demo`.
Сервер **оставлен запущенным без voice** на **http://192.168.68.111:8080/**, PID **543424**.
Проверены реальный `/proc/543424/exe`, аргументы запуска, localhost и LAN
status и SHA256 всех **шести** отдаваемых UI assets. В HTTP стоит
`Cache-Control: no-store`. Runtime готов, режим mock, generation 1,
stored/active revision 1. Тестовые процессы перед этим завершены.

Advanced/status показывает **Firefly Milestone 11 · Final UX · typed mock**,
HEAD `99c635d3534789d5c132b245ee53caf236383e12` и source SHA256
`27afcabdc9bde4c541848deb1e5b1c4e6a8a4106a1d6b12462d480f696c2f8ce`.
SHA256 executable:
`1ea410807f1adc123c16ee8bf7ae3009a2e0d72773e2db5261eac353989805be`.
Артефакты проверки в `final-ux/`: `user-demo-final.json`,
`user-demo-lan-check.json`, `user-demo.log`, `rechecked-build-audit.json`.
Первый вспомогательный LAN audit ошибочно ожидал поле `model_ready`; публичный
status использует `runtime_ready`. Исправлен только read-only audit, повторная
проверка прошла; `user-demo-lan-check-first-error.txt` сохраняет эту ошибку скрипта.

Пользовательское хранилище не переписано. SHA256 до/после:
`6f7d116d7cdb0f95bce447f4b98d0de104cf08056aa0694e17aaa6b12efbdff2`.
В нём сохранены прежние четыре зоны/пять устройств. Тестовые ТВ, цех и дозатор
остались в отдельных smoke stores/review fixtures и не импортированы в него.

Точная команда запуска этой сборки (сервер уже работает; второй запуск на тот же
порт не нужен):

```sh
cd /home/firefly/FireflyVoiceAssistantLinux
./build/milestone-11/final-ux/rechecked-full/firefly-demo \
  --store /home/firefly/.local/share/firefly-demo-m11 \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-bind 0.0.0.0 --admin-port 8080
```

Для будущей физической проверки H340 эта же сборка принимает:
`--voice --stt-model-dir build/stt/model --vad-model build/stt/vad-ab/silero_vad.onnx --audio-device plughw:CARD=H340,DEV=0`.
В текущем listener эти флаги не использованы: повторная проверка USB/ALSA после
regression также не обнаружила H340 (`usb-final-after.txt`, `alsa-final-after.txt`).
Live voice после Apply не заявляется выполненным; выполнены только описанные replay.

## Explicit boundaries

No hardware executor, GPIO, RS-485, MQTT, Home Assistant, cloud AI, wake word,
model research, heavy frontend toolchain, systemd or packaging work is included.
AI linguistic suggestions still require review; structured validity alone does
not establish semantic quality or permission. No claim of real TV/conveyor control
or industrial safety is made. The Logitech H340 is absent from USB/ALSA on this
Firefly; replay can be tested, but a live H340 proof must not be claimed.

Remaining product limits are explicit:

- Generic commands need an exact configured operation phrase and an unambiguous
  device mention; typo tolerance concerns entity mentions only. Enum values and
  boolean argument literals are exact. Rich string/number arguments use RKLLM;
  arbitrary paraphrases, multi-operation requests and invented values may reject.
- A template declares mock assignments, not a hardware capability discovery result.
  TV channel/volume setters do not implicitly power on the TV. Their independent
  state effects are visible and can be edited before confirmation.
- AI may produce awkward synonyms (an earlier child-room proposal included
  “ребенокская комната”) or invalid structured output. It also produced useful
  configured forms; the user reviews/removes unsuitable entries. No claim of
  complete Russian morphology or perfect semantic suggestions is made.
- Every complete command scope must fit the measured 1024-token export. Very long
  schemas/language lists fail admission. The old active generation remains usable;
  no new model export was attempted. First validation of many new scopes takes
  measurable time because each prompt is admitted by the real tokenizer/runtime.
- Mock state is in memory. Reconciliation rejects incompatible retained state;
  automatic destructive reset is not offered. A new process initializes declared
  state. Revision restore restores configuration as a candidate, not past execution
  state.
- The inherited runtime still rejects disabled/deny configurations and additional
  protected entrance-door targets rather than claiming to support policies it does
  not implement. The existing entrance target and mandatory compiled policies remain.

## File inventory

The complete review inventory (including the resumed M11 changes):

Final UX review inventory: **44 modified tracked files + 23 new files (67 total)**,
no staged changes, HEAD still `99c635d3534789d5c132b245ee53caf236383e12`.
Tracked diff: **2070 insertions, 281 deletions**; new files are additional to
`git diff --stat`. The inventory below matches Git exactly. Build/generated
artifacts, model files and credentials are not included. Nothing is committed.
The preceding continuation had 44 modified + 20 new files; the three additional
files in this pass are `simple-editor.js`, `AdminRetryTests.cpp` and
`FinalUxWebTests.js`. Final audit artifacts are under `final-ux/`; the earlier
continuation audit remains retained in its own evidence directory.
`review-audit.json`, `final-git-status.txt`, `final-git-diff-stat.txt`,
`final-git-diff.patch`, `final-review-files.json` record the final source inventory.
`git diff --check` and an additional whitespace check of new files pass. Staging
is empty. Source fingerprint and six assets match the final verified build;
`tests/data/dev.tsv`, `config/current-m4.json` and the user's store are unchanged.

- `CMakeLists.txt`
- `config/admin-templates.json`
- `config/schema-v1.json`
- `config/schema-v2.json`
- `docs/milestone-11-generalized-ai-admin.md`
- `include/firefly/CommandEngine.h`
- `include/firefly/RkllmEngine.h`
- `include/firefly/actions/GenericAction.h`
- `include/firefly/actions/ScalarJson.h`
- `include/firefly/admin/AdminDraft.h`
- `include/firefly/admin/RuntimeGeneration.h`
- `include/firefly/admin/RuntimeWorker.h`
- `include/firefly/config/ConfigStore.h`
- `include/firefly/config/Configuration.h`
- `include/firefly/execution/ActionExecutor.h`
- `include/firefly/llm/LlmContext.h`
- `include/firefly/llm/StructuredOutput.h`
- `include/firefly/runtime/DomainView.h`
- `src/CommandEngine.cpp`
- `src/RkllmEngine.cpp`
- `src/SafetyGate.cpp`
- `src/actions/CompactV1Adapter.cpp`
- `src/admin/AdminBuild.h.in`
- `src/admin/AdminDraft.cpp`
- `src/admin/AdminPlan.cpp`
- `src/admin/AdminTemplates.h.in`
- `src/admin/Api.cpp`
- `src/admin/CMakeLists.txt`
- `src/admin/HttpServer.cpp`
- `src/admin/Json.h`
- `src/admin/RuntimeGeneration.cpp`
- `src/admin/RuntimeModel.h`
- `src/admin/RuntimeWorker.cpp`
- `src/admin/web/admin-model.js`
- `src/admin/web/admin.css`
- `src/admin/web/admin.js`
- `src/admin/web/index.html`
- `src/admin/web/typed-editor.js`
- `src/admin/web/simple-editor.js`
- `src/config/CMakeLists.txt`
- `src/config/ConfigLoader.cpp`
- `src/config/ConfigSchema.h.in`
- `src/config/ConfigStore.cpp`
- `src/config/ConfigValidator.cpp`
- `src/config/TypedSchema.cpp`
- `src/execution/MockDeviceExecutor.cpp`
- `src/llm/LlmContext.cpp`
- `src/llm/StructuredOutput.cpp`
- `src/runtime/DomainView.cpp`
- `src/runtime/EntityResolver.cpp`
- `tests/FakeRkllmRuntime.cpp`
- `tests/FakeRkllmRuntime.h`
- `tests/admin/AdminTests.cpp`
- `tests/admin/RkllmContextRefreshTests.cpp`
- `tests/admin11/AdminDraftTests.cpp`
- `tests/admin11/AdminRetryTests.cpp`
- `tests/admin11/FinalUxWebTests.js`
- `tests/admin11/AdminModelTests.js`
- `tests/admin11/OfflineWebTests.js`
- `tests/admin11/TypedHttpSmoke.py`
- `tests/admin11/TypedMockTests.cpp`
- `tests/admin11/TypedWebTests.js`
- `tests/admin11/WebAdminTests.js`
- `tests/config/ConfigCliTests.py`
- `tests/config/ConfigTests.cpp`
- `tests/execution/UnifiedHttpTests.py`
- `tests/runtime/ParserRoomCutoverTests.cpp`

Builds, models, raw outputs and screenshots are not tracked changes.
