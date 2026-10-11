# M19 / #16: Russian device speech forms

Engineering fix in the existing Draft [PR #15](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/15).
Status: **READY_FOR_CHIEF_ENGINEER_REVIEW** after 83/83 serial CTest and
41/41 frontend tests, typecheck and build/offline asset checks.
Live acceptance remains open under [Issue #16](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/16).
The previous scripted M19 success did not establish genuine Qwen proposal quality.

## Confirmed failure and alias provenance

Read-only owner configuration on 2026-10-11: revision 2, active hash
`688690e892d2f59413cdc2f7d81f1e129ab9ac0dd247f84b2898d74c3c3cdd18`.
`device_lampa` has metadata alias `лампа` and these spoken aliases:
`лампа`, `лампа_включенная`, `лампа_включит`, `лампа_выключенная`,
`лампа_выключит`. The exact original STT command `включи лампу в кабинете`
has no grounded device. Short-word fuzzy protection correctly refuses to guess.
No voice HTTP POST occurred; the earlier two POSTs were `source=admin-device`.

The source chain for the owner's recorded request is:

1. `RuntimeWorker.cpp` dialogue → `DialogueOperations.h::expandDialogueOperations`:
   typed `location` / `power_device` tuples contain names, not speech aliases.
2. `AdminCopilot.cpp`, branch `create_device`: absence of explicit aliases seeds the
   lowercased name in **both** `spoken_aliases` and `metadata.aliases`.
   `power_only` from `config/admin-templates.json` supplies state/actions;
   it does not append action or state words to device aliases.
   `deterministicId` creates separate transliterated IDs, never spoken aliases.
3. `SetupSession.h` enriches every new entity through `language_profile`.
   `AdminLanguage.cpp` decodes the model's array, normalizes whitespace/case,
   deduplicates and merges it with seed aliases. It does not synthesize suffixes.
   The session writes the result to `spoken_aliases` only; metadata stays seeded.
4. Frontend `stores/setup.ts` keeps proposal and accepted plan separate.
   `adapters/setup.ts::applyLanguage` copies suggested aliases; it does not
   inflect or concatenate them. Review/Apply and ConfigLoader/ConfigStore
   persist the reviewed list without grammatical enrichment.

Read-only `/var/lib/firefly/store/setup-draft.json` records the exact owner request
`Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать`,
one completed step, and five first-attempt `decode` traces: dialogue, room,
device, two actions. This agrees with the enrichment path above. The persisted
trace deliberately excludes raw model text (`SetupSession::invoke`), so the
word-for-word Qwen response cannot be reconstructed. The extra aliases enter
through the language-profile result in this documented flow; no code path
in the dialogue/template/frontend/store generates the underscore pseudo-words.
Do not treat the missing raw response as a captured Qwen transcript.

## Minimal correction

- Device language-profile input contains only the existing **name**, excluding
  technical device/action IDs, states and old pseudo-word aliases as model cues.
- The Admin prompt asks for 1..6 natural forms: nominative and accusative direct
  object after `включи/выключи`, agreement across multiword names, preserved
  distinctive words/numbers. Identical forms need only one entry. Examples use
  a socket and fan; no device-name exception or morphology engine is added.
- Newly proposed device aliases containing `_` are rejected, alongside existing
  unsafe-language and conflict checks. Existing reviewed aliases remain merged;
  this fix does not silently migrate or remove historical configuration data.
- AI proposal and accepted plan display **Речевые формы** before acceptance.
  Existing final Review already displays **Обращения**. The editor explains the
  required object form; rejected enrichment now shows the server's language
  warning instead of silently displaying only the seed name.
- Device editor AI assistance uses the same existing `language-profile` route
  as automatic Setup enrichment, rather than the older metadata draft prompt
  that asks for names/location forms without the command's object case.

No changes to ConfigStore/schema, EntityResolver, index matching, SafetyGate,
compact runtime protocol, command prompt v5, voice/STT/VAD or HTTP admission.
The existing bounded Admin format retry remains unchanged; no new retry,
automatic Apply, voice execution, HTTP resend or Observe polling is introduced.

## Regression and proof

`tests/language/m19-owner-semantic.json` contains the actual owner semantic
entities, IDs, state, metadata and aliases from the read-only configuration.
Connections/bindings/observations are explicitly removed to make the fixture
incapable of addressing production or equipment. It is not a full-store backup.

`M19RussianDeviceProfile` checks the exact missing-case failure even with a
model willing to output the configured target, proposal isolation, exact ON/OFF
after explicit isolated generation activation, retained historical aliases,
unsafe proposals, negative/multiple/question commands, collision warnings and
fail-closed ambiguity. It also checks socket, adjective + numbered socket, fan,
relay and no-zone devices. These supplied valid proposals are decoder/runtime
regressions, not actual model inference.

`M19DemoVerticalSlice` adds a browser replay of the **actual bad alias array**:
the decoder rejects it, the warning and seed forms are visible, rejection/cancel
leave the private store unchanged and perform zero HTTP. Its two successful
desktop/mobile paths still use explicitly scripted model profiles and synthetic
speech, with real C++ backend, Vue/Chromium and isolated HTTP transport. They
check visible forms at proposal and final Review, one voice POST per command,
explicit independent GET, unsafe/no-wake zero sends, restart and LKG recovery.
No fixture is evidence that the revised genuine Qwen proposal succeeds.

Commands and final test totals are recorded in the PR and engineering report.
[Post-fix review proof](evidence/m19/alias-review-proof.json) records actual
owner provenance, exact fixture HTTP and voice execution history, test input
hashes and the pre-commit binary build identity.
Raw logs and fresh browser artifacts are under `build/m19-alias-evidence/`
and `build/m19-acceptance/`. The earlier `docs/evidence/m19/review-proof.json`
is a **historical** pre-fix scripted snapshot, not post-fix or live acceptance.

An intermediate full run found an old Chromium text assertion that omitted the
new visible speech forms; the assertion now includes them. That run also
overlapped a final relink, temporarily making the private test binary unavailable
to `AdminV2Migration`. Final verification runs serially after the completed build;
neither intermediate failure is reported as a passed full run.
One later full run completed both M19 HTTP/voice scenarios but failed the final
browser protocol-error aggregate after page shutdown; the focused diagnostic
repeat passed. The harness now preserves full protocol error details and records
only the specific invalid-interception cancellation of a private diagnostic GET
during explicit `about:blank` shutdown. Mutations, external requests and actual
JavaScript exceptions still fail. No request is retried by this handling.

## Owner acceptance after independent review

The running production service/configuration is preserved. This change has
not been installed, live Apply has not been called and no second NPU/mic owner
has been started. Candidate selection and live Apply require a separate
owner-authorized procedure after Chief Engineer review.

1. With the candidate running under the approved single-owner procedure, create
   a fresh Cabinet + Lamp through genuine AI Setup. Before accepting, check
   visible `лампа, лампу`, correct `в кабинете`, no underscore pseudo-words;
   record proposal quality/timing. Do not hand-edit aliases to claim AI success.
2. Repeat with a socket/multiword numbered socket, fan or relay, and a no-zone
   device. Review exact HTTP facts, aliases and conflicts before owner Apply.
3. After the separately authorized owner Review/Apply, say both natural ON/OFF
   commands. Correlate original STT, `INDEX_EXACT`, SafetyGate PASS,
   execution **source=voice**, one exact POST, then one explicit independent
   GET and Confirmed. Existing manual POSTs do not satisfy this gate.
4. Check negative/ambiguous commands produce no POST. If genuine AI still omits
   or misinflects a form, record its proposal and stop acceptance; the prompt
   and syntax checks cannot guarantee morphological quality for arbitrary names.

Historical pseudo-word aliases are retained until the owner explicitly edits
and reviews them. No global stemming or short-word fuzzy expansion is a fallback.
PR stays Draft; Issue #16 remains open until this genuine model/voice gate passes.
