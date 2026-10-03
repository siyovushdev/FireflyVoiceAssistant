# Web Admin UX V2 B3 — AI-first setup

Статус: **deterministic gate PASS; live NPU gate BLOCKED; B3 не принят владельцем**. B2 принят владельцем; PR #1 переведён в Ready и merged в master как `0c8436b7b5d7e52e5b06b3a3e9f7e9a32e0858df`, с head `ff23b562cb24f96657b6fbbe34d916d6d2bd9c21`. Локальный master обновлён, работа B3 выполняется в `feature/web-admin-ux-v2-b3`. Источник дизайна: [утверждённый design](web-admin-ux-v2-design.md).

## Coordinator и proposal lifecycle

`/v2/#/setup` — первый пункт navigation. Empty Overview ведёт в AI setup. Открытие route читает status, но не вызывает Start, не останавливает рабочий runtime и не получает ownership. Только явное «Начать настройку» вызывает существующий setup lifecycle. Banner настройки виден на всех routes. Старый `/` сохранён.

Одна беседа использует два внутренних пути:

- Semantic: `/api/setup/message`, server draft → неподтверждённая card → «Принять в план». Server draft не считается человеческим подтверждением. Reject восстанавливает предыдущий принятый checkpoint через `/setup/draft`.
- Connectivity: явные факты собираются локально; полный input передаётся `/api/admin/copilot`, затем отдельная card → `/copilot/accept` → `/setup/draft`. IP и JSON никогда не отправляются в sentence splitter.

`stores/setup.ts` хранит текущую session/version, принятый checkpoint, pending proposal и короткие explicit connectivity facts. Текст беседы показывается пользователю, но не отправляется целиком в модель и не хранится как durable chat. `stores/manual.ts` сохраняет TaskBuffer и общий readable review B2. Узкий lifecycle bridge синхронизирует законченные ручные задачи в принадлежащий этому tab server draft; во время setup никогда не делает прямые config Save/Apply.

Proposal cards показывают зоны, устройства, возможности, внешний HTTP contract и shared consumers; exact JSON доступен в Advanced. Ручное редактирование proposal сначала возвращает checkpoint, затем открывает B2 TaskBuffer. Cancel не теряет предыдущий принятый план. Изменённое предложение требует новой проверки.

## Факты и сетевые гарантии

Follow-up сохраняет только явно введённые endpoint, ON/OFF method/path/scalar JSON, Observe GET path/response field либо явное отсутствие чтения. «Как обычно» не заполняет поля. IPv4 без протокола вызывает уточнение; протокол не угадывается. Для физических proposals используется существующее boolean `power`; иные контракты доступны через B2 wizard.

Перед принятием V2 сверяет structured proposal с explicit facts: semantic changes, неизвестные operations, чужой target, выдуманные endpoint/path/body/response mapping/headers отклоняются целиком. Полная C++ validation и hash-bound Copilot acceptance остаются authoritative. Context/output budgets не увеличиваются.

При изменении общего Connection показываются все consumers; пользователь явно выбирает общий edit либо отдельное Connection. Отдельный вариант не разрешает модели менять endpoint общего Connection. TCP probe использует B2 `/api/connections/probe`: connect/close, ноль HTTP bytes, не проверка hardware/API. Нет auto-poll, hardware execution, auto-Apply или blind retries из setup.

## Contextual AI Assist

Zone и Device editors содержат «✨ Заполнить с AI». Используется существующий `/api/admin/draft`; description/aliases/mentions/locations становятся реальными editable TaskBuffer values. Предложенные моделью capabilities/templates не копируются в устройство. ON/OFF не превращаются в TV volume/channel capabilities.

Action language использует `/api/admin/language-profile`; отдельная кнопка schema proposal — `/api/admin/action-draft` с существующими строго проверяемыми state fields. Stable ID и enabled status существующего действия сохраняются. Для ещё неполного нового action запрос контекста исключает только этот локальный action, не вставляя его в server draft.

Маркер «Предложено AI — проверьте перед добавлением», принятие AI-полей и отказ с возвратом checkpoint отделены от «Добавить в план» и финального Apply. Без активной принадлежащей пользователю сессии предлагается явное начало setup. Ручные поля работают без AI.

## Узкие Admin изменения относительно design

| Изменение | Причина / контракт |
| --- | --- |
| Optional `atomic_message: true` в `/api/setup/message` | Короткая V2 просьба вместе с финальными ограничениями («Оба только…») должна проверяться одной единицей. Лимит 1800 UTF-8 bytes и максимум три операции сохраняются. Legacy clients без поля используют прежний sentence splitter. Это не разрешает HTTP в semantic lane. |
| `create_device.location: null`; dialogue `power_device` с null; `device` с nullable location | Explicit «без зоны» создаёт nullable `room_id`, без fake Location. Без запрошенных возможностей — существующий `custom` с пустыми actions; power-only остаётся ровно ON/OFF и допускается только при явном запросе включения/выключения в Admin input. Одного названия оборудования недостаточно. RuntimeWorker допускает null при явном «без зоны» в Admin input. |
| `system_name` → `rename_system` | Только display name существующей Системы; stable ID и остальные поля сохраняются. Новый domain entity не вводится. |
| Admin-only universal prompt / Location metadata type | «Система и оборудование», необязательные зоны вместо обязательного дома/комнаты. Production command prompt, LanguageIndex, SafetyGate, модель и бюджеты не меняются. |
| Manual `/setup/draft` checkpoint cleanup | После явного возврата checkpoint удаляет focus на отсутствующие entities и закрывает прежнее unresolved clarification. `message_incomplete` по-прежнему блокирует ручной draft patch и Apply. |

Остальные API payloads сохраняют существующие контракты. Никакого нового chat backend/agent framework.

## Ownership, review и восстановление

Owner token хранится только в tab sessionStorage; не в URL, transcript или prompt. Reload авторизует существующий token heartbeat-запросом и повторно показывает неподтверждённый proposal. Чужая session не захватывается; интерфейс read-only. Истёкшая/inactive session возобновляется только явной кнопкой Resume/Start.

Любое изменение server version инвалидирует review, даже если значения draft восстановлены. Version conflict сохраняет локальный ввод, инвалидирует review, предлагает перечитать серверный draft для повторной проверки. Принятый checkpoint и неподтверждённый результат различаются. При незавершённом message допускается только явный повтор полного исходного текста или cancel; flag не исправляется клиентом.

Общий review содержит все semantic/connectivity/manual изменения и shared effects. В setup `/config/validate` проверяет полный candidate, но production preflight отложен до `/setup/apply`, поскольку Admin model занимает runtime. Только принадлежащая пользователю session с проверенным draft может использовать этот путь; вне setup B2 по-прежнему требует `apply_ready`. Final confirmation проверяет stored/active identity и session version. Сам backend восстанавливает production model, выполняет preflight, CAS Save/Apply/compensation. После Apply начинается новая session для следующих изменений; UI отмечает «Эта часть уже применена».

## Голос

Существующий local WAV/STT: permission → recording с timer → transcribing → editable transcript → отдельное Send. `X-Firefly-Setup` передаёт существующую setup ownership. Запись/распознавание блокирует Apply. Отмена и navigation закрывают tracks, late responses не вставляются в новую задачу; PCM очищается после обработки. Raw audio не сохраняется. Secure-context baseline — localhost / SSH tunnel. Plain LAN HTTP получает объяснение и текстовый fallback.

## Проверки

Frontend: typecheck PASS, **29/29** unit/state/adapter tests PASS, production build и offline artifact gate PASS (4 declared local assets, hashed JS/CSS). Fake model доказывает contracts/UI, но не качество реального Qwen.

```sh
npm --prefix src/admin/frontend run typecheck
npm --prefix src/admin/frontend run test
npm --prefix src/admin/frontend run build
cmake --build build -j4
ctest --test-dir build -R '^(AdminSetupV2Chromium|AdminSetupV2Api|AdminCopilot|DialogueSetup|AdminManualChromium|AdminManualApi|AdminGuardedCommand)$' --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

Browser uses production Vite dist → C++ HttpServer → private temporary ConfigStore and HTTP mock; happy paths use visible controls. Failed-version fixtures use explicit external API writes solely to simulate another client. No Vue candidate assignment. Asset/network gate blocks external runtime requests.

Browser suite покрывает A (зона и два ON/OFF устройства, reload/reject), B (явный non-home context и no-zone), C (полный HTTP proposal → TCP probe → plan → review → setup Apply → один explicit GET/poll), D (missing facts/«как обычно»), E (malformed output, version conflict, unrelated invalid Connection), F (stable rename и shared consumers). Negative Observe/mixed/unknown/network-fact cases дополнительно проверяются unit/API tests.

Contextual Zone/Device/Action проверяются через видимые controls: real editable values, checkpoint Cancel, отдельный review/Apply. Voice component проходит browser fixture с fake media/STT: timer, запрет Apply, editable transcript без auto-send, закрытие tracks, late response после navigation игнорируется. Это **не** real microphone acceptance.

До explicit poll AI/proposal/review/Apply дают **0 HTTP hardware requests**; TCP probe — **0 HTTP bytes**. Затем только explicit poll даёт ровно один GET. Operational ON/OFF hardware counts сохраняет B2 Chromium suite. Layout gate: 390×844, 800×600, 1280×800; external runtime requests = 0.

Финальный deterministic gate 2026-10-03:

| Проверка | Результат |
| --- | --- |
| Frontend typecheck | PASS |
| Frontend unit/state/adapter tests | 29/29 PASS |
| Vite production build / offline assets | PASS, 4 local declared assets |
| CMake build `-j4` | PASS |
| Focused B2/B3 API/Chromium + Setup/Copilot | 7/7 PASS, 70.12 s |
| Full CTest | 72/72 PASS, 257.60 s |
| B3 Chromium | PASS, реально выполнен через C++ HttpServer |
| B2 Manual Chromium / guarded commands | PASS, существующие request-count assertions сохранены |
| `git diff --check` | PASS |

Logs: `build/b3-{typecheck,frontend-test,frontend-build,build,focused,ctest}.log`. Реальный Chromium запускается отдельно от live NPU. Screenshots и network summary: `build/b3-acceptance/` (не Git).

### Live NPU — отдельно

Harness `benchmarks/web-admin-b3/Acceptance.cpp` использует штатный `RuntimeModel` и отдельный `/tmp/firefly-b3-npu-*` store. Real request/trace/latency/context metrics сохраняются только в private build artifacts. Никаких production-store requests и hardware methods. Runtime model/context/output settings не меняются. Реальный прогон 2026-10-03 выполнен на RK3588, RKLLM 1.3.0, штатной Admin Qwen3-4B W8A8 CTX4096. Никакого fake worker. Перед ним тестовый demo с private store `/tmp/firefly-b2-manual` штатно остановлен; после прогона восстановлен с прежними аргументами. Production user-store не использовался.

```sh
python3 benchmarks/web-admin-b3/Monitor.py --prefix build/b3-live/run-20261003-1624
```

| Сценарий | Реальный результат | Задержка |
| --- | --- | --- |
| A: Детская, Свет и Телевизор, только ON/OFF | semantic_success: 1 Zone, 2 Devices, ровно 2 actions на каждом, без connectivity | 112.259 s весь setup message; dialogue 19.242 s вместе с загрузкой контекста; inference 12.049 s |
| B: Цех упаковки → Вентилятор ON/OFF | semantic_success | 51.323 s весь message; dialogue 16.629 s; inference 8.082 s |
| C: explicit HTTP endpoint + ON/OFF + Observe | **ai_unavailable / RKLLM deadline exceeded; abort status=0**. Проверяемый proposal не получен | 72.312 s Copilot целиком; context bootstrap 11.989 s, затем штатный deadline 60 s |
| D/E/F, no-zone, отдельные Цех/Склад/Лабораторный стенд | **не выполнены live после остановки C**; покрыты deterministic/browser, это не NPU evidence | — |

A: context 511 tokens, input 44, output 34/384, system prompt 2629 bytes. B: context 533, input 39, output 22/384, prompt 2707 bytes. C: system prompt 3596 bytes / bootstrap 838 tokens, explicit input 736 bytes, output budget 384. После deadline runtime сбросил engine: metrics=null, raw output пуст. **Нет доказательства, что достигнут output budget или inventory limit**; установлен именно deadline failure. Generic поле harness classification для C записано как `validation_rejection`, но authoritative trace содержит `ai_unavailable`, а не ошибку config validation.

Всего run 320.79 s, exit 1; minimum MemAvailable 2,052,032 KiB, memory guard не сработал. Private store остался revision 1, 0 zones / 0 devices; final Apply не вызывался, hardware requests 0. Automatic retry отсутствовал (`retry_count: 0`).

Артефакты (локальные, не Git): `build/b3-live/run-20261003-1624.json`, `.log`, `-memory.jsonl`. В JSON есть полный explicit input и trace; private store `/tmp/firefly-b3-npu-Ot3URv`.

**B3 live acceptance НЕ PASS.** Согласно owner constraint работа над inference/budget остановлена. Deadline, context, inventory и output budget не увеличены. Нужно отдельное решение владельца о дальнейшей диагностике/изменении Admin contract или профильных лимитов. До него оставшиеся live A–F и real voice не объявляются завершёнными. Draft PR фиксирует проверенную реализацию и этот blocker, а не готовность к merge.

### Real voice — отдельно

**Owner microphone smoke pending; после text live blocker не запускался.** Fake recognition и browser controls не являются доказательством реального microphone/STT. Требуется произнести «Добавь вентилятор в Цех», проверить transcript, явно Send, увидеть proposal без Apply.

## Ограничения и B4

- Автоматический разбор connectivity facts ограничен power-only HTTP; typed/non-power, nested/unsupported contracts остаются ручными B2 workflows. Observe без nested JSONPath/coercion.
- Full catalog остаётся в существующем Copilot context; inventory около 900 bytes/output 384 не увеличены. При измеренном блокере требуется отдельное решение владельца.
- Setup conversation не durable chat; checkpoints/ownership обеспечивают безопасный reload, история беседы не production state.
- B4 не начат: vanilla `/` остаётся fallback, `/v2/` не становится default. Cloud, auth, HTTPS, новые executors, STT/VAD/wake и production inference не изменяются.
- Owner acceptance B3 отдельная; публикация PR не означает принятия.

URL: **http://localhost:8080/v2/#/setup** (после запуска binary с актуальным dist).

## Changed files

- `benchmarks/web-admin-b3/Acceptance.cpp`
- `benchmarks/web-admin-b3/Monitor.py`
- `docs/web-admin-ux-v2-b3.md`
- `src/admin/AdminCopilot.cpp`
- `src/admin/CMakeLists.txt`
- `src/admin/DialogueOperations.h`
- `src/admin/RuntimeWorker.cpp`
- `src/admin/SetupSession.h`
- `src/admin/frontend/src/App.vue`
- `src/admin/frontend/src/adapters/editing.ts`
- `src/admin/frontend/src/adapters/manualValidation.ts`
- `src/admin/frontend/src/adapters/setup.ts`
- `src/admin/frontend/src/api/admin.ts`
- `src/admin/frontend/src/api/setup.ts`
- `src/admin/frontend/src/components/AdminVoice.vue`
- `src/admin/frontend/src/components/ContextualAssist.vue`
- `src/admin/frontend/src/components/EntityEditor.vue`
- `src/admin/frontend/src/composables/useRefresh.ts`
- `src/admin/frontend/src/router/index.ts`
- `src/admin/frontend/src/stores/manual.ts`
- `src/admin/frontend/src/stores/setup.ts`
- `src/admin/frontend/src/styles/base.css`
- `src/admin/frontend/src/views/DeviceDetailView.vue`
- `src/admin/frontend/src/views/OverviewView.vue`
- `src/admin/frontend/src/views/SetupView.vue`
- `src/admin/frontend/src/views/ZonesView.vue`
- `src/admin/frontend/tests/setup.test.mjs`
- `tests/admin/SetupTests.cpp`
- `tests/admin/v2/ChromiumTests.js`
- `tests/admin/v2/SetupApiTests.py`
- `tests/admin/v2/SetupChromiumTests.js`
- `tests/admin/v2/SetupWorker.cpp`
- `tests/admin12/AdminCopilotTests.cpp`

`user-store/`, build/dist/node_modules/.test-build и private artifacts не входят в B3 commit.
