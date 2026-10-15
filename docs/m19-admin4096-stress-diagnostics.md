# M19 Admin4096 failure diagnostics

## Current result — 2026-10-15

**BOUNDED_TWO_CYCLE_PASS / RECOVERY_PASS** on the clean native aarch64
`c7fa14906f8ae15f4c8fb488e9c9d7628eb8154f` product candidate, with operator
`b98714fcbc6d24e0d19c9c415d1a6e6a401a80f6`. The first authorized window completed
two real Runtime → Admin4096 → Runtime cycles. Each cycle sent the same 1606-byte
long request and produced a complete strict Cabinet/Lamp proposal, unchanged
system identity, expected Russian forms, boolean ON/OFF and no bindings. Request
times were 73.970 and 72.677 seconds. Both internal Runtime restores passed.
The 10 reported main/language inference trace entries all used attempt 1; that
count excludes SDK bootstrap inference.

There was exactly one original-production stop/start. Independent recovery
returned installed `m19-320b80a6-20261014T173500Z` to READY / WAITING_WAKE in
53.044 seconds, with sole NPU/microphone ownership. Production ConfigStore bytes,
owners and modes, model metadata, release, unit and arguments were preserved.
All seven temporary units were inactive afterward. No Apply, device commands or
voice replay occurred; the private active ConfigStore remained empty revision 1.

Across 382 watchdog samples, minimum MemAvailable was 1.577 GiB and peak test
process PSS was 5.160 GiB; cgroup max/OOM/OOM-kill counters remained zero.
The maximum observed context was 1211 tokens, with 1571 reserved: **full 4096-token
saturation, long-running soak and universal model reliability are NOT_VERIFIED**.
Previously accepted owner voice ON/OFF results are unchanged; this window did
not add instrumented OFF evidence. The candidate was not installed by this test.

Independent evidence review passed. Private evidence is retained on the control
PC at `evidence/m19-repeatability-prep-20261015T124327Z/live/attempt-1/` (outside
the repository). The final supervisor/private-store archive SHA256 is
`35ad2a16b4cee8bde005ca9e60bcba180749320cbcb41a2e635b03f19613287b`, verified
against the Firefly stream. Raw private stores, device endpoints and audio are
not published. The relocated test ELF SHA256 is
`566342b4708c9d9b569c9a425b0e918083ad2cdee48b814d2d84b9882b73aa9e`.

The sections below record preparation and earlier failures chronologically in
reverse order. Historical UNVERIFIED statements are scoped to their own stage;
they do not override the bounded result above or imply full-context acceptance.

## Bounded repeatability on the verified c7fa149 candidate

The real single-long check of `c7fa149` passed: one strict Cabinet/Lamp proposal
with boolean ON/OFF and the expected Russian forms, followed by Runtime restore.
Its main and four language generations each used attempt 1. Original installed
`320b80a6` production recovery also passed. The corrected candidate is uninstalled.

The existing PC harness adds `--candidate-repeatability`, requiring that exact
root-private candidate descriptor. It runs **two complete Runtime → Admin4096 →
Runtime cycles, with one long1606 request per cycle**. The short control request
is omitted. No Apply, commands, device calls or voice tests occur. Every proposal
must be editing/complete with the unchanged system, one Кабинет, one Лампа in
that room, one boolean power field and exactly ON/OFF effects, without bindings.
Both cancel/Runtime READY checks and external production recovery are mandatory.

The fixed repeatability mode is distinct from diagnostic1×1 and historical
stress2×2. Mode, workload/client hashes and descriptor reference are bound across
the PC transport, offline receipt, frozen supervisor and cutover checks. A prior
mode's proof cannot qualify this window. The previously verified native ELF,
installed libraries/assets and models are reused; no product rebuild or rollout.

Measured single-cycle timing was 193.59s plus 55.84s initial startup. Two cycles
therefore estimate 443s against an approximately 570s client deadline; this is
an estimate, not a guarantee. The new profile requires 300s before starting a
cycle and 220s before a message (120s message timeout + 100s for cancel and
Runtime READY together). Cancel and subsequent readiness polls share one absolute
restore deadline, capped by the original window; a late READY cannot pass.
Those checks repeat immediately before submission after preparatory operations.
Insufficient time produces an incomplete FAIL and recovery; no deadline extends.
The 600s hardware window, independent 360s recovery and RAM6/1GiB are unchanged.

Success is labelled **BOUNDED_TWO_CYCLE_PASS**, never full-context stress PASS.
Vendor counts and memory at each cycle are recorded. The last real maximum was
1211 observed/1571 reserved tokens. The Admin API limits each atomic step to
1800 UTF-8 bytes; no offline vendor tokenizer measurement proves that a legal
workload fills4096. Saturation and long-term soak remain **NOT_VERIFIED**. This
preparation does not authorize another hardware window; a fresh owner START and
fresh matching gates are required.

## Offline brightness-exclusion instruction routing

The `874036b` real single-request check still failed: both full 87-byte replies
ended with an extra closing brace. Strict parsing rejected both replies, no
Apply occurred, and external production recovery passed. This is not a model
quality fix or a full ctx4096 stress PASS.

The long request repeats `без яркости`, which the old substring check treated
like an affirmative brightness request when selecting the dialogue instructions.
Instruction selection now recognizes only the complete `без яркости` phrase,
with whitespace between its words. Every brightness mention must be excluded;
another affirmative/ambiguous mention, an editing verb, or a standalone `не`
keeps the general instructions for this newly recognized case. Punctuation
between `без` and `яркости` does not extend the exclusion across clauses.
Other negative formulations conservatively keep the general instructions.

Only instruction selection changes. The original inference input, existing
scope/grounding/ambiguity decisions, strict whole-document decoder, semantic
guards, model, sampling, retry count and resource limits are preserved. In
particular, a multiple-room request still asks before inference, and an AI
proposal adding excluded brightness still fails validation.

Offline regressions check the exact long input, normal creation, case/whitespace,
positive and mixed brightness, negation, editing, malformed boundaries, original
input preservation, room ambiguity, and rejection of excluded brightness. Their
scripted model verifies routing and safety contracts only. At this offline-preparation stage, whether real Qwen
would return valid JSON remained **UNVERIFIED**. No new
hardware window or production activation is authorized by offline preparation.

## Single-document instruction correction

The real format-fix check of `0e458dd` still failed: the two complete replies
contained the requested operation arrays followed by either a second JSON object
with the literal question placeholder or an extra closing brace. The complete
replies were rejected at bytes 89 and 87, without extracting or executing a prefix.
Production recovery passed. Neither an OOM nor full ctx4096 coverage was observed.

The dialogue prompts now describe mutually exclusive `ops`, `question` and
`unsupported` branches in prose and retain one complete positive JSON example.
Literal alternative JSON documents and duplicate full-plan examples are removed.
The shared retry remains schema-neutral because it also serves entity/language
drafts: generate a complete single object, close each open construct once and end
the response. The bounded validator reason remains; rejected raw is not inserted.

Both complete real responses are offline negative fixtures on the exact long
input and in manual-checkpoint/diagnostic tests. Strict whole-document parsing,
the existing one-format-retry allowance, routing, semantic guards, models, context
and memory limits are unchanged. Valid question/unsupported responses remain
supported. These fixtures establish fail-closed behavior, not real Qwen quality.

## Captured failure and offline instruction correction

The subsequent single-request diagnostic window captured both complete rejected
Qwen responses (139 and 106 bytes). Both use object braces for ordered operation
tuples, for example `{"location","Кабинет"}`, instead of JSON arrays. They are
syntactically invalid JSON; the strict decoder rejects them before semantic
validation. The first response also requests a system name absent from the user
request. `system_name` itself is a supported operation, not an unknown opcode.

The exact 1606-byte request takes the general dialogue prompt because its explicit
constraints mention brightness. Routing is preserved. That prompt previously
introduced `ops` with a non-JSON placeholder rather than a complete example.
It now shows a complete valid nested-array response, explains array brackets,
and distinguishes naming a system from creating a zone. Optional arguments are
described outside syntactically valid JSON examples. The creation-only prompt
also states the array rule. The existing single format retry gains a static JSON
syntax reminder; no rejected output is copied into it or repaired.

The offline contract regression replays both captured malformed responses against
the exact long input: both must fail, preserve configuration and keep the message
incomplete. Only a separately generated valid second response can form a proposal.
These scripted fixtures do **not** prove that Qwen will now generate valid JSON.
No decoder, retry count, model, RAM limit, context size or routing changes.
The runtime compact prompt and execution/SafetyGate are unchanged.

That diagnostic window recovered the installed `320b80a6` production release to
READY / WAITING_WAKE in 52.675 seconds. All temporary units were stopped. Its 163
memory samples had a minimum MemAvailable of 1.655 GiB and no OOM events. The
full ctx4096 stress/soak remains unverified. These source corrections are local
and uninstalled; a new real Qwen check requires a reviewed native candidate,
fresh preflight/recovery and separate owner authorization.

## Historical first bounded stress and evidence gap

The real 2026-10-15 bounded stress window failed on the 1606-byte input after
the short request passed. The long request reached inference (275 vendor input
tokens), then both built-in attempts returned `invalid_ai_response` at `decode`.
The API returned HTTP 409 and the draft stayed `planning_failed` and incomplete.
The original production was independently restored. This remains **FAIL**, with
recovery **PASS**; no second hardware attempt was made.

The 205 watchdog samples had no cgroup OOM/max events and a minimum available
RAM of 1.675 GiB. The maximum observed context + input + output was 954 tokens,
with reservation 1310, so this does not verify a full 4096-token workload.
Neither memory exhaustion nor a particular JSON/schema defect is established.

Two evidence gaps prevented a more precise diagnosis:

- The stress CLI discarded the HTTP error response and did not append the
  failing case before sending the request.
- `SetupSession::invoke` intentionally retained only trace metadata. The setup
  API also wraps the failure as a generic `setup_failed` response. Capturing
  that HTTP body alone cannot recover the rejected vendor response.

The new diagnostics preserve HTTP status before reading the response, capture
up to 64 KiB as exact base64, and label incomplete reads, truncation and JSON
parse failures. Failed cases retain elapsed time and private trace independently
of the request result. Mutations are never retried by the client. Reports are
mode 0600; malformed Unicode/JSON diagnostics must not mask the original status.

The existing private mode-0600 draft now retains a bounded rejection reason,
parser stage when available, and a hex prefix of failed decode responses.
Successful response text, prompts and inputs are not added to trace. Each raw
prefix is at most 4096 bytes; error prefixes are at most 512 bytes. Combined
serialized error strings and raw hex payload have a session-wide 32768-byte
budget. Byte counts, truncation and exhausted-budget markers distinguish partial
evidence. Public setup views do not expose these fields. The evidence originates
from the worker's IPC trace, whose existing JSON serialization can replace invalid
UTF-8: it is not claimed to preserve pre-IPC vendor bytes.

No decoder, prompt, model, context/RAM threshold, retry policy, Apply or execution
rule was relaxed. This is an evidence correction, **not a demonstrated fix for
the real Qwen failure**. Its missing historical raw response cannot be restored.

Targeted validation:

- `python3 -B tests/M19AdminStressTests.py`: PC offline HTTP fixture, error
  retention, read/reset/parse/Unicode failures, limits and no mutation retries.
- `AdminDialogueContract`: the exact long input with a scripted valid response
  checks the real request/decoder contract only. Rejected outputs check exact
  private prefixes, UTF-8 boundaries, public-view exclusion, and 32 failed
  requests followed by successful cancel/restore within the diagnostic budget.
- `DialogueSetup`, `AdminSuggestionRetry`, `AdminSetupV2Api`: targeted lifecycle,
  fail-closed Apply and private HTTP/API diagnostics regressions.

Offline executables are built on the PC with the real aarch64 compiler, static
linking, `FIREFLY_BUILD_ASSISTANT=OFF`, `FIREFLY_BUILD_VOICE=OFF`, and the existing
CMake targets. They are CPU-only test binaries, not a production artifact. Their
Firefly execution is isolated with `DevicePolicy=closed`, 512 MiB MemoryMax,
one CPU, private temporary files and localhost networking. Production stays up.
These checks cannot establish real RKLLM correctness or stress acceptance.

The installed production remains the accepted `320b80a6` release. These source
changes are not installed. Further hardware testing requires a separately built,
reviewed candidate, fresh admission/recovery preparation and new owner START.

## Historical single-request diagnostic window

At that stage, the PC harness accepted a root-private diagnostic descriptor and its
explicit SHA256. This profile uses a clean native `9945f59` candidate, whose only
product source difference from installed `320b80a6` is `SetupSession.h`. The
descriptor pins source/header/ELF identity; the copied ELF is checked against the
expected SHA before its plan is sealed. Shared installed libraries, models and
wake assets are reused. Recovery still targets the installed `320b80a6` release.

`--diagnostic-only` sends exactly the long 1606-byte request once in one private
Setup cycle. The short request and voice checks are not repeated. It calls no
Apply, execution or device endpoints. Fresh offline proof binds this mode, client,
workload, backend and candidate reference; the existing complete PC browser gate
and independent 600-second timer/360-second recovery remain mandatory.

The result is `AI_REJECTION_CAPTURED` only for a failed POST `/api/setup/message`
with HTTP 409, an incomplete `planning_failed` long case, and complete rejected
decode evidence. Cancel, restore, transport errors and incomplete capture remain
failures. An accepted request with a completed restore is labelled
`AI_REJECTION_NOT_REPRODUCED`. Neither result is full-context stress acceptance
or proof that the historical Qwen failure was fixed.

Preparation never arms recovery or stops production. After new owner START the
same harness creates a fresh one-use plan and repeats the required short-lived
gates. It does not reuse an expired prepared root or authorize another attempt.

## Historical 0e458dd format-fix candidate qualification

That private candidate was built from clean `0e458dd`, in a separate
`FireflyVoiceAssistantLinux-m19-format` checkout. The operator pins that exact
commit, source fingerprint, ELF identity and the hashes of all three product files
differing from installed `320b80a6`: `SetupSession.h`, `RuntimeGeneration.cpp` and
`RuntimeWorker.cpp`. The historical `9945f59` candidate and evidence are retained.
Only test/operator files are overlaid after the clean product build; they do not
change the product fingerprint or installed release.

For this check, an accepted clarification alone is insufficient. After the client
has completed its normal cancel/Runtime restore, the PC success classifier
requires an editing, complete proposal: one Кабинет, one Лампа in that room, one
boolean power field and the two parameter-free ON/OFF effects, no bindings and no
change to the original system identity. Other outcomes remain failures without
another inference. The scripted offline worker now explicitly handles the exact
long input so this gate exercises proposal creation and restore, not just its
previous clarification fallback. This remains scripted transport/contract proof,
not evidence that real Qwen has accepted the changed instructions.
