# Web Admin UX V2 — B4 default migration

Дата: 2026-10-04. Ветка: `feature/web-admin-ux-v2-b4`. Owner acceptance B4 **pending**; автоматическая проверка не заменяет ручную приёмку. B3 завершён владельцем (manual acceptance и real browser voice/setup smoke PASS), PR #2 merged в `master`: `a1fd5be64261ceb14abfaab4b49f703e135c683e`.

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
| `/api/devices/command`, `/api/devices/poll`, `/api/devices/observe`, `/api/test-command` | guarded actions, explicit readback, Playground и backend evidence/security tests |

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
