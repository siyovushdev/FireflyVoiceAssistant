# Web Admin UX V2 B3 — AI-first setup

Статус: **deterministic gate PASS; physical C PASS без NPU; semantic no-zone retest PASS; Language Profile quality + live index gate PASS; owner manual acceptance PASS; real browser voice/setup smoke PASS; B3 принят владельцем**. B2 принят владельцем; PR #1 переведён в Ready и merged в master как `0c8436b7b5d7e52e5b06b3a3e9f7e9a32e0858df`, с head `ff23b562cb24f96657b6fbbe34d916d6d2bd9c21`. Локальный master обновлён, работа B3 выполняется в `feature/web-admin-ux-v2-b3`. Источник дизайна: [утверждённый design](web-admin-ux-v2-design.md).

## Coordinator и proposal lifecycle

`/v2/#/setup` — первый пункт navigation. Empty Overview ведёт в AI setup. Открытие route читает status, но не вызывает Start, не останавливает рабочий runtime и не получает ownership. Только явное «Начать настройку» вызывает существующий setup lifecycle. Banner настройки виден на всех routes. Старый `/` сохранён.

Одна беседа использует два внутренних пути:

- Semantic: `/api/setup/message`, server draft → неподтверждённая card → «Принять в план». Server draft не считается человеческим подтверждением. Reject восстанавливает предыдущий принятый checkpoint через `/setup/draft`.
- Connectivity (owner decision 2026-10-04): после сбора всех explicit facts кандидат строится локально через общие B2 `newHttpConnection`, `defaultHttpMapping`, `projectConnectivity`. Затем `/config/validate` → отдельная card → человеческий Accept → повторная validation и `/setup/draft` по текущей версии. `/copilot` и `/copilot/accept` не вызываются. IP и JSON не отправляются в sentence splitter. Legacy Admin Copilot API сохранён.

`stores/setup.ts` хранит текущую session/version, принятый checkpoint, pending proposal и короткие explicit connectivity facts. Текст беседы показывается пользователю, но не отправляется целиком в модель и не хранится как durable chat. `stores/manual.ts` сохраняет TaskBuffer и общий readable review B2. Узкий lifecycle bridge синхронизирует законченные ручные задачи в принадлежащий этому tab server draft; во время setup никогда не делает прямые config Save/Apply.

Proposal cards показывают зоны, устройства, возможности, внешний HTTP contract и shared consumers; physical card помечена «Подключение из явных фактов», exact JSON доступен в Advanced. Ручное редактирование proposal сначала возвращает checkpoint, затем открывает B2 TaskBuffer. Cancel не теряет предыдущий принятый план. Изменённое предложение требует новой проверки.

## Факты и сетевые гарантии

Follow-up сохраняет только явно введённые endpoint, ON/OFF method/path/scalar JSON, Observe GET path/response field либо явное отсутствие чтения. «Как обычно» не заполняет поля. IPv4 без протокола вызывает уточнение; протокол не угадывается. Для физических proposals нужны ровно два однозначных безаргументных ON/OFF actions и одно существующее boolean `power`; иные контракты доступны через B2 wizard. Пустой body для POST/PUT/PATCH задаётся явно как `{}`. Endpoint с неявным path/protocol, nested body/field и противоречивые HTTP facts не исправляются. Shared/separate choice всегда явный. Если существующих Connection несколько либо задан base path, редактирование общего подключения передаётся ручному B2 wizard.

Перед показом и принятием V2 использует полный текущий accepted configuration. `verifyConnectivity` повторно строит кандидат из pinned facts/target/shared choice и отклоняет любое расхождение, включая чужие mappings или semantic changes. Setup heartbeat/version, сопоставление accepted/server draft и CAS `/setup/draft` не дают принять устаревшую основу. Полные C++ ConfigLoader/ConfigValidator остаются authoritative. Context/output budgets не увеличиваются; physical lane не генерирует AI JSON.

При изменении общего Connection показываются все consumers; пользователь явно выбирает общий edit либо отдельное Connection. Отдельный вариант не разрешает модели менять endpoint общего Connection. TCP probe использует B2 `/api/connections/probe`: connect/close, ноль HTTP bytes, не проверка hardware/API. Нет auto-poll, hardware execution, auto-Apply или blind retries из setup.

## Contextual AI Assist

Zone и Device editors содержат «✨ Заполнить с AI». Используется существующий `/api/admin/draft`; description/aliases/mentions/locations становятся реальными editable TaskBuffer values. Предложенные моделью capabilities/templates не копируются в устройство. ON/OFF не превращаются в TV volume/channel capabilities.

Action language использует `/api/admin/language-profile`; отдельная кнопка schema proposal — `/api/admin/action-draft` с существующими строго проверяемыми state fields. Stable ID и enabled status существующего действия сохраняются. Для ещё неполного нового action запрос контекста исключает только этот локальный action, не вставляя его в server draft.

Маркер «Предложено AI — проверьте перед добавлением», принятие AI-полей и отказ с возвратом checkpoint отделены от «Добавить в план» и финального Apply. Без активной принадлежащей пользователю сессии предлагается явное начало setup. Ручные поля работают без AI.

## Узкие Admin изменения относительно design

| Изменение | Причина / контракт |
| --- | --- |
| Optional `atomic_message: true` в `/api/setup/message` | Короткая V2 просьба вместе с финальными ограничениями («Оба только…») должна проверяться одной единицей. Лимит 1800 UTF-8 bytes и максимум три операции сохраняются. Legacy clients без поля используют прежний sentence splitter. Это не разрешает HTTP в semantic lane. |
| `create_device.location: null`; dialogue `power_device` с null; `device` с nullable location | Explicit «без зоны» создаёт nullable `room_id`, без fake Location. Без запрошенных возможностей — существующий `custom` с пустыми actions; незапрошенный `power_only` детерминированно сужается до `custom` перед authoritative decode без повторного inference; power-only остаётся ровно ON/OFF и допускается только при явном запросе включения/выключения в Admin input. Одного названия оборудования недостаточно. RuntimeWorker допускает null при явном «без зоны» в Admin input. |
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

## Проверки baseline 2026-10-03

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

**Исторический результат раннего B3 live прогона: НЕ PASS (blocker закрыт ниже).** Согласно owner constraint работа над inference/budget остановлена. Deadline, context, inventory и output budget не увеличены. На момент прогона требовалось отдельное решение владельца о дальнейшей диагностике/изменении Admin contract или профильных лимитов. На момент этого прогона оставшиеся live A–F и real voice не объявлялись завершёнными; последующее owner decision и диагностика приведены ниже. Draft PR фиксирует проверенную реализацию и этот blocker, а не готовность к merge.

### Owner decision и отдельная staged-диагностика — 2026-10-04

[Owner review PR #2](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/2) разрешает staged coordinator только после трёх валидных отдельных proposals внутри существующего deadline. Admin Qwen3-4B W8A8, CTX4096, request output budget 384, deadline 60 s и RKLLM profile не изменены. Повторов inference нет.

Диагностика вызывает текущие `copilotPrompt`, `RuntimeModel::useAdminContext(prompt, 384)`, `inferAdmin` и `decodeCopilotProposal` напрямую, без frontend/HTTP transport и без встроенного format retry. Это проверка первого ответа реальной модели, **не staged browser/API acceptance**. System-prompt tokens ниже — измеренный RKLLM bootstrap prefill с chat template, не оценка tokenizer.

Все запросы используют один private store `/tmp/firefly-b3-stages-wBJYiq` с сохранённой semantic configuration успешного live B (Вентилятор и существующие ON/OFF actions). C1 отклонён и не принят. Для независимых C2/C3 Connection задан отдельной ручной prerequisite fixture из explicit endpoint `http://127.0.0.1:8765`; raw C1 не исправлялся. Это не accepted chain. Fixture не сохраняется в store, Save/Apply не вызывается. Demo с `/tmp/firefly-b2-manual` штатно останавливается на время эксклюзивного NPU прогона и восстанавливается с прежними аргументами.

| Запрос | System prompt bytes / tokens | Input bytes / tokens | Output tokens | Context bootstrap | Inference wall | Полный вызов | Validation |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C1 — Connection only | 3596 / 838 | 255 / 71 | 124 / 384 | 11.744 s | 45.134 s | 56.885 s | FAIL: `Invalid copilot string`, пустой `summary` |
| C2 — только ON/OFF | 3768 / 896 | 415 / 112 | 151 / 384 | 13.705 s | 52.318 s | 66.028 s | FAIL: JSON parse error; неверный `set_http_binding` вместо `set_connection_binding` |
| C3 — Observe only | 3768 / 896 | 365 / 77 | 136 / 384 | 12.812 s | 44.420 s | 57.309 s | PASS: ровно одна `set_http_observe`, GET `/relay/1`, `on → power` |

C1 raw содержит ровно одну `set_http_connection` с заданным endpoint, но envelope `"summary":""` не проходит текущий strict decoder. C2 raw содержит две попытки legacy `set_http_binding` с повреждённым object syntax (`"device","Цех упаковки",...`); полноценный JSON proposal не получен. Ни один ответ не исправлен и не повторён. C2 wall >60 s включает bootstrap; сам inference завершился за 52.318 s без deadline error. Vendor prefill/generation: C1 1.534/43.552 s; C2 2.462/49.817 s. C3 prefill/generation 1.936/42.450 s. Загрузка/проверка модели отдельно: C1 87.066 s, C2 78.971 s, C3 88.762 s. Все три inference завершились без timeout; 60 s deadline не увеличивался.

Артефакты содержат точные inputs, system prompts, raw, metrics и validation errors; остаются локальными, не Git:

- `build/b3-live/stages-20261004-063617.{json,log}` и `-memory.jsonl` — C1.
- `build/b3-live/stages-rest-20261004-064206.{json,log}` и `-memory.jsonl` — C2.
- `build/b3-live/stages-observe-20261004-064536.{json,log}` и `-memory.jsonl` — C3; полный валидный raw и decoder result сохранены.
- `build/b3-live/StageDiagnostic-C1.cpp`, `StageDiagnostic.cpp`, `diagnostic-base.json` — private diagnostic sources/fixture, слинкованные с текущими build libraries; production sources не изменены.
- `stages-rest-20261004-064058.json` — отказ harness до загрузки модели/AI: попытка initialize существующего private store. Исправлено только открытие store через `current()`; это не повтор AI-запроса.

Во всех трёх прогонах `stored_unchanged=true`, hardware requests 0, inference retries 0. Production user-store не использовался и его существующие локальные изменения не затронуты. Минимальный MemAvailable: C1 2,054,528 KiB, C2 2,041,052 KiB, C3 2,068,268 KiB; memory admission не блокировал запросы. C3 является отдельным валидным diagnostic proposal, но не исправляет отказы C1/C2.

**Исторический staged-Qwen implementation gate не пройден:** необходимы валидные proposals всех трёх stages, а не только отсутствие timeout. В этом диагностическом commit coordinator, UX, inference limits, prompt и profile не изменялись. Staged C и последующие D/E/F/no-zone/Цех/Склад/Лабораторный стенд не запускаются. В этом историческом прогоне real browser microphone smoke ещё не выполнялся; итоговая owner acceptance с voice/setup — PASS, см. ниже.

Для этого documentation-only изменения frontend tests, полный CTest и Chromium повторно не запускались: результаты 29/29, 72/72 и Chromium PASS выше относятся к baseline 2026-10-03, а не к новой staged реализации. Проверяется `git diff --check`.

### Real voice — отдельно

**Исторический baseline 2026-10-03: microphone smoke после прежнего text blocker не запускался.** Последующая диагностическая попытка и итоговый owner PASS описаны ниже. Fake recognition и browser controls не являются доказательством реального microphone/STT. Требуется произнести «Добавь вентилятор в Цех», проверить transcript, явно Send, увидеть proposal без Apply.

## Ограничения и B4

- Автоматический разбор connectivity facts ограничен power-only HTTP; typed/non-power, nested/unsupported contracts остаются ручными B2 workflows. Observe без nested JSONPath/coercion.
- Для legacy Copilot full catalog остаётся в существующем context; inventory около 900 bytes/output 384 не увеличены. При измеренном блокере требуется отдельное решение владельца.
- Setup conversation не durable chat; checkpoints/ownership обеспечивают безопасный reload, история беседы не production state.
- B4 не начат: vanilla `/` остаётся fallback, `/v2/` не становится default. Cloud, auth, HTTPS, новые executors, STT/VAD/wake и production inference не изменяются.
- Owner acceptance B3 завершена PASS; разрешён merge и последующий B4.

URL: **http://localhost:8080/v2/#/setup** (после запуска binary с актуальным dist).

## Deterministic physical lane — owner decision 2026-10-04

[Последний owner comment](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/2#issuecomment-5977519061) заменяет staged-Qwen направление: поддерживаемый power-only HTTP proposal собирается из уже известных фактов без NPU. Semantic setup и Contextual AI Assist остаются на Qwen. C больше не NPU-сценарий; прежние C/C1/C2/C3 таблицы выше сохраняются только как история решения.

Общая B2 construction path сохраняет полный candidate, stable IDs и нетронутых consumers. Separate создаёт новый ID штатным allocator с учётом retired IDs; shared меняет выбранное единственное существующее Connection и показывает его consumers, включая Observe-only. Explicit no-observe удаляет только чтение выбранного устройства. Неизвестные headers, вложенные body, дополнительные actions и legacy/non-power контракты не синтезируются.

Ready-to-review proposal ещё не меняет setup draft. Accept повторяет backend validation и проверку версии, затем записывает только draft. Final review содержит весь combined connectivity и требует отдельного Apply. Повреждённый/устаревший proposal или отказ backend сохраняет предыдущий accepted plan. Reload сохраняет неподтверждённый deterministic proposal; старые AI connectivity cards считаются устаревшими. Ручной B2 editor получает исходный proposal до создания формы, поэтому endpoint/mappings не теряются; Cancel сохраняет accepted checkpoint.

Регрессии проверяют отсутствие запросов `/api/admin/copilot` и `/accept` на всём B3 connectivity пути; точные typed bodies; no-observe; shared/separate и Observe-only consumers; строгую полную validation; stale version/facts; сохранение плана после отказа; reload; B2 edit/cancel; общий review и explicit Apply; TCP probe с нулём HTTP bytes. Semantic malformed/mixed и старый Copilot остаются покрыты существующими suites.

Проверки финального production/frontend кода 2026-10-04:

| Проверка | Результат |
| --- | --- |
| Frontend typecheck | PASS |
| Frontend unit/state/adapter | 32/32 PASS |
| Frontend production build / offline gate | PASS, 4 local assets |
| CMake build | PASS |
| Focused B2+B3 Chromium/API + Setup/Copilot/GuardedCommand | 7/7 PASS, 70.99 s |
| Full CTest | 72/72 PASS, 256.74 s |
| C Chromium/API | PASS: zero Copilot/accept calls, proposal → explicit Accept → combined review → final confirmation → Apply |
| Network assertions | До explicit operational action: hardware requests 0; TCP probe: 0 HTTP bytes; затем ровно один explicit GET |
| B2 manual edit/cancel, regressions | PASS |
| `git diff --check` | PASS |

Logs: `build/b3-explicit-{typecheck,frontend-test,frontend-build,build,focused,ctest}.log`; screenshots/network report: `build/b3-acceptance/`. Первоначальный focused запуск пересёкся с линковкой и один B2 API fixture не дождался runtime; suite повторён после сборки и прошёл. Дополнительная browser regression выявила, что старый edit-proposal путь открывал wizard до замены buffer; передача initial candidate в `manual.begin` исправляет потерю endpoint и покрыта тестом.

### Semantic и Contextual live NPU после deterministic gate — до no-zone narrowing

```sh
python3 benchmarks/web-admin-b3/Monitor.py --prefix build/b3-live/explicit-semantic-20261004-0735
python3 benchmarks/web-admin-b3/Monitor.py --prefix build/b3-live/explicit-remaining-20261004-0741 \
  --remaining build/b3-live/explicit-semantic-20261004-0735.json
```

`--remaining` — явное продолжение только ранее не выполненных non-home/contextual сценариев с validated fixture успешного B из указанного отчёта. Оно не повторяет no-zone, A/B/F и не меняет модель или profile. Используются новый private store и штатный новый engine после отмены незавершённой тестовой сессии. Production user-store не используется.

| Сценарий | Результат | Полный message/API wall | Основные inference calls |
| --- | --- | --- | --- |
| A: Детская, Свет и Телевизор только ON/OFF | PASS | 112.050 s | dialogue 11.529 s |
| B: Вентилятор в Цех упаковки, ON/OFF | PASS | 50.435 s | dialogue 7.708 s |
| F: Вентилятор → Вытяжка, stable ID/actions | PASS | 4.926 s | dialogue 2.681 s |
| no-zone: «Добавь контроллер стенда без зоны.» | **FAIL, безопасный validation rejection** | 28.803 s | 6.115 s + 6.368 s |
| Цех | PASS | 35.265 s | dialogue 3.614 s |
| Склад | PASS | 34.718 s | dialogue 4.078 s |
| Лабораторный стенд | PASS после существующего format-retry | 63.953 s | 4.985 s + 5.540 s |
| Contextual Zone draft | Validated suggestion | 31.499 s | 7.711 s + 15.363 s |
| Contextual Device draft | Validated suggestion | 42.181 s | 8.683 s + 2.856 s + 18.943 s |
| Contextual Action language | Validated suggestion | 8.938 s | 4.619 s |
| Contextual Action schema | Validated suggestion | 25.075 s | 20.863 s |
| Physical C | **Не NPU scenario** | Chromium/API gate выше | 0 AI calls |

Message wall включает context admission и штатное дополнение языковых профилей. JSON artifacts содержат latency/trace/metrics/raw для **каждого** AI call, включая все профили; в таблице dialogue показан отдельно. Несколько inference calls в contextual draft — существующие группы генерации, не новый staged connectivity coordinator.

No-zone дважды выдал `{"ops":[["power_device",null,"контроллер стенда"]]}`. Guard отклонил незапрошенные ON/OFF (`Power capabilities were not explicitly requested`); draft остался неизменным, никаких wrong capabilities не принято. В Лабораторном стенде первая попытка содержала неверный JSON object syntax; существующий semantic format-retry дал валидный `location`. Эти два retry уже предусмотрены прежним semantic backend, не добавлены этим изменением. В physical lane retries отсутствуют. Prompt, inference limits и retry policy semantic/legacy API не менялись; failed no-zone scenario целиком не перезапускался.

Первый run: exit 1 после no-zone, 337.93 s; второй: exit 0, 366.87 s. Minimum MemAvailable 2,057,788 / 2,056,708 KiB. Оба private stored configuration остались равны исходному empty config, hardware requests 0, final Apply не выполнялся. На этом checkpoint общий live gate был **не PASS** из-за no-zone; модель не настраивалась для исправления этого результата. Результат последующего owner-authorized narrowing и отдельного retest приведён ниже.

### No-zone semantic narrowing — owner follow-up 2026-10-04

По [owner decision](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/2#issuecomment-5977968137) RuntimeWorker сужает только `create_device` с `template=power_only`, когда исходный Admin input не содержит явно запрошенного ON/OFF по прежней проверке. Меняется только template на существующий `custom`; имя, description и nullable location сохраняются. Затем выполняются прежние AdminCopilot decode и ConfigLoader/ConfigValidator. Устройство не получает actions или state fields. Обратного преобразования custom → power нет. Повторный inference для сужения не нужен, raw Qwen output остаётся в trace.

Правило общее для zoned/no-zone устройств и любых имён. Regression matrix использует «Вентилятор», «Контроллер стенда», «Измерительный блок» в обеих scopes; сравнивает результат с обычным custom construction, проверяет ровно один inference и явный ON/OFF. Protected entrance door, неизвестная зона, неявный no-zone, лишние arguments, смешанные/unsupported operations и незапрошенная brightness остаются отклонёнными. Browser/API fixture намеренно выдаёт `power_device` для запроса без возможностей. До Accept card показывает «Без зоны. Возможности: ещё не заданы»; итоговый draft имеет null room и пустые actions/state, stored config не меняется.

Для одного изолированного live retest добавлен `Monitor.py --no-zone`: один semantic message через настоящий SetupSession, прежние Qwen/CTX4096/budget 384/deadline 60 s, private temp store, cancel без final Apply. Harness проверяет нулевые capabilities, один dialogue call/trace и retry_count=0. Нормальное дополнение language profile учитывается отдельно; narrowing не вызывает его повторно. Voice/STT/VAD и inference configuration не изменены.

Повторная проверка после fix:

| Проверка | Результат |
| --- | --- |
| Frontend typecheck / unit/state/adapter | PASS / 32/32 PASS |
| Production frontend build / offline assets / CMake build | PASS |
| Focused B2+B3 Chromium/API + Setup/Copilot/GuardedCommand | 7/7 PASS, 72.66 s |
| Full CTest | 72/72 PASS, 258.03 s |
| Chromium | B2/B3 PASS; no-capability card до Accept; 0 hardware до explicit operational action; TCP probe 0 HTTP bytes |
| `git diff --check` | PASS |

Логи повторной проверки: `build/b3-no-zone-{build,focused,ctest}.log`, `build/b3-no-zone-frontend-{typecheck,test,build}.log`.

Один real NPU retest выполнен командой:

```sh
python3 benchmarks/web-admin-b3/Monitor.py --no-zone \
  --prefix build/b3-live/no-zone-narrowing-20261004
```

**PASS**: «Добавь контроллер стенда без зоны.» → raw `{"ops":[["power_device",null,"контроллер стенда"]]}` → validated `custom`, `room_id=null`, `action_ids=[]`, `state=[]`, общий actions list пуст. Ровно один dialogue inference, `retry_count=0`; новый inference для narrowing не выполняется. Полный message wall **26.050 s**, включая штатный language profile. Профиль не меняет capabilities и также прошёл без retry.

| AI call | System prompt bytes / context tokens | Input bytes / tokens | Output tokens / budget | Inference wall | API wall |
| --- | --- | --- | --- | --- | --- |
| Semantic dialogue | 2629 / 511 | 63 / 23 | 18 / 384 | 5.358 s | 13.772 s |
| Existing device language profile | 1437 / 291 | 49 / 24 | 33 / 256 | 7.884 s | 12.227 s |

Context tokens взяты из штатных RKLLM metrics; prompt bytes — длина UTF-8 system instruction из trace, без chat template. Dialogue prefill/generation: 0.645 / 4.697 s; profile: 0.487 / 7.373 s. Model load/bootstrap и enter/restore lifecycle учитываются отдельно от request deadline. Raw, decoded/validation results, per-call metrics и memory trace сохранены в `build/b3-live/no-zone-narrowing-20261004.{json,log}`, `-memory.jsonl`, `-run.log`.

Run exit 0, полный lifecycle 149.73 s, minimum MemAvailable 1,942,880 KiB. Private store `/tmp/firefly-b3-npu-WoxUqz`: `stored_unchanged=true`, hardware requests **0**, final Apply отсутствует. После cancel тестовый engine завершён, private demo восстановлен с исходными аргументами. A/B/F, non-home и contextual cases в этом fix не перезапускались; их предыдущие результаты приведены выше. No-zone semantic blocker закрыт этим retest; реальный voice/setup flow впоследствии принят владельцем, см. итоговую acceptance ниже.

### Real browser microphone smoke — 2026-10-04

В отдельном native `firefly-demo` с private store, штатным AdminSetup Zipformer и настоящим Chromium (без fake media/device/STT) выполнена одна запись на 6 секунд. Browser enumerated `reSpeaker XVF3800 4-Mic Array Analog Stereo`, системный default source — этот же физический вход. Запись сделана после отдельной отметки готовности. Raw WAV на диск не записывался.

Local STT вернул «ха»; ожидаемая фраза «Добавь вентилятор в Цех» не получена. Результат появился в editable transcript. **Transcript не исправлялся и не отправлялся**: `/api/admin/transcribe` — 1 запрос, `/api/setup/message` — 0, Apply — 0, executions — 0; stored hash до/после одинаковый. Это подтверждает реальный capture → native STT → editable transcript и отсутствие auto Send, но **не** полный speech → semantic proposal PASS. Это исторический диагностический результат; последующий owner real browser voice/setup smoke завершён PASS, см. итоговую acceptance ниже.

Артефакты: `build/b3-live/explicit-real-voice-20261004.{json,log,png}`, `-run.log`; diagnostic script `build/b3-live/RealVoiceSmoke.cjs` локальный, не Git. После завершения private demo `/tmp/firefly-b2-manual` восстановлен с исходными аргументами, включая прежний voice/wake режим; эти настройки не менялись. Это исторический прогон до итоговой owner acceptance; итоговый статус B3 — PASS.

## Startup/reload ownership — owner acceptance fix

V2 Start/Resume передаёт `async_enter: true`. SetupSession создаёт и публикует ownership/version/entering state и немедленно возвращает owner token; загрузка Admin Qwen продолжается в принадлежащем сессии joined worker. Старые клиенты без optional flag сохраняют синхронный контракт. Worker живёт независимо от HTTP request и reload, при завершении API его join выполняется до уничтожения используемых runtime/state объектов.

Браузер сразу записывает token в tab sessionStorage до любых следующих запросов. Во время entering показаны «Подготавливаем локальный AI… Это может занять некоторое время.» и indeterminate progress. Повторный Start, Send, Apply, ручные изменения и Contextual Assist блокируются; Cancel остаётся доступным. Heartbeat/status не ждут загрузки модели. Reload того же tab проверяет сохранённый token через heartbeat, сохраняет ownership и показывает preparation до автоматического перехода в editing. Новый tab без token остаётся read-only; status не раскрывает token. Ownership проверяется до ответа setup_busy, поэтому краткая блокировка checkpoint при завершении entry не считается чужой сессией. После завершения асинхронной отмены браузер очищает token.

Backend во время entry отклоняет message/draft/apply/authorize и duplicate Start. Cancel и истечение disconnect/inactivity deadline ставят pending cancellation; worker выполняет штатные setup_restore/setup_finish после окончания entry, без параллельного обращения к модели. Heartbeat и CAS/version сохраняются; окончание загрузки не продлевает heartbeat задним числом. Entry failure публикует entry_failed без автоматического повтора. Уже имеющийся локальный manual plan переносится в setup draft после editing одним CAS request; pending flag хранится при reload и расходуется до запроса, поэтому response loss не вызывает auto retry. Конфликтующие UI действия остаются заблокированными до ответа CAS, включая переход entering → editing. Apply не выполняется.

Regressions используют блокируемый model-entry seam: token до завершения loading, heartbeat/reload с прежним owner, второй tab read-only, duplicate Start и конфликтующие запросы rejected, cancel/disconnect restore, entry failure без retry, zero hidden Apply/hardware. Chromium проверяет настоящий production bundle + C++ API и две вкладки с разным sessionStorage. Language Profile generation, prompts, limits, STT и модели не изменены; в этом fix реальный NPU/voice gate не перезапускался.

Проверки startup fix (2026-10-04): frontend typecheck **PASS**, unit/state/adapter **34/34 PASS**, production frontend build/offline assets и CMake build **PASS**. Focused Setup/B2/B3 Chromium/API + Setup model **7/7 PASS**, **105.44 s**. Полный CTest **72/72 PASS**, **269.67 s**. `git diff --check` **PASS**. Legacy Chromium readiness при reload теперь ждёт новый document, чтобы не прочитать globals уходящей страницы.

Логи: `build/b3-entry-{typecheck,frontend-test,frontend-build,build,focused,ctest}.log`; Chromium artifacts — `build/b3-acceptance/`. Private demo `/tmp/firefly-b3-manual` восстановлен с исходными аргументами после проверок. После этой проверки итоговая owner acceptance и real browser voice/setup smoke завершены PASS; PR #2 разрешён к merge.

## Changed files

- `benchmarks/web-admin-b3/Acceptance.cpp`
- `benchmarks/web-admin-b3/Monitor.py`
- `docs/web-admin-ux-v2-b3.md`
- `src/admin/AdminCopilot.cpp`
- `src/admin/Api.cpp`
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
- `src/admin/frontend/src/components/ConnectionWizard.vue`
- `src/admin/frontend/src/styles/base.css`
- `src/admin/frontend/src/views/DeviceDetailView.vue`
- `src/admin/frontend/src/views/OverviewView.vue`
- `src/admin/frontend/src/views/SetupView.vue`
- `src/admin/frontend/src/views/ZonesView.vue`
- `src/admin/frontend/tests/setup.test.mjs`
- `tests/admin/SetupTests.cpp`
- `tests/admin/SetupWebBrowserTests.js`
- `tests/admin/v2/ChromiumTests.js`
- `tests/admin/v2/SetupApiTests.py`
- `tests/admin/v2/SetupChromiumTests.js`
- `tests/admin/v2/SetupWorker.cpp`
- `tests/admin12/AdminCopilotTests.cpp`

`user-store/`, build/dist/node_modules/.test-build и private artifacts не входят в B3 commit.


## Language Profile quality — owner acceptance fix

По [owner review](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/2#issuecomment-5978820214) исправлено общее разделение названий и местоположений. Admin language-profile prompt больше не требует 3–6 падежей и нескольких предлогов ради количества. `aliases` / `mentions` предлагают естественные названия; одного варианта достаточно. `locations` предлагают полные естественные фразы с привычным для места `в` / `во` / `на`. Contextual draft и suggestion prompts следуют тому же правилу, без словаря исключений для Цеха, Кухни или других названий.

Grouped decoder теперь переносит в mentions только name/short/synonym. Он не копирует туда location phrases и не отрезает от них предлог. Оба language decoder принимают `во`; language-profile decoder дополнительно требует полный предложный оборот вместо bare noun. Детерминированная `locationGrammar` не менялась: только прежние безопасные регулярные прилагательные, без новой эвристики склонения существительных.

Для первой language enrichment новой setup-зоны внутренний `initial_location` заменяет только временный singleton `в + name`. Этот placeholder исключён из данных prompt и не подмешивается к естественному AI locations. Публичный `/api/admin/language-profile` не принимает этот внутренний флаг. Для существующих зон contextual profile сохраняет ранее проверенные формы; seed name, canonical action words, conflict/safety validation и финальная ConfigLoader/RuntimeGeneration admission сохранены. Старые сохранённые profiles не переписываются автоматически: пользователь может отредактировать их перед review.

Regressions: Цех / Кухня / Детская / Склад проходят representative ON/OFF и перестановки слов через `INDEX_EXACT` без runtime Qwen. Проверяются отсутствие stripped noun mentions, сохранение reviewed forms, `во дворе`, отклонение location без предлога и запрет внешнего initial flag. B3 Chromium проверяет natural locations в новом server draft и разделённые editable поля Contextual Assist с отказом от AI-полей. Accept/Apply, ownership/version/CAS, zero hardware до явного действия, STT и frozen inference settings не менялись.

Проверки Language Profile fix (2026-10-04): frontend typecheck **PASS**, **34/34** frontend tests, production build/offline gate и CMake build **PASS**. Focused language/admin + B2/B3 Chromium/API: **14/14 PASS**, **125.04 s**. Логи: `build/b3-language-{typecheck,frontend-test,frontend-build,build,focused}.log`.

Полный CTest для Language Profile fix: **72/72 PASS**, **271.13 s** (`build/b3-language-ctest.log`); `git diff --check` **PASS**. В этом code fix microphone smoke не перезапускался; последующий owner real browser voice/setup smoke — **PASS**; Voice/STT не менялись.

Первый диагностический profile-only smoke (`build/b3-live/language-quality-20261004.*`) дал aliases/mentions `["цех"]`, `["кухня"]`, `["склад"]`, но locations `["в цехе"]`, `["в кухне"]`, `["в складе"]`. JSON validation прошла без retry (inference/API wall: 6.099/11.698 s, 6.697/13.095 s, 6.009/12.757 s); это **не считалось quality acceptance** для обычных команд «на кухне» / «на складе». Общее правило prompt дополнено выбором самого употребительного ответа на «где?»: нахождение внутри здания не повод заменять привычное «на» на «в». Использованы примеры других мест (остановка / кабинет / двор), без production mapping по названиям. Live harness теперь также проверяет ON/OFF corpus через LanguageIndex на фактических AI suggestions, без command inference, bindings или hardware.

Второй диагностический run (`build/b3-live/language-quality-final-20261004.*`, имя артефакта не означает PASS) исправил Кухню / Склад на `["на кухне"]` / `["на складе"]`, но дал `["на цехе"]`. Новый index gate вернул **FAIL** для Цеха; ошибка не маскировалась postprocessing или retry. Односторонняя подсказка про «на» заменена сбалансированным правилом: отдельно выбирать привычный предлог каждого названия, с примерами офис / улица / переговорная / площадка / двор. В этом варианте названия четырёх acceptance-зон не включались в prompt или mapping.

Третий run (`build/b3-live/language-quality-balanced-20261004.*`) снова дал «в цехе» / «в кухне» / «в складе» и **FAIL** index gate для Кухни / Склада. Поэтому location instruction привязана к реальному назначению поля — естественному уточнению команды «включи свет ...» — с двумя полными JSON-примерами (Офис / Кухня). Это few-shot guidance общей модели, без ветвления, словаря предлогов или postprocessing по имени зоны; Цех и Склад остаются вне примеров.

### Final real Admin-Qwen language smoke

**PASS**, `build/b3-live/language-quality-command-20261004.{json,log}`, memory trace `-memory.jsonl`, runner `-run.log`. Один запрос на каждую зону, `retry_count=0` у всех. Существующие Admin Qwen W8A8 / CTX4096 / RKLLM profile / budget 256 / deadline 60 s сохранены. Вся сессия с setup enter/cancel/restore: **162.47 s**. Private store `/tmp/firefly-b3-npu-Iec8Cw`; stored hash неизменён, hardware requests **0**, финального Apply нет.

| Зона | aliases = mentions (точно) | locations (точно) | Prompt bytes / context tokens | Input bytes / tokens | Output tokens | Inference wall | API wall |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Цех | `["цех"]` | `["в цехе"]` | 2209 / 443 | 49 / 24 | 18 | 5.881 s | 11.592 s |
| Кухня | `["кухня"]` | `["на кухне"]` | 2223 / 447 | 49 / 24 | 20 | 6.603 s | 13.397 s |
| Склад | `["склад"]` | `["на складе"]` | 2221 / 443 | 49 / 24 | 18 | 5.970 s | 12.328 s |

Во всех трёх ответах `conflicts: []`; raw JSON совпадает с указанными массивами без лишних aliases, bare oblique mentions или альтернативных предлогов. После authoritative decode фактические suggestions использованы для отдельного in-memory fixture с ON/OFF Света: «Включи свет» / «Выключи свет» + соответствующая обычная location phrase — **6/6 ExactUnique**, без runtime Qwen и без execution. Детская покрыта unit + B3 Chromium, отдельного live inference для неё не запускалось. AI suggestions остаются editable/reviewed; это smoke результата модели, не новая автоматическая проверка русской морфологии и не автоматическое исправление ответа.


## Final owner acceptance — 2026-10-04

**Owner manual acceptance: PASS. Real browser voice/setup smoke: PASS.** [Exact owner acceptance](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/2#issuecomment-5980447868) подтверждает ручную проверку реального V2 UI, включая итоговый voice/setup flow, и отсутствие оставшихся owner blockers. Предыдущая запись «ха» сохраняется как диагностическая история и не описывает итоговый gate. Новые timings или transcript этого owner run не выдумываются.

Обновление acceptance затрагивает только документацию/PR metadata. Code не менялся; выполнен `git diff --check`, полный rerun не нужен. Владелец разрешил Ready/merge PR #2 в master и начало B4 после merge.
