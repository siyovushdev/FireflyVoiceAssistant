# Qwen power intent and setup phrases (owner follow-up, 2026-10-20)

This is a language/runtime change after M20's accepted G3 baseline. Earlier G3
or package acceptance does not approve this new runtime. It needs a new ARM64
candidate and an owner-controlled target/ESP32 check before installation.

## Behavior

AI Setup requests ordinary, polite and colloquial equivalents for each new
action. Power examples include `отключи`, `выруби`, `вырубай` for OFF and
`вруби`, `врубай`, `зажги` for ON. The model proposes up to eight phrases; the
server normalizes and deduplicates them, merges existing phrases, checks
conflicts, and rejects obvious opposite-polarity or descriptive power aliases.
Existing reviewed entities are not silently regenerated. Proposals remain in
the private draft. Cancel preserves current configuration and runtime. Review
and explicit Apply publish the draft through existing CAS/admission/LKG rules.
Neither suggestions nor Save/Apply execute device commands.

Runtime first tries the existing LanguageIndex/parser. A configured phrase
keeps its fast path. A new wording can reach Qwen only when the original text
exactly grounds one enabled Generic device, with a boolean `power` field and
exactly one permitted, no-argument ON and OFF assignment. Fuzzy target repair,
multiple devices/locations, conflicting states, negation, informational questions, numeric
arguments, conditions, schedules and extra capability constraints do not enter
this new route. Descriptive statements such as `свет на кухне горит` are refused
before inference. Unknown location clauses remain subject to source coverage.

The unknown intent must be a short contiguous request span (at most eight
words), separate from grounded entity spans. Conservative request morphology
admits imperative/infinitive forms and `да/пусть/пускай …`. Shared request-form
analysis additionally accounts for positional courtesy: leading `а`,
`пожалуйста`, `будь добр / будьте добры`, a request verb followed by `ка` or
attached `-ка`, and `можешь/можете` followed by an infinitive. Grounded entities
may occur between the modal and verb: `можешь свет включить на кухне`.
A terminal `?` is allowed for that immediate modal request, not for arbitrary
questions. `можешь включать свет?`, `можешь ли …`, state questions, negations
and conditions remain refused. This deliberately does not cover every Russian
sentence or word order, including `не мог бы …`. A refusal is preferable to
guessing. Existing deterministic commands use their existing validation.

An initial `да` is not itself evidence of a request. The uncovered intent must
have a request verb (`да включи уже`) or a jussive verb (`да будет`, `пусть
погаснет`, `пускай включится`), followed only by bounded request particles.
Reports such as `да я включил свет`, `я выключил свет` and plural past forms
such as `включили свет` are refused even if a model returns a valid positive
choice. This checks request structure, rather than banning `да` or rewriting
the transcript. The model instruction separately contrasts these reports with
immediate requests. Direct model-only checks in the isolated harness verify
that instruction as well as the runtime/gate and real MQTT publication path.

Known action aliases do not exclude semantic fallback: `можешь включить свет`
can need Qwen even when `включить` is already configured. The model must retain
that explicit action/polarity. The same request analysis supplies the original
byte spans to SafetyGate; courtesy is not globally stripped or ignored. In
particular `а` inside a coordinated request and misplaced `ка` are not fillers.
No input is rewritten before inference, and unknown location clauses are not
silently removed. General SafetyGate and deterministic-only device controls
do not receive a new permissive word list.

Qwen receives the unchanged original phrase and a server-derived two-choice
contract. It can return only `[0]`, `[1]`, or `null`. The server maps that choice
to existing action/target IDs, checks explicit ON/OFF polarity, recomputes the
original scope and source coverage in SafetyGate, and validates the typed
action again. No new transport topic, payload, device or parameter can come
from the model. Scalar `0`/`1`, extra text, invented JSON fields and wrong
polarity fail closed. Model refusal/error never executes. The default
SafetyGate path and deterministic-only operational device controls retain
their previous contract.

This scope limits model authority; it does not make semantic inference
infallible. New phrasing is interpreted anew, never automatically learned or
persisted. AI-proposed aliases require owner review. Broader actions,
parameters, schedules, arbitrary multi-clause language and online learning are
outside this MVP.

## Diagnostics

Playground and Diagnostics show the original recognized text, rules/Qwen
route, configured action/device names, and a refusal reason. Wire diagnostics
add `llm.invoked` and `llm.task` (`power_intent` or `arguments`), plus
`language_route=LLM_POWER_INTENT` after the scoped decoder. Invocation is
recorded before inference, including inference failure; zero fake token
metrics are never proof that a model ran. Existing raw diagnostics remain.
Execution acceptance/CONNACK/PUBACK is distinct from independently observed
physical state.

The existing systemd journal is the terminal live-log interface:

```sh
ssh -t firefly@100.91.173.108 'journalctl -u firefly-voice-assistant.service -f -n 30 -o short-iso --no-pager'
```

`VOICE` retains the speech transcript and timings. `COMMAND_TRACE` adds a bounded
decision summary: source (`voice/preview/playground/admin-production/admin-device`),
rules/Qwen route, actual `llm.invoked/task`, SafetyGate/domain
result, refusal stage and execution `accepted/failed/not_executed`. This summary
does not copy prompts, model responses, transport credentials/addresses/payloads
or audio. Recognized command text is private diagnostic data; review logs before
sharing. `accepted` still does not assert physical confirmation. Ctrl+C stops
only the viewer. No Web Admin logging feature or new logging service is needed.

## Isolated verification

Build the frontend first as in [build-and-test.md](build-and-test.md), then
build both host MQTT OFF and ON with prepared real dependencies as in
[M20 G3](m20-g3-isolated-acceptance.md). Run the full registered CTest suites;
the new ordinary tests are `PowerIntent` and `AISynonymSetup`. They exercise
production decoders, gates, runtime, setup drafts, Cancel/Apply and diagnostics
with an inference seam, including adversarial positive model output.

For real inference, use a prepared native CPU `llama-server`, Qwen3-4B GGUF
and native Mosquitto. Nothing is downloaded or installed by this harness:

```sh
python3 tests/language/PowerIntentAcceptance.py \
  --root /absolute/checkout \
  --fixture /absolute/checkout/build/host-on/src/admin/power_intent_http_fixture \
  --server /absolute/prepared/llama-server \
  --model /absolute/prepared/Qwen3-4B-Q4_K_M.gguf \
  --mosquitto /absolute/prepared/mosquitto \
  --mosquitto-libs /absolute/prepared/native-broker-libraries \
  --chromium /absolute/prepared/chrome \
  --output /absolute/new-evidence-directory
```

Start as an ordinary user. The harness creates a fresh rootless network
namespace, starts only private loopback endpoints, and reaps its processes.
It uses the real C++ HttpServer/API/RuntimeWorker/ConfigStore and real MQTT
workers. Only the model interface is relayed to actual CPU Qwen; raw model
outputs and token usage are retained. It observes real MQTT packets, checks
positive controls, and verifies no publish for negative commands, Preview,
suggestions and Save/Apply. Optional Chromium drives visible Preview controls
against that server, including actual Qwen inference. Nested failure is FAIL.
Use a fresh output path for every run; preserve failed evidence.

CPU GGUF verification is genuine Qwen inference, **not** RKLLM W8A8/NPU,
microphone, wake, ESP32 or production acceptance. Prepare a fresh immutable
aarch64 candidate using existing `scripts/m20-arm64-candidate.py`, the pinned
SDK, `stage-release.py verify-candidate`, and the existing supervised activation
and recovery procedures. Do not reuse consumed deployment plans or change
working service/configuration/model pointers during preparation.

In the later owner window, verify on the actual runtime model and ESP32:
`Вырубай свет на кухне` → OFF; `Да будет свет на кухне` → ON;
`Не выключай свет` → no execution; ambiguity/contradiction → no execution.
Also verify `Можешь включить свет, пожалуйста`, `А можешь отключить свет на
кухне`, `А отключи-ка свет на кухне`, and `Можешь свет включить на кухне`;
state/ability questions and negative/conditional versions must not execute.
Verify the route in diagnostics and independently observe the ESP32 state.
Stop on model admission, resource, safety or recovery failure.
