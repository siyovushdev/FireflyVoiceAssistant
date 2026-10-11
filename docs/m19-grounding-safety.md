# M19 / Issue #17: original-request authorization

The 2026-10-11 isolated live acceptance **failed**. Fresh genuine Qwen speech
forms and isolated Test Apply passed, but physical voice ON/OFF was not recorded.
An automated **text** negative caused unintended HTTP actuation. This remediation
is offline engineering evidence, not a new hardware acceptance or release approval.

## Root cause and trust boundary

With the reviewed `Кабинет → Лампа` configuration, the original request
`включи лампу в неизвестной комнате` followed this path:

1. `DomainView::resolve` collected the known device alias `лампу`. It had no
   configured match for the requested room, so `grounding.locations` was empty.
2. `LanguageIndex::match` detected uncovered `неизвестной`, `комнате`, but returned
   `INDEX_MISS`, admitting fallback rather than an executable index result.
3. `llm::selectAction` selected the unique configured device/action from the
   partial grounding. Stable compact Qwen returned argument-only `[]`.
4. `CompactActionDecoder` attached that result to the server-selected valid IDs.
   `SafetyGate` verified the device/action aliases and scalar evidence; its
   generic AssignState branch did not account for the remaining request.
5. Domain validation checked the configured operation's shape/range, not the
   omitted room. `RuntimeGenerations::Request::process` dispatched the admitted
   action through `BoundDeviceExecutor` to one POST on the isolated mock.

The problem was incomplete **server authorization**, not an invented model ID.
The server had already given the model a selected contract; registered IDs and
valid scalar syntax did not prove correspondence with the whole original request.

## Mandatory rule

SafetyGate remains the common preserve-or-reject barrier for LanguageIndex,
legacy deterministic parsing and all model protocols. Its request coverage check
accounts for the original input using the selected configured device/location,
exact configured action atoms, independently validated argument spans, and bounded
existing command/politeness syntax. Unknown meaningful words fail closed.

Every spatial `в / во / на` construction must refer to the selected configured
location, a supported device object (`на телевизоре`), a validated numeric slot,
or the established entrance-door syntax. A whole device/action alias containing
an unknown location cannot grant location authorization, even on `INDEX_EXACT`.
Unquoted spatial text cannot be recast by the model as an opaque string argument;
an explicitly quoted literal can remain scalar data without bypassing existing
entity/negation checks. Known conflicting locations,
ambiguous devices/actions and zone-less devices with a location are rejected.
Unique devices without an explicit location, including devices without a zone,
retain their existing supported behavior.

No aliases are invented, no transcripts repaired, and no model confidence supplies
missing evidence. Existing negation, coordination, scalar, door and brightness
checks remain mandatory. ConfigStore schema, compact protocol, model and prompts
are unchanged. LLM fallback remains available for supported polite requests and
string/numeric extraction; it cannot authorize dropped constraints.

The common check deliberately tightens prior loose expectations for unaccounted
background/source/timing text and unreviewed intent/unit typos. Those requests
now reject instead of discarding words. Frozen DEV data is unchanged. Unsupported
paraphrases may produce conservative false rejects; this is not permission to
add arbitrary filler words or relax grounding.

## Offline regression and provenance

`tests/language/m19-live-semantic.json` contains the actual reviewed semantic
configuration from `test-apply.json` of the failed live run. Connections, bindings
and observations are empty so it contains no live endpoints. Each test creates
only replacement loopback contracts with distinct target paths.

`GroundingAuthorizationHttp` exercises 450 cases through Api text execution,
guarded device commands, worker text, synthetic `RuntimeWorker::processVoice`,
CommandEngine without an index, and direct injected SafetyGate candidates.
Structured and stable compact inference are deliberately scripted to return
registered IDs / `[]`, including unsafe requests. A Python HTTP observer checks
each completed case before allowing the next: zero outbound requests on rejects,
exactly one correct target-path/body POST on valid requests, and correct source
history. The matrix includes unknown/known/conflicting rooms, duplicate names
across/in one zone, mixed zone-less devices, Russian cases, numbered multiword
devices, forged registered targets, and aliases that would otherwise give an
unsafe `INDEX_EXACT`. Additional scalar tests retain legitimate LLM extraction
and reject attempts to disguise unknown locations as arguments.
Legacy brightness and entrance-door regressions also reject extra unknown or
conflicting locations through both injected candidates and fallback.

```sh
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
# Optional private machine-readable case evidence:
FIREFLY_GROUNDING_EVIDENCE=/private/path/grounding.json \
  ctest --test-dir build -R GroundingAuthorizationHttp --output-on-failure
```

These tests use no NPU/microphone and do not prove acoustic/Qwen live acceptance.

Verified on 2026-10-11 with Admin, assistant, voice and configuration-tool build
options enabled: **84/84 CTest**, including the Chromium/integration suites,
**41/41 frontend tests**, TypeScript checks, and the embedded Web Admin production
build/offline asset check. The authorization matrix passed **450/450**: **286**
rejected cases emitted **zero HTTP requests**, and **164** permitted cases each
emitted exactly one verified target/action POST. Of the rejects, 32 reached
`INDEX_EXACT` and were stopped by SafetyGate. See the shareable
[review proof](evidence/m19/grounding-review-proof.json) for artifact digests.

Original immutable live evidence, including proposal, Apply, raw negative response,
HTTP log and restoration proof:
`/home/firefly/firefly-m19-live-final-20261011T181251Z/evidence/`.
All 1089 manifest digests were checked before remediation. Remediation build,
frontend/CTest logs, adversarial pre-fix reproduction and read-only production
comparison: `/home/firefly/firefly-m19-safety-20261011/evidence/`.
The final read-only audit verified the same production PID, release, systemd unit,
all ConfigStore file hashes and exclusive NPU/microphone owner; readiness remained
`READY / WAITING_WAKE`. All 1089 original evidence digests still matched.

## Installed production exposure and next gate

Installed release remains `wake-beep-dc7b142e-20261011`, source `dc7b142e`.
Its resolver, SafetyGate, CommandEngine, compact selection and RuntimeGeneration
source are identical to the failing `74850ba` candidate. Read-only production
aliases still omit `лампу`; this blocks that particular short form but does not
remove the vulnerability. An **offline** replay of its current semantic snapshot
with `включи лампа в неизвестной комнате`, the shared pre-fix source and a scripted
`[]` produced one POST to a replacement localhost binding. This establishes an
executable unsafe path under a permissive model result, **not** observed production
Qwen inference or a production-device test. Production cannot be declared safe.

No production writes, service restart, model runtime, deployment or release-pointer
change is part of remediation. The installed release has not received this fix.
PR #15 stays Draft; Issues #16/#17 stay open. Independent Chief Engineer review
is required. Genuine Qwen/reSpeaker ON/OFF, source=voice, exact single POST and
independent GET/Confirmed remain pending a separately authorized test window on
the reviewed new candidate; the live harness/watchdog identity must be pinned to
that new clean commit before any such window.
