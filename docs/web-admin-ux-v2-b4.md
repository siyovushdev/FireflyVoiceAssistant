# Web Admin UX V2 — B4 default migration

Исходный отчёт: 2026-10-04. Ветка: `feature/web-admin-ux-v2-b4`. B4 functional owner acceptance **PASS** по сообщению владельца после исправления AI Setup. Последний запрос владельца — foreground busy UX polish; его проверки приведены в дополнении от 2026-10-08 ниже. Исторические результаты далее сохранены с контекстом. B3 завершён владельцем (manual acceptance и real browser voice/setup smoke PASS), PR #2 merged в `master`: `a1fd5be64261ceb14abfaab4b49f703e135c683e`.

## Default UI и совместимость URL

**Owner acceptance URL: http://localhost:8080/**. AI setup: **http://localhost:8080/#/setup**.

Vue 3 + TypeScript + Vite теперь является единственным Web Admin. `/`, `/index.html`, `/v2`, `/v2/`, `/v2/index.html` возвращают один embedded entry; hashed resources всегда находятся в `/assets/`. `/v2/` сохранён как дешёвый alias для B1–B3 bookmarks, без отдельной сборки или набора assets. Router использует root hash history.

Работают `#/overview`, `#/setup`, `#/zones`, `#/devices`, `#/devices/:id`, `#/connections`, `#/states`, `#/advanced`, `#/advanced/playground`, `#/diagnostics`; refresh device detail сохраняет fragment. Старые vanilla bookmarks `#overview`/`#diagnostics` открывают соответствующий route. Остальные anchors:

| Старый fragment | V2 route |
| --- | --- |
| `#copilot-section` | `#/setup` (только просмотр, без Start) |
| `#locations-section` | `#/zones` |
| `#devices-section` | `#/devices` |
| `#capabilities-section` | `#/advanced` |
| `#playground` | `#/advanced/playground` |
| `#states-section` | `#/states` |
| `#revisions-section` | `#/advanced` |
| Неизвестный fragment | `#/overview` |

Неизвестные **server paths**, assets и `/api/*` сохраняют 404; HTML не подставляется вместо API или отсутствующего JS. Query/encoded paths по-прежнему отклоняются транспортом. Старые `/admin.js`, `/admin.css` и editor scripts больше не обслуживаются.

## Удалено и сохранено

Удалён каталог `src/admin/web/`: `index.html`, `admin.js`, `admin-model.js`, `admin.css`, `typed-editor.js`, `simple-editor.js`, `connection-editor.js`. Удалены CMake COPYONLY/`FIREFLY_ADMIN_ASSETS`, filesystem asset loader и vanilla-only fallback; Admin build без verified Vue dist завершается понятной ошибкой. Core-only build возможен с `FIREFLY_BUILD_ADMIN=OFF`. Убраны ссылка «Открыть прежний интерфейс» и неиспользуемый `PlaceholderView.vue`.

Удалены пять зарегистрированных тестов, зависевших от vanilla implementation: `AdminDraftUiModel`, `DialogueSetupWebModel`, `LanguageAdminChromium`, `ConnectivityWeb`, `DialogueSetupChromium`, а также exclusive fixtures `LanguageUiWorker.cpp` и `SetupUiApp.cpp`. Также удалены шесть незарегистрированных ручных vanilla DOM harnesses: `WebUiTests.js`, `UnifiedWebUiTests.js`, `WebAdminTests.js`, `TypedWebTests.js`, `OfflineWebTests.js`, `FinalUxWebTests.js`; они обращались к удалённым DOM IDs/global JS state. Их backend контракты остаются в `GeneralizedAdmin`, `LanguageAdminProfile`, `DialogueSetup`, `ConnectivityHttp`, `AdminTranscribe`; пользовательское поведение покрывают frontend TS tests и production-bundle `AdminManualChromium`/`AdminSetupV2Chromium`.

| Сохранённое поведение удалённых frontend suites | Текущая проверка |
| --- | --- |
| Offline editing при недоступном model/runtime: локальный план сохраняется, final Apply заблокирован | frontend `manual.test.mjs` offline regression; `AdminHttp` offline Validate/Save contract |
| Typed scalar constraints, immutable candidate, diff/shared consumers, errors, cancel/review/CAS | frontend `manual.test.mjs`, `adapters.test.mjs`, `AdminManualChromium`, backend `GeneralizedAdmin` |
| Owned setup, duplicate/stale requests, entering/reload, cancel/restore, no automatic Send/Apply | frontend `setup.test.mjs`, `DialogueSetup`, `AdminSetupV2Api`, `AdminSetupV2Chromium` |
| Local voice capture/WAV, editable transcript, explicit Send, release tracks | `AdminSetupV2Chromium` browser capture fixture, `AdminTranscribe`; B3 owner real smoke PASS |
| Language suggestions editable/reviewed; conflict rejection, LanguageIndex deterministic matching | `LanguageAdminProfile`, `LanguageIndex`, `LanguageComposition`, B3 contextual browser lane |
| Explicit HTTP facts, TCP probe 0 HTTP bytes, ON/OFF payload, Observe, Predicted/Confirmed | `ConnectivityHttp`, `AdminManualApi`, `AdminManualChromium`, `AdminSetupV2Chromium` |

Backend source, schema v2, ConfigStore и runtime architecture не менялись. Перед удалением проверены URL references vanilla JS, Vue API allowlists/call sites, C++ dispatch и tests. **Ни один endpoint не удалён**, включая совместимые APIs, которыми Vue напрямую не пользуется:

| Endpoint family | Потребители и причина сохранения |
| --- | --- |
| `/api/status`, `/api/dashboard`, `/api/config`, `/api/config/active`, `/api/devices/state`, `/api/executions` | Vue overview/catalog/diagnostics/states, runtime evidence, HTTP/API tests |
| `/api/config/validate`, `/api/config/diff`, `/api/config/apply`, `/api/config/reconcile` | manual/owned final review, authoritative validation, CAS/recovery; Apply только явно |
| `/api/config/export`, `/api/config/import`, `/api/config/migrate` | Vue Advanced import/export/migration; `ConnectionAdmin` contract |
| `/api/config/revisions`, `/api/config/revision`, `/api/config/restore-candidate` | Vue revisions и reviewed restore candidate |
| `/api/config/restore` | совместимый API restore; `AdminGenerationApi`, `LanguageAdminProfile`; не заменяется скрытым Apply |
| `/api/admin/draft` | Vue Contextual AI Assist; server-authoritative draft validation |
| `/api/admin/id`, `/api/admin/templates` | совместимые ID/template APIs в C++ dispatch; удаление vanilla callers не меняет backend contract |
| `/api/admin/language-profile`, `/api/admin/action-draft`, `/api/admin/copilot`, `/api/admin/copilot/accept`, `/api/admin/transcribe` | Vue Contextual AI Assist, setup semantic lane, explicit acceptance, local voice; inference остаётся Qwen |
| `/api/admin/plan`, `/api/admin/suggest` | совместимые Admin API; `AdminSuggestionRetry`, `GeneralizedAdmin` |
| `/api/setup/status`, `/api/setup/start`, `/api/setup/resume`, `/api/setup/heartbeat`, `/api/setup/message`, `/api/setup/draft`, `/api/setup/apply`, `/api/setup/cancel` | owned setup lifecycle, entering/reload, version/CAS, heartbeat/disconnect/cancel/restore |
| `/api/connections/probe` | Vue manual wizard, TCP-only probe; 0 HTTP bytes |
| `/api/connections/test` | совместимая диагностика и `ConnectivityHttp`; удаление старого frontend не удаляет endpoint |
| `/api/devices/command`, `/api/devices/poll`, `/api/test-command` | guarded actions, explicit readback, Playground и backend evidence/security tests |

`POST /api/devices/observe` удалён в [M18.3](m18-production-api-hardening.md),
возвращает 404. Chromium evidence fixture теперь получает Confirmed через реальные
HTTP GET к локальному mock device; production UI не использовал удалённый route.

SafetyGate, LanguageIndex semantics, production/Admin Qwen profile/model, CTX/budget/deadline, ownership/CAS и STT/VAD/voice код не менялись. B3 power-only HTTP construction остаётся детерминированным из explicit facts через общие B2 semantics; unsupported cases используют ручной wizard. Accept != Apply, никакого hidden hardware из AI/review или first page load.

## Static package

```text
Vue + TS + Vite (Node только на build машине)
  → offline dist + digest manifest
  → package-admin-v2.py (build-time Python)
  → generated AdminV2Bundle.h
  → C++ HttpServer с exact URL allowlist
  → target без Node/npm/dist/frontend source
```

`FIREFLY_ADMIN_DIST_DIR` по умолчанию указывает на `src/admin/frontend/dist`, либо принимает заранее собранный dist того же release. CMake не вызывает npm, не скачивает зависимости, не обслуживает произвольные filesystem paths. Manifest не публикуется. Symlinks/traversal, duplicates, undeclared types, oversized/corrupt assets отклоняются packager.

Все ответы сохраняют MIME, `Cache-Control: no-store`, `nosniff`, `Referrer-Policy: no-referrer`, прежний self-only CSP. Один hashed JS, один CSS и локальный SVG; CDN/fonts/telemetry/external requests не добавлены. Browser gate проверяет весь bundle, не только строки HTML. Read-only test запускает C++ server с `PATH=/nonexistent`, подтверждая отсутствие Node runtime dependency.

## Parity и verification

Все проверки используют новые temporary stores и loopback equipment fixtures. `user-store/`, прежний `build/`, `node_modules/` и прежние private artifacts не редактируются и не публикуются; существующие SDK/STT/wake dependencies используются только для чтения. Новая сборка и evidence: `/tmp/firefly-b4-ugqzHE/` (не входят в Git).

| Flow | Проверка |
| --- | --- |
| Overview/Zones/Devices/detail/States/Connections, stored vs active, Predicted/Confirmed/Unknown/Stale | `AdminV2Chromium` через `/` |
| Root, aliases, all old anchors/unknown fragment, deep-link refresh, 1280×800 / 800×600 / 390×844 | `AdminV2Chromium`, real C++ static assertions |
| Manual editing, typed boolean/integer/number/string/enum, shared connections, wizard | `AdminManualChromium` + `AdminManualApi` |
| AI semantic setup, Contextual AI Assist, editable voice composer, no-zone/non-home | `AdminSetupV2Chromium` + `AdminSetupV2Api` (inference scripted IPC seam) |
| Deterministic HTTP connectivity, incomplete facts/failure preservation, final combined review/Apply | `AdminSetupV2Chromium` + frontend tests |
| Guarded ON/OFF, explicit Observe, TCP probe zero bytes, no hidden hardware | B2/B3 Chromium + API, `AdminGuardedCommand` |
| Revisions/import/export/recovery/Playground and saved-not-active failure | `AdminManualChromium` + backend/API suites |
| Existing B3 store with connections/bindings/observations, identities and byte-for-byte rollback | `MigrationTests.py` against B3 and B4 binaries |

B4 does not retune or newly benchmark real NPU/STT: the functional browser seams run real C++ API/ConfigStore/runtime with scripted inference; B3 real NPU and owner real browser voice acceptance remain the prior evidence.

## Results — 2026-10-04

| Check | Result |
| --- | --- |
| Frontend typecheck | PASS (`vue-tsc --noEmit`) |
| Frontend tests | **35/35 PASS**, including offline-plan regression |
| Production build + offline gate | PASS; 4 digest-verified local assets, 252,938 embedded bytes |
| Fresh CMake configure/build | PASS; full Admin/assistant/voice/config-tools build in new `/tmp` directory |
| Focused Setup/B2/B3/B4 APIs and Chromium | PASS; root gate rerun after correcting test wait for loaded catalog |
| Final full CTest | **68/68 PASS**, 226.24 s |
| Root/compatibility/bookmarks/refresh + read-only Chromium | PASS, 62.30 s in final CTest; three viewports, zero external requests/mutations/hardware |
| Manual B2 Chromium | PASS, 32.28 s; explicit HTTP commands/readback, TCP probe 0 HTTP bytes |
| Setup B3 Chromium | PASS, 36.31 s; semantic/contextual scripted inference, voice fixture, deterministic connectivity, reviewed Apply |
| Migration + release rollback with B3 main and B3 IPC worker | PASS, B3 → B4 → B3 → B4, unchanged applied config/export/revisions/store bytes |
| `git diff --check` | PASS |

Logs: `typecheck.log`, `frontend-tests.log`, `frontend-build.log`, `configure.log`, `build.log`, `root-chromium.log`, `unified.log`, `ctest-final.log`, `rollback.log`; browser screenshots/evidence under `build/v2-acceptance`, `build/b2-acceptance`, `build/b3-acceptance`, all inside `/tmp/firefly-b4-ugqzHE/`. Initial runs found a new alias-test loading race and a legacy inline-DOM assertion in `UnifiedHttpTests.py`; both are corrected. The final full run is green. The latter preserves every runtime/execution assertion and checks Vue entry/assets, while rendered flows run in Chromium.

Reproduction (existing dependencies only):

```sh
cd src/admin/frontend
npm run typecheck
npm test
npm run build
cd ../../..
cmake -S . -B /tmp/firefly-b4-ugqzHE/build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python
cmake --build /tmp/firefly-b4-ugqzHE/build -j4
PYTHONDONTWRITEBYTECODE=1 ctest --test-dir /tmp/firefly-b4-ugqzHE/build --output-on-failure
```

## Release-level rollback

Previous compatible source: B3 merge **`a1fd5be64261ceb14abfaab4b49f703e135c683e`**, including accepted language-profile fix `b4292ec6496d4294202fe25f3d96180bfaee6ca5`. Before B4, copies of the existing compatible `firefly-admin`, `firefly-demo`, B3 IPC test worker, and all seven previous assets were saved to `/tmp/firefly-b4-ugqzHE/rollback-b3/`, with SHA-256 manifest. Existing release files remain intact. Tested B3 `firefly-admin` SHA-256: `40c1c5d440fbb1fe76dd1710340cea660f6fdff7ea15793508a44faad87bab6f`.

Procedure:

1. Stop the current assistant/Admin process normally, releasing its runtime/model ownership.
2. Restore the previous compatible **binary/library/assets release package** and its launch arguments. B3's vanilla asset path was compiled into the binary; preserve that release's configured asset location. For this installed B3 artifact it is `/home/firefly/FireflyVoiceAssistantLinux/build/src/admin/web`. A relocated binary alone is insufficient; retain or restore its matching assets at that path. B4 embeds all Vue assets and does not need that directory.
3. Restart with the **same existing store**. Do not restore `configuration.json`, backups, last-known-good or revisions from an earlier date. Schema/config do not change in B4.
4. Check stored/active revision+hash, config export, connectivity mappings and runtime readiness. Opening the UI must not save/apply or execute hardware. Operational changes require the usual explicit guarded action.

`MigrationTests.py <B4 firefly-admin> <B4 IPC worker> <repo> <B3 firefly-admin> <B3 IPC worker>` exercises B3 → B4 → B3 → B4 using one private store. B4 performs an explicit validated Save and Apply; rollback checks the exact applied identity, configuration/export/revisions and every store file byte without automatic config rollback. This is a release rollback, not a new recovery subsystem.

## Owner review artifact

Review demo запущен из новой сборки: **http://localhost:8080/**, AI setup **http://localhost:8080/#/setup**. Real C++ `firefly-demo` отвечает `/` и `/v2/` с Vue module entry и `no-store`; `/api/status` сообщает `runtime_ready=true`. Он использует новый `/tmp/firefly-b4-ugqzHE/owner-store`, seeded empty config; существующий `user-store` и прежние private demo stores не используются. Никаких operational actions или AI setup accept/apply от лица владельца не выполнялось.

Launch/restart command (stop the current process first):

```sh
/tmp/firefly-b4-ugqzHE/build/firefly-demo \
  --store /tmp/firefly-b4-ugqzHE/owner-store \
  --init-config /home/firefly/FireflyVoiceAssistantLinux/config/empty-home.json \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-bind 127.0.0.1 --admin-port 8080 \
  --stt-model-dir /home/firefly/FireflyVoiceAssistantLinux/build/stt/model
```

Artifact и review store находятся в `/tmp` и могут исчезнуть после reboot; для постоянного release deployment следует сохранить целый новый binary/library package отдельно от configuration store. Node на target не требуется.

## Limitations and owner gate

B4 owner acceptance remains pending. Default UI migration adds no cloud/auth/HTTPS/new executors, no schema migrations and no model/prompt/voice changes. B3 real voice owner PASS is retained; automated voice uses a browser capture/STT fixture and is labelled accordingly. `/v2/assets/*` is deliberately absent: old `/v2/` entry bookmarks load the same current root assets, and no stale second bundle is retained. Offline upgrades should replace the binary as a whole; open tabs should refresh for the matching bundle.

## Owner blocking defect: compound semantic creation — 2026-10-08

Owner acceptance remains **blocked pending recheck** of this fix. This section describes uncommitted corrections on top of `37c0253`; it does not replace historical B4 results above. Routing/delivery are unrelated: the owner rebuilt B4 and confirmed `/` and `/v2/` return 200 while `/admin.js` and `/admin.css` return 404.

### Reproduction and exact rejection

Original request:

```text
создай цех зону и добавь туда вентилятор где он может включаться и выключаться по питанию
```

A private diagnostic executable linked to the unchanged current Admin libraries called `RuntimeWorker::handle(dialogue)` with `config/empty-home.json`, empty focus, the real `RuntimeModel`, normal setup entry and the existing `structuredSuggestion` trace. No new production logging was introduced. Evidence directory: `/tmp/firefly-b4-dialogue-debug/`; `Before.cpp`, `before` and `before.json` preserve the reproducer and complete prompt/input/raw/metrics/error for both attempts. The owner's demo was stopped normally for exclusive NPU use; its arguments were retained for restart. Production references and `user-store` were not modified.

Exact first raw output (the blank line is present):

```text
{"ops":[["location","цех"],["power_device","цех","вентилятор"]]}

{"unsupported":"параметр 'where он может включаться и выключаться по питанию' не поддерживается"}
```

Exact retry raw output:

```text
{"ops":[["location","цех"],["power_device","цех","вентилятор"]]}}
```

First attempt: JSON parse error 101, line 3 column 1, unexpected `{`, expected end of input. Retry: JSON parse error 101, line 1 column 81, unexpected `}`, expected end of input. The decoder correctly refuses both complete strings. Neither attempt reaches operation expansion, semantic validation or proposal conversion.

| Before fix | System tokens | Input tokens | Output tokens / budget | Prefill ms | Generation ms | Inference wall ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| First | 511 | 44 | 48 / 384 | 858.530 | 15021.60 | 15906 |
| Retry | 605 | 44 | 22 / 384 | 878.443 | 6866.61 | 7765 |

Both finish below the output limit; truncation is not the observed cause. `DialogueOperations.h` legally accepts one `ops` array containing `location(name)` followed by `power_device(location,name)`. It expands to `create_location` and `create_device(template=power_only)` and delegates references, protected entities and config validation to `decodeCopilotProposal`. The requested expected object is legal.

The creation-specific prompt displaced the general prompt's explicit single-object contract with separate complete-object examples. The observed model treated the relative clause describing ON/OFF as an unsupported extra parameter and appended a second alternative. The old retry added a generic syntax instruction plus parser error; it generated an extra brace. This is an Admin generation-contract reliability defect, not a reason to accept partial JSON. B4 did not change these prompts or decoders relative to B3. Historical B3 checks used other wording, including a pre-existing selected zone; their PASS does not establish coverage of this wording. The evidence does not establish model nondeterminism or a KV reset bug. Sampling remains top_k=1; request output and KV are cleared through the existing synchronous path. The second object has different content, not a duplicate copy of the first object.

A separate deterministic defect affected `В цехе есть вентилятор, его можно включать и выключать.`: an unresolved-pronoun guard returned “Какое устройство изменить?” before inference even on an empty system. An intermediate diagnostic that deferred this guard then exposed wrong arguments from the general prompt (`power_device("вентилятор","включить")`, similarly for выключить); reference validation rejected the nonexistent zone. `probe.json` preserves that intermediate failure, not a passing final gate.

### Changes and retained boundaries

- Creation prompt uses one combined envelope, explicit tuple arities, mutually exclusive `ops/question/unsupported`, and a stop-after-object instruction. ON/OFF descriptions are capabilities rather than additional arguments. A generic pavilion/heater example teaches location/device/capability roles; no owner-specific entity or phrase mapping was added.
- Explicit no-zone requests receive a generic portable-generator example preserving qualifying words in the device name. An intermediate live run produced `ИБП` instead of `Серверный ИБП`; it was recorded as FAIL despite correct null scope and empty capabilities.
- Descriptions using the word `есть` can use this same bounded creation contract; edit/rename/delete/range requests remain outside that shortcut. It selects a prompt, not an executable action or repaired proposal.
- An unresolved pronoun can refer to exactly one newly proposed device in the same request. Existing-entity edits and multiple new devices remain ambiguous; all original authoritative validation still runs.
- The one allowed retry retains the concrete rejection reason and explicitly prohibits second objects and extra closing braces. Runtime failures still do not get format retries.
- A regression with trailing Russian prose revealed that nlohmann's parser `what()` can contain an incomplete UTF-8 excerpt. JSON parse failures now record safe error ID/byte offset and `rejection_stage=json_parse`. Raw response bytes remain unchanged. This fixes diagnostic serialization, not model output.

The exact reference RKLLM 1.3.0 header exposes no ready-made JSON-schema/grammar constrained-decoding parameter. `rkllm_set_function_tools` configures prompt/tool descriptions, not a documented hard grammar guarantee. A new logits-based decoder was not introduced. Models, sampling, context limits, response budget 384, deadline, chat template, KV reset, production commands, SafetyGate and B4 delivery remain unchanged.

`parseMessage`, `expandDialogueOperations` and `decodeCopilotProposal` are unchanged. No regex extraction, first-object selection, output repair, automatic Accept/Apply/probe/poll or equipment command was added. Server draft proposals still require human acceptance in Vue; publication still uses the existing explicit global review.

### Regression and live procedure

`AdminDialogueContract` exercises acceptance A–G: owner input, descriptive paraphrases with different entities, explicit no-zone, malformed syntax, extra brace, second object, trailing Russian prose, markdown, unknown operation/arity, unknown references and duplicate zones. Invalid attempts preserve an existing manual checkpoint, stored/active identity and empty execution history. It verifies invalid→valid and invalid→invalid retry behavior, plus retained ambiguity of ungrounded edits and multiple devices. Unknown references retain the existing clarification result, not an invented zone or a syntax retry. `AdminSetupV2Chromium` now also uses visible controls for the original phrase, natural paraphrase and no-zone, verifies proposal cards and rejects them without publication or hardware requests. Scripted model output in those tests is not evidence of live model quality.

The separate `benchmarks/web-admin-b4/DialogueSmoke.py` launches the actual rebuilt B4 `firefly-demo`, real RKLLM and a new private store. It requests C, B and A three times through the real HTTP setup API, resetting and checking the entire draft/focus/unresolved-text checkpoint between requests. The model receives no transcript history; the normal per-request KV clear remains in effect. This is equivalent isolation without repeatedly loading the same model. It checks exact ON/OFF structure, nullable zone, unchanged stored/active identity and zero executions; it never calls Accept, Save, Apply, probe or hardware endpoints.

Fresh commands and final results are recorded below after execution. Earlier failed diagnostic/test iterations remain in the evidence directory and are not labelled PASS.

### Fresh live result (final source, 2026-10-08)

**PASS: A 3/3, B and C.** This is a new real RKLLM run through the rebuilt B4 binary and C++ HTTP API, not the historical B3 acceptance or a scripted response. Final evidence: `/tmp/firefly-b4-dialogue-debug/live-verified.json`, `live-verified-run.log`; private store and server log: `/tmp/firefly-b4-dialogue-live-0j0g0fmf/`. The binary SHA-256 is `33870accc79117b3cea09266852761adf791e76600eb9e16cbdb1502fc2da751`.

| Case | Semantic attempts | System tokens | Semantic inference wall ms | Complete setup message wall s | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| Original A1 | 1 | 853 | 8554 | 69.880 | Цех → Вентилятор → ON/OFF |
| Original A2 | 1 | 853 | 8717 | 69.326 | Same correct proposal |
| Original A3 | 1 | 853 | 8613 | 69.348 | Same correct proposal |
| Natural B | 1 | 853 | 8681 | 68.107 | Equivalent semantic proposal |
| No-zone C | 1 | 960 | 6697 | 33.544 | Серверный ИБП; null zone; no invented actions |

All five semantic attempts finished below the unchanged output budget 384, with no retry. All language enrichment attempts also succeeded without warnings. The complete-message timing includes context changes and language enrichment, not only the semantic inference. The empty-context creation prompt is 4578 UTF-8 bytes; no-zone has an additional bounded instruction. The existing system-token ceiling for this budget remains 1664 within CTX4096; neither context nor sampling was increased.

Every case verified unchanged stored hash and exact active identity/configuration, empty execution history, no connections/bindings/observations. The runner's only POST paths were setup start, heartbeat, draft, message and cancel. It never published config or sent equipment requests. Cases reset the private draft and explicitly check empty focus/unresolved text; they are not chained requests relying on a previously created zone. The test server was stopped normally at completion.

Intermediate reports remain FAIL: `live.json` passed the original phrase 3/3 but rejected B; `live-final.json` passed B and original 3/3 but shortened the no-zone device name. Neither is substituted for the final complete PASS. The source corrections for B and C use unrelated generic examples, not mappings for Цех/Вентилятор/ИБП.

Commands executed in this work (existing dependencies, no download):

```sh
cd /home/firefly/FireflyVoiceAssistantLinux/src/admin/frontend
npm run typecheck
npm test
npm run build
cd /home/firefly/FireflyVoiceAssistantLinux
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j4
ctest --test-dir build -R '^(AdminDialogueContract|AdminSuggestionRetry|DialogueSetup|AdminCopilot|AdminSetupV2Api|AdminGuardedCommand|AdminSetupV2Chromium)$' --output-on-failure
ctest --test-dir build -R '^AdminDialogueContract$' --output-on-failure
python3 -B benchmarks/web-admin-b4/DialogueSmoke.py \
  --binary /home/firefly/FireflyVoiceAssistantLinux/build/firefly-demo \
  --root /home/firefly/FireflyVoiceAssistantLinux \
  --output /tmp/firefly-b4-dialogue-debug/live-verified.json
PYTHONDONTWRITEBYTECODE=1 ctest --test-dir build --output-on-failure
git diff --check
```

The live runner refuses to overwrite evidence; choose a new output filename when repeating. Stop the existing demo normally before running it: the runner requires exclusive RKLLM ownership. Existing CMake cache retains the installed RKLLM/STT/wake paths and Admin/voice build flags. Builds were repeated after each final C++ change; `build-release.log` is the final successful rebuild. No NPU owner ran concurrently with CTest.

### Fresh automated results and remaining owner gate

| Check run now | Result |
| --- | --- |
| Frontend typecheck | PASS |
| Frontend tests | 35/35 PASS, zero skipped |
| Production frontend build / offline asset gate | PASS, 4 declared local assets |
| Current B4 CMake build | PASS (`build-release.log`) |
| New `AdminDialogueContract` | PASS, also included in full CTest |
| Full CTest on final code | **69/69 PASS**, 234.51 s |
| Actual `AdminV2Chromium` | PASS, 63.90 s |
| Actual `AdminManualChromium` | PASS, 33.27 s |
| Actual `AdminSetupV2Chromium`, including new A/B/C | PASS, 39.29 s |
| `AdminSetupV2Api` / `AdminV2Migration` | PASS, 2.35 / 3.57 s |
| Real RKLLM, rebuilt B4 binary | **A 3/3, B, C PASS**, all semantic attempts without retry |
| `git diff --check` | PASS |

Evidence logs are in `/tmp/firefly-b4-dialogue-debug/`: `typecheck.log`, `frontend-tests.log`, `frontend-build.log`, `configure.log`, `build-release.log`, `contract-final.log`, `ctest-final.log`, `live-verified.json`, `live-verified-run.log`. Final browser artifacts are under `build/v2-acceptance/`, `build/b2-acceptance/`, `build/b3-acceptance/`. The first focused run had a test expectation that incorrectly classified unknown-reference clarification as a syntax failure; the test was corrected to retain the existing safe clarification semantics. Final full CTest includes all relevant API/unit suites and all three real Chromium suites; it is not a core-only result. No historical result above is presented as a newly executed test.

Changed/new repository files for this correction:

- `src/admin/RuntimeWorker.cpp`: bounded Admin creation prompt and same-request pronoun handling.
- `src/admin/RuntimeGeneration.cpp`: explicit single-object retry and safe parse-error diagnostics.
- `src/admin/CMakeLists.txt`: register `AdminDialogueContract`.
- `tests/admin/DialogueContractTests.cpp`: new strictness, semantic, retry and no-mutation regressions.
- `tests/admin/v2/SetupWorker.cpp`: scripted fixture replies for owner A/B/C, no production phrase mapping.
- `tests/admin/v2/SetupChromiumTests.js`: rendered proposal/reject/no-publication checks for A/B/C.
- `benchmarks/web-admin-b4/DialogueSmoke.py`: explicit exclusive real-model HTTP smoke on a fresh store.
- `docs/web-admin-ux-v2-b4.md`: this diagnosis and evidence.

Owner recheck: open `http://localhost:8080/#/setup`, start a new setup session and repeat the original request. Check that one proposal contains Цех, Вентилятор and two power actions; inspect/edit before Accept. Acceptance into the plan must not publish or execute. Then repeat the natural wording and explicit no-zone request in clean drafts. Final Apply remains a separate reviewed operation and was not performed by the live smoke. Automated success does not replace B4 owner acceptance.

Limitations: this verifies the stated cases and strict failure paths, not every Russian wording. A complete setup request still takes roughly 68–70 s for the zone/power-device cases because it also generates language profiles; no latency tuning was attempted. No new microphone/STT or always-on voice acceptance was performed. No cloud, model/context-window changes, transport changes, routing changes, user-store cleanup, commit, merge or push are part of this correction. Existing local `user-store` changes remain untouched.

The owner's demo was restored with its original arguments and `/tmp/firefly-b4-manual`, using the rebuilt binary (PID 974850 at verification). Fresh read-only checks: `/api/status` reported `runtime_ready=true`; `/` and `/v2/` returned 200, `/admin.js` and `/admin.css` returned 404. Owner URL: `http://localhost:8080/#/setup`. No setup message, Accept or Apply was sent to that owner store during restoration.

## 2026-10-08 — foreground busy UX после functional owner acceptance

Владелец сообщил, что B4 functional acceptance пройдена после AI Setup fix.
Последний UX defect: disabled-кнопки без достаточно заметного объяснения ожидания,
особенно при 8–20+ секундах Admin inference. Это дополнение описывает новый frontend
polish; результаты более ранних разделов не являются повторными проверками этой правки.

### Правило и механизм

User-initiated async workflow регистрирует foreground operation; periodic reads этого
не делают. `stores/foreground.ts` хранит независимые operation ID, type, человеческую
подпись и delayed-visible state. Pending устанавливается синхронно; existing workflow
locks немедленно блокируют повторное действие. Индикатор появляется через 250 ms.
Таймер управляет только видимостью: он не запускает запросы, не снимает locks и не
влияет на правильность операций. Быстрый ответ отменяет таймер без вспышки loader.

`foreground.run()` гарантирует cleanup через `finally`. В компонентах с существующей
обработкой результата используется тот же `begin()/stage()/finish()` с `finally`.
Независимые tickets сохраняют остальные операции при завершении одной; поздний
stage/finish завершённого ticket не восстанавливает его и не очищает чужой.
Существующие `manual.busy`/`setupBusy` и локальные guards сохранены, без переписывания
state management или transport queue. Повтор Confirm во время busy становится no-op.
Никаких automatic retries mutation.

`ForegroundStatus.vue` — один компонент и один CSS pattern: spinner + текст,
`role=status`, polite live region, reduced-motion. Он виден в фиксированной полосе
без overlay; внутри native modal используется тот же компонент в sticky region,
поскольку фон native dialog недоступен. `aria-busy` отмечает изменяемые области,
а live region Review находится за их пределами. Навигация остаётся доступной с
прежними consistency guards. Серверные entering/restoring transitions используют
тот же компонент даже после завершения стартового HTTP-запроса; heartbeat не становится
foreground operation.

### Покрытые операции

| Workflow | Feedback |
| --- | --- |
| AI Start/resume, entering, cancel/restoration | Подготовка локального AI / завершение настройки / восстановление ассистента |
| AI prompt, явный retry, contextual language/action assist | Готовим предложение… |
| Accept/reject/edit proposal, ручное закрытие уточнения, явное reread draft | Добавляем в план / отклоняем / перечитываем черновик / подготавливаем редактор |
| Voice permission, закрытие записи и WAV preparation, local STT, WAV upload | Ожидаем доступ к микрофону / распознаём речь локально; отмена/unmount убирают feedback, late result по-прежнему игнорируется |
| Manual create/update/delete через TaskBuffer, candidate validation и global Review | Подготавливаем редактор / проверяем изменения |
| CAS Save → Apply → active verification → refresh | Сохраняем конфигурацию → применяем конфигурацию → проверяем активную версию → обновляем данные |
| Owned Setup Apply | Сохраняем и применяем конфигурацию…; API атомарен для frontend, внутренние этапы не имитируются; затем active verification |
| Manual и setup connection probe | Проверяем подключение…; explicit-only, прежние 0 HTTP bytes |
| Guarded device command, explicit poll, Playground preview/execute | Отправляем команду / читаем состояние / проверяем команду; post-action refresh остаётся внутри busy lifetime |
| Revisions opening/reload, restore candidate, import file/validation/migration, export stored, reconcile, reset base | Подписи соответствующих операций; Apply по-прежнему отдельный Review |
| Явное «Обновить данные» | Обновляем данные… |

Исключены: periodic Overview/States refresh, `/setup/status`/heartbeat, начальное
read-only восстановление session ownership; они не регистрируются в foreground counter.
Периодическое обновление больше не меняет подпись кнопки на «Обновление…».
Синхронный ввод TaskBuffer, фильтры/выбор контекста, локальный Accept AI-полей,
отмена локального редактора и экспорт уже имеющегося candidate не требуют loader.
Запись голоса сохраняет собственный таймер записи; spinner относится к ожиданию
permission/processing, а не заменяет индикатор записи.

API paths/methods/bodies, CAS, backend validation, SafetyGate, guarded command,
HardwareExecutor, AI permissions, B4 routing, model/prompt в этом polish не менялись.
Ранее принятые изменения AI inference остаются отдельными существующими изменениями
working tree. `user-store` не изменялся этой задачей; его tracking здесь не исправляется.

### Проверки именно этой правки

Новый `foreground.test.mjs`: slow AI wrapper/duplicate guard/success/error; fast request
без позднего loader; concurrent tickets; sequential stage identity; slow/failing
background refresh без foreground; реальный manual store Review/Save/Apply/verify
с удерживаемыми promises, double Confirm и failure cleanup. Существующие 35 frontend
regressions сохранены (всего 40 tests).

Chromium fixtures удерживают реальные запросы production bundle через CDP Fetch,
проверяют disabled control и видимый текст до `Fetch.continueRequest`, включая
положение индикатора внутри viewport, и сохраняют screenshots `busy-*.png`.
`AdminSetupV2Chromium`: три AI wording, malformed output cleanup/retry usable,
local-STT delayed fixture. `AdminManualChromium`: slow probe, Save, Apply error,
guarded ON/OFF и state read. Hardware counts/strict plan isolation прежние.
Это реальные Chromium processes + embedded Vite dist + C++ HttpServer, но AI/STT
worker responses тестовые. Новый real microphone/RKLLM smoke этим UX polish не заявляется.

Команды запущены заново для финальных frontend sources, а не перенесены из B3/B4 истории:

| Команда | Результат |
| --- | --- |
| `npm --prefix src/admin/frontend run typecheck` | PASS |
| `npm --prefix src/admin/frontend test` | **40/40 PASS** |
| `npm --prefix src/admin/frontend run build` | PASS; offline gate: 4 declared local files, hashed JS/CSS, no external HTML/CSS resources |
| `cmake --build build -j2` | PASS, включая embedded Admin и `firefly-demo` |
| `ctest --test-dir build --output-on-failure` — первый полный прогон | **68/69**, 246.66 s; `AdminSetupV2Api` получил `409 setup_busy` на Resume после асинхронного Cancel |
| `ctest --test-dir build -R '^AdminSetupV2Api$' --output-on-failure --repeat until-fail:3` | **3/3 PASS**, 6.92 s, без изменения исходников или fixture |
| `ctest --test-dir build --output-on-failure` — повторный полный прогон | **69/69 PASS**, 245.53 s; Chromium root **62.31 s**, manual **45.26 s**, setup **42.30 s**, API **2.35 s** |
| `git diff --check` | PASS |

В первом полном прогоне реальные Chromium: `AdminV2Chromium` **62.30 s PASS**,
`AdminManualChromium` **45.25 s PASS**, `AdminSetupV2Chromium` **42.30 s PASS**.
Предварительный targeted Chromium run также прошёл 3/3; окончательные результаты
после последних ARIA/cleanup правок выше. Артефакты:
`build/b2-acceptance/busy-{probe,config,apply,command,poll}.png`,
`build/b3-acceptance/busy-message.png`, стандартные viewport screenshots и results JSON.

Обнаруженная отдельная нестабильность API fixture: `SetupSession::persist()` публикует
`published_` перед завершением записи draft; `restore()` вызывается под mutex.
Lock-free status может уже показать `cancelled_draft_saved`, пока Resume с `try_lock`
ещё получает `setup_busy`. Воспроизведённый ответ соответствует этому существующему
окну; frontend bundle данный Python API test вообще не загружает. Backend и fixture
не менялись; этот timing issue не скрывается повторным PASS и остаётся known limitation
вне busy UX scope. Production automatic retry не добавлен.

Полные свежие логи: `/tmp/firefly-b4-busy-typecheck.log`,
`/tmp/firefly-b4-busy-frontend.log`, `/tmp/firefly-b4-busy-offline.log`,
`/tmp/firefly-b4-busy-build-final.log`, `/tmp/firefly-b4-busy-ctest.log`,
`/tmp/firefly-b4-busy-api-repeat.log`, `/tmp/firefly-b4-busy-ctest-repeat.log`.

### Изменённые файлы этого polish

Новые:
- `src/admin/frontend/src/stores/foreground.ts`
- `src/admin/frontend/src/components/ForegroundStatus.vue`
- `src/admin/frontend/tests/foreground.test.mjs`

Изменены:
- `src/admin/frontend/src/App.vue`, `styles/base.css`, `stores/manual.ts`, `stores/setup.ts`
- `components/AdminVoice.vue`, `ConnectionWizard.vue`, `ContextualAssist.vue`, `DeviceControls.vue`, `TaskPanel.vue`
- `views/AdvancedView.vue`, `PlaygroundView.vue`, `SetupView.vue`
- `views/ConnectionsView.vue`, `DeviceDetailView.vue`, `DevicesView.vue`, `OverviewView.vue`, `ZonesView.vue` — disable async editor entry controls
- `tests/admin/v2/ManualChromiumTests.js`, `tests/admin/v2/SetupChromiumTests.js`
- `docs/web-admin-ux-v2-b4.md`

### Owner polish check

После запуска пересобранного `build/firefly-demo` открыть `http://localhost:8080/#/setup`.
Действующий demo этой задачей не перезапускается: embedded assets обновятся при
перезапуске процесса, одной перезагрузки вкладки старого процесса недостаточно.
Начать настройку и отправить нормальную просьбу: сразу disabled, затем
«Готовим предложение…», после ответа доступны review/actions. Повторить с отказом
AI и убедиться, что сообщение ошибки остаётся, а busy исчезает. Через manual wizard
проверить explicit probe; через global Review проверить подписи Save/Apply/verify;
через Device Detail — command/read. Оставить Overview открытым на несколько циклов
refresh: глобального loader быть не должно. Навигация, mobile и modal Review должны
оставаться удобными. Functional owner PASS выше не считается отдельной ручной
приёмкой нового polish.

Ветка остаётся `feature/web-admin-ux-v2-b4`; commit/push/merge не выполняются.

Финальный Git status: HEAD `37c0253`, ветка `feature/web-admin-ux-v2-b4`; 27 modified и 6 untracked entries, включая ранее существовавшие AI fix и локальный user-store. Полный вывод сохранён в `/tmp/firefly-b4-busy-git-status.txt`. `git diff --check` после обновления отчёта: PASS. Commit/push/merge не выполнялись.
