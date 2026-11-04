# PR #43 — focused dialogue review remediation

This follow-up addresses the four P2 findings in the owner-provided independent
review of `53429eb218e4966c5e98f60c59ea2df3be2480a7`.
It changes the operational dialogue layer and its RuntimeWorker call site.
MQTT/HTTP executors, SafetyGate, bindings, models, TTS and AI Setup are unchanged.

## Fixes

| Finding | Resulting behavior |
| --- | --- |
| R1: command/question boundary | A location prefix alone does not claim an entire request as a question. After exact entity-span analysis, only a bare elliptical continuation can enter the context route; explicit requests retain the original CommandEngine/SafetyGate path even with questions disabled. Feminine and plural state predicates are recognized. Configured names are data and cannot choose the question kind. |
| R2: unknown device + known room | Room membership is used only after question-frame coverage succeeds. Unrecognized object or qualifier words require clarification; they cannot select another room device. Qwen's bounded question code cannot override this check. Failed/ambiguous scope clears the old pronoun referent. Existing DomainView room disambiguation, including shared aliases across rooms, remains authoritative. |
| R3: scoped failure hides latest rejection | A rejected request retains a target only when the original exact entity resolution and existing request-form analysis establish one. The original transcript is not persisted or rewritten. Missing/ambiguous association produces an explicit uncertainty response instead of an older successful result. A genuinely newer matching execution event still wins, and another device's known rejection is not attributed to the queried device. |
| R4: last-action referent lost | A current-generation last-action result can establish its actual, existing target as the read-only referent. TTL, follow-up limit and generation/hash invalidation remain enforced. Dialogue context never supplies a command target. |

The question frame is deliberately conservative. Unsupported extra words require
clarification; this does not add an open-ended entity resolver or let Qwen invent
names. Existing configured aliases are still required. No device/action names
are hardcoded in the new scope guard.

## Reproduction and regression evidence

`OperationalReviewTests.cpp` links the real project ConfigLoader, DomainView,
DeviceStateStore, OperationalDialogue, RuntimeWorker, CommandEngine and
SafetyGate. Only inference and STT are explicit doubles. The supplied archive's
simplified data/resolver scaffolding was not used for acceptance.

Before the product fix, the first 28 checks reported **16 failures** spanning all
four findings. Both question modes also proved that the unmodified command path
accepts the exact original “А на кухне включи свет”; the voice question filter
was the component preventing executor admission.

The expanded regression includes negative/positive controls for unknown objects,
unknown locations, mixed known/unknown objects, malicious Qwen classification,
shared aliases in different rooms, known and uncertain rejection association,
newer history, last-action pronouns, feminine forms, expiry, turn limit and
old-generation history. Its worker controls also verify that “А включи его”
never borrows a dialogue target.

`OperationalVoiceHttp` uses the actual C++ server/runtime/ConfigStore and a real
private loopback HTTP endpoint. Exactly four explicit command utterances produce
four exact POST payloads. Review questions, unknown objects, pronouns and
pre-execution rejections produce zero additional POSTs. The location-prefix
positive controls run with questions disabled and enabled; existing live
Save/CAS/Apply and persisted restart checks remain in the same fixture.

Results: **50/50 focused checks**, **80/80 registered MQTT=OFF CTest**,
**83/83 registered MQTT=ON CTest**, frontend typecheck/build and **66/66 tests**,
and explicit headless Chromium against the real C++ HTTP fixture all passed.
After the final adjustment treating configured aliases as data, the three
operational suites were rebuilt/repeated in both MQTT modes: **3/3 each**.
The cross-built clean-HEAD ARM64 artifacts and their pins are reported separately
in the PR and private evidence. Optional DNS namespaces and legacy DISPLAY-gated
browser registrations were not added to or counted in this follow-up matrix.

Private logs and reproduction sources are under:
`/home/siovush/firefly-m19-control/evidence/operational-voice-f2-pr43-remediation-20261101`.

The initial hardware acceptance and its limits remain documented in
[the original verification](operational-voice-dialogue-verification.md).
This logic-only remediation does not claim a new microphone/STT/Qwen/F2 hardware
acceptance, subjective listening, acoustic echo acceptance or long soak.
No Firefly service, live ConfigStore, broker, models or release pointers were
changed by this follow-up. The existing F2 latency and cooperative SDK
cancellation limitations are unchanged. The PR remains Draft; no merge/release.
