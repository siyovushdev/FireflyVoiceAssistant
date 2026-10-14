# M19 engineering review

Current integration: M19 incorporates approved master `818157ec` through merge
`dd1ffa2`, preserving PR #15 history. Production already runs the safety hotfix;
original-request SafetyGate and voice/wake source remain byte-identical to master.
The earlier reports below are historical. Current integration proof and the next
physical gate are recorded in [the integrated report](m19-integration-review.md).
No new deployment development, production Apply, restart or physical test is part
of this integration.

Prior follow-up: [Issue #16 correction and owner acceptance](m19-russian-device-profile.md).
The owner live check of the initial M19 candidate failed on missing `лампу`;
live M19 acceptance remains open. The initial results below are scripted
engineering evidence, not genuine Qwen proposal acceptance. The follow-up
changes the Admin device language prompt/validation and Vue review visibility,
adds the actual owner alias regression and an invalid-profile browser case.

Follow-up final verification: **83/83 serial CTest** (489.89 s), **41/41 frontend
tests**, typecheck and production build/offline asset check passed. All existing
Chromium suites passed. M19 adds the rejected owner profile (zero Apply/HTTP)
and retains desktop/mobile **4 POST / 14 explicit GET**, with execution history
attributing the successful fixture POSTs to `source=voice`. See
[fresh review proof](evidence/m19/alias-review-proof.json) and
[visible rejected-profile warning](evidence/m19/owner-profile-rejected-warning.png).
Self-review checked the minimal product diff, strict grounding/ambiguity tests,
`git diff --check`, test-only worker linkage and absence of production writes.
Status: **READY_FOR_CHIEF_ENGINEER_REVIEW**; genuine live acceptance is pending.

Issue: [#14](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/14).
Base: актуальный на начало работы `origin/master`,
`5fed4230a3cd04d522e5f0ba482609cc7bb9599f` (post PR #13).
Изолированный worktree: `/home/firefly/FireflyVoiceAssistantLinux-m19`;
ветка: `feature/m19-demo-vertical-slice`.

## Короткий gap ledger

| Область | Аудит / решение | Доказательство |
| --- | --- | --- |
| Semantic AI proposal, отдельный Accept, nullable zone, сохранение draft, явный CAS Apply | Уже работает; product code не переписан | `AdminSetupV2Api`, `AdminSetupV2Chromium`, новый M19 |
| Явные HTTP факты, Connection wizard, общий Review, connect-only probe | Уже работает; AI не вызывается для придумывания HTTP | `AdminManualChromium`, `ConnectionAdmin`, `ConnectivityHttp`, новый M19 |
| Guarded voice → один HTTP POST → Predicted; независимый GET → Confirmed | Компоненты работали, единой browser/setup/voice/restart проверки не было | Новый `M19DemoVerticalSlice` соединяет существующие production C++ слои |
| Презентация и воспроизводимость с пустого store | Не хватало короткого private launcher/runbook и точного CLI request log | `m19-private-demo.py`, optional JSONL в существующем mock, `M19PrivateDemoHarness` |
| Настоящие Qwen/NPU/STT, микрофон, физическая лампа, production cutover | Отдельные owner gates; не выполнялись при занятом production runtime | Точная процедура в [runbook](m19-demo.md); не заявлены PASS |

Первоначальный commit `904ced8`: изменения ограничены CMake-регистрацией test targets, test-only B3 model/voice
fixtures, сквозным Chromium-тестом, private launcher, HTTP mock logging и
документацией/evidence. ConfigStore, схема, Vue product code, parser, SafetyGate,
Qwen prompts/settings, production runtime и voice implementation не изменены.
Новый test worker не устанавливается в release; он не связан с RKLLM, sherpa или
ALSA (`ldd`: только стандартные библиотеки).

## Проверки и evidence

Результаты окончательного прогона приведены в PR и committed summary. Все тесты
работают с новыми private stores и localhost mocks. Сборка использует существующие
pinned dependencies; скачивания моделей/зависимостей не было.

- Frontend typecheck; **40/40** tests; production Vite build + offline asset gate
  (4 digest-verified local assets).
- Полная CMake-сборка с assistant/voice/config tools включёнными. Real appliance
  собран, но не запускался; CTest model-backed seams — fake RKLLM/speech.
- Полный offline CTest и все три существующие Chromium suites, а также новый
  desktop/mobile M19. Команды полностью приведены в [runbook](m19-demo.md).
- В каждом M19 сценарии: **2 POST / 7 GET**. POST тела строго `{"on":true}` и
  `{"on":false}`; оба execution source `voice`, route `INDEX_EXACT`, SafetyGate PASS.
  До final Apply и для четырёх unsafe/unsupported фраз / no-wake — **0 HTTP**.
  Setup блокирует voice до STT. Connect-only probe — **0 HTTP-байтов**.
- GET: два обычных readback, одно противоречащее прежнему значению независимое
  наблюдение, три видимых отказа (malformed JSON, wrong type, HTTP 500), один
  readback после restart. Ошибки не заменяют существующее evidence.
- После controlled test restart: тот же persisted configuration/hash, пустая
  execution history, физическое состояние Unknown, **0 HTTP** до explicit GET.
  Повреждённый private current восстанавливается через существующий LKG,
  сигнализирует degraded store, состояние Unknown, **0 HTTP**.
- Новый launcher: empty 0700 store, actual C++ READY, zero HTTP на старте,
  SIGTERM cleanup. CLI mock log: **2 POST / 1 GET**, TCP-only **0 записей**,
  отказывается перезаписывать существующий лог.

Полный журнал и 32 screenshots генерируются в `build/m19-acceptance/`; raw
`results.json` содержит HTTP counts/bodies, исходные fixture transcripts, routes,
latencies, state transitions, ошибки, binary SHA256 и embedded build identity.
Компактный snapshot и выбранные изображения лежат в `docs/evidence/m19/`.
Скрипт/тесты сами не обращаются к production Admin; проверка неизменности
production выполнена отдельно read-only и записана в private build evidence.

Ошибки инженерных итераций сохранены, не выданы за PASS: преждевременный запуск
трёх baseline-тестов до линковки (`ENOENT`); ошибочные ожидания повторной proposal
при same-browser resume и `voice` в IPC `/api/status`; гонка cleanup Chromium
(`ENOTEMPTY`); диагностический GET во время явного Observe (`runtime_busy`);
ожидание текста страницы до создания `document.body` при navigation/restart.
Исправлен harness: используются существующие contracts, ограниченный cleanup,
retry **только** документированного busy diagnostic GET. Команды/Apply/Observe
не повторяются. Итоговый `passed:true` записывается лишь после успешной cleanup.

## Self-review и границы результата

Для первоначального commit проверены diff, отсутствие product/schema/prompt изменений, `git diff --check`,
fixture-only compilation, отсутствие model libraries у test worker, private
0700 stores, exact request counts, сохранение данных при ошибках, zero-I/O
recovery и отсутствие автоматического Apply/polling/retry. Не добавлялись новые
product endpoints, transports, framework, secrets или persistent model caches.

Offline AI — scripted semantic answers; voice — synthetic PCM/KWS/VAD/STT.
Тест исполняет **реальные** wake loop, VoiceProcessor, RuntimeWorker,
CommandEngine/SafetyGate, BoundDeviceExecutor и HTTP socket transport, но не
проверяет акустическую точность, качество Qwen или физическую лампу. Test-only
IPC Admin показывает упрощённый health/status (может оставаться «Запуск системы»);
его READY проверяется API, fixture voice — `/api/dashboard`. Production local
appliance lifecycle/status отдельно покрывают существующие startup/recovery suites.
Новые screenshots не объявлены live-NPU доказательством.

При первоначальной инженерной проверке production PID **44482**, current release **wake-beep-dc7b142e-20261011**, active
revision **1**, hash `36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`
и SHA256 всех четырёх store files совпали до/после. Production service не
останавливался; store, installed releases/models, symlinks, unit и rollback
worktrees не записывались. Merge/deployment не выполнялись.

Нужны независимый Chief Engineer review одного Draft PR и отдельно разрешённые
live model/voice/hardware acceptance, release selection, rollout/rollback и
production reboot. Агент после передачи PR останавливается.
