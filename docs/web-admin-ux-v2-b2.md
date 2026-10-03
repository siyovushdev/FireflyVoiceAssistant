# Web Admin UX V2 — B2 Complete Manual UX

Дата: 2026-10-03. Основа: утверждённый [дизайн](web-admin-ux-v2-design.md), B1 commit `0c81088`. Старый vanilla UI остаётся на `/`; V2 — на `/v2/`. B3 не начат. **Ручная owner acceptance B2 ещё не завершена; автоматические PASS не означают принятие владельцем.**

## Реализованная ручная настройка

- Система: изменение названия.
- Зоны: создание, редактирование, удаление с backend constraints, необязательная родительская зона и признак контейнера, описание и речевые формы.
- Устройства: создание, редактирование, удаление; «Без зоны» — полноценный сценарий. Начальный шаблон generic device содержит только ON/OFF и boolean power. Тип и operation существующего built-in устройства не преобразуются в generic автоматически.
- Возможности: boolean, integer, number, enum, string; bounds, units, string lengths, enum choices, начальное **виртуальное** значение, параметры действий, фиксированные и параметрические assignments. Общие действия показывают их consumers. Полная C++ validation остаётся окончательным ограничением.
- Operational Device Detail: активная конфигурация, состояния и время evidence, отдельные configured/mapping count/endpoint/Observe/transport сведения, guarded actions и явное чтение состояния. Защищённая дверь требует ручного текста в Playground.
- Connections: общий список, consumers, создание/редактирование/удаление с проверкой ссылок; HTTP wizard. Остальные транспорты видны как planned/disabled.
- Advanced: Preview, Mock Execute, явно подтверждаемый Production Execute; revisions, import/export, restore candidate, reconcile recovery, raw diagnostics и stored/active identities. Для schema v1 доступна подготовка schema v2 как кандидата.
- Manual voice baseline в Playground: микрофон либо локальный WAV → существующий `/api/admin/transcribe` → редактируемый transcript. Никакого автоматического Execute/Apply, browser cloud SpeechRecognition или AI conversation.

## Границы дизайна и принятые решения

Manual configuration не требует входа в AI setup. V2 читает setup status; при активной чужой сессии показывает ownership block, не захватывает её и не обходит `/setup/apply`. Создание/продолжение AI session остаётся B3; существующий setup workflow доступен через vanilla fallback. Это применение правила read-only при другом owner, а не новая competing setup session.

В B2 сохранён единый coordinator **обычного manual publication**. Адаптер owned `/setup/draft → /setup/apply` потребуется вместе с B3 conversation coordinator: V2 B2 не создаёт такую сессию. Такое ограничение отделяет завершённые manual flows от неразрешённого B3.

Локальный незавершённый импорт хранится отдельно от candidate, переживает переходы внутри SPA, инвалидирует review и блокирует общий Apply до проверки или явного удаления импорта. Это позволяет probe валидного A при незавершённом документе B без удаления B и без подстановки «починенной» конфигурации.

Structured config errors используют существующие `code/path/message`. Для Connection добавлена точность пути до индекса/поля. У нового probe отказ setup authorization имеет отдельный code; старые setup endpoints и их envelopes не переписывались.

## TaskBuffer, review и публикация

`stores/manual.ts` хранит отдельно base stored snapshot, accepted candidate, одну редактируемую задачу, непринятый import document, immutable reviewed snapshot и результат публикации. Wire-preserving clone сохраняет редкие поля; UI не пересобирает configuration из ограниченной проекции карточек.

Открытие редактора копирует checkpoint в локальный TaskBuffer. В нём могут быть пустые/невалидные поля. «Добавить в план» полностью проверяет задачу и только после успеха заменяет candidate. Ошибка сохраняет ввод. «Отмена» выбрасывает только эту локальную задачу; ранее принятые изменения остаются. Закрытие dirty dialog запрашивает отказ от задачи; reload предупреждает о локальных данных. Полная конфигурация и audio не пишутся в browser localStorage.

Единая публикация:

```text
completed local task → accepted candidate
→ full validate + stored diff + active diff
→ readable review ВСЕХ pending изменений и shared consumers
→ «Подтвердить применение»
→ перечитать setup/stored/active и проверить основу review
→ CAS PUT /api/config
→ POST /api/config/apply с identity из ответа Save
→ проверить active identity и обновить runtime projection
```

До final confirmation stored/active не меняются. Изменение формы, импорта или candidate инвалидирует review даже при возврате прежнего значения. Перед подтверждением сравниваются stored hash/revision и active hash/revision/generation. Backend CAS и generation admission остаются последними барьерами; клиентская проверка их не заменяет.

При `apply_ready:false` финальная кнопка недоступна. Recording/transcription также блокируют review/Apply. После Save success / Apply failure интерфейс показывает **«Версия сохранена, но не активирована»** и перечитывает identities. После неизвестного результата Save/Apply тоже выполняются чтения; mutations автоматически не повторяются. Несовпадение основы не исправляется автоматическим merge или удалением чужих объектов.

GET и mutations одной вкладки проходят последовательную transport queue, поскольку legacy IPC использует общий runtime admission lock. После явного изменения выполняется новое чтение после завершения ранее начатого refresh, чтобы не показывать дооперационный snapshot как итог Apply. Это координация frontend, не изменение runtime lock architecture.

## HTTP wizard

1. HTTP / REST; MQTT, TCP, UDP, Serial / RS-485, CAN disabled.
2. Новое/существующее Connection, имя, numeric IPv4 endpoint, timeout/base path в раскрываемых подробностях, shared consumers, scoped TCP probe. Результат рядом с кнопкой; изменение snapshot помечает результат устаревшим.
3. Реальные действия выбранного устройства: подключить либо явно пропустить; POST/PUT/PATCH/DELETE/body-free GET, path, typed JSON scalar fields. Для power-only предлагается `on: Да/Нет`, wire body остаётся boolean. Точный JSON раскрывается отдельно. Доступны expected status, timeout override, Content-Type, non-secret Accept/X-* headers и существующий single-argument mapping.
4. Необязательный независимый GET: top-level поле → существующее свойство с видимым типом. Nested JSONPath/coercion отсутствуют; точная совместимость полученного значения проверяется backend при чтении.
5. Connection, mappings, Observe, consumers, probe evidence; явное **«Оборудование командами ещё не проверялось»**. «Сохранить и применить» открывает общий review, а не делает Apply.

При выборе устройства в редакторе общего Connection загружаются его mappings и Observe. Существующие ссылки на другие Connections сохраняются; перевод действия или чтения на выбранное подключение требует отдельной кнопки. Enter не завершает wizard в обход его шагов.

Legacy absolute URL mappings открываются и сохраняются без автоматической конверсии. Перевод конкретного mapping на выбранное shared Connection требует отдельного нажатия и ввода path, затем отражается в review.

В task dialog есть sticky header/footer, inline field errors и summary текущего шага, focus на первое известное ошибочное поле либо summary. Ошибка вне задачи обозначает блокирующую сущность; переход не удаляет текущую незавершённую форму автоматически. В Advanced import/revisions/recovery ошибки размещены внутри соответствующего раздела.

## Исправление по ручной приёмке: required поля и примеры

При создании зоны «Цех» тексты `цех` и `в цехе` находились в placeholder, а TaskBuffer содержал пустые `speech.mentions` и `speech.locations`. Пользователь воспринимал примеры как уже заполненные значения. Backend правильно отклонял candidate: дефект был в представлении формы, не в validation.

Исправлено для любых названий зон:

- Поля остаются визуально пустыми; примеры вынесены **под** textarea с явной подписью «Пример:». Примеры не записываются в TaskBuffer и не являются AI proposal.
- Для обычной зоны оба speech поля отмечены `*` и `aria-required`; пояснение обязательности видно до submit. Для зоны-группы сохранено существующее правило: речевые формы отсутствуют. Переключение не очищает ввод автоматически.
- `stage()` проверяет незаполненные speech поля локальной задачи до HTTP validation. При ошибке TaskBuffer и ранее принятый candidate сохраняются, возле обоих полей видны русские сообщения, focus переходит к первому ошибочному полю. После заполнения одного — ко второму.
- Полная backend validation всё равно обязательна. Её конкретная ошибка о speech зоны показывается человеку по-русски; исходные `code/path/message` сохранены и доступны в Advanced/Details. Прочие backend errors не маскируются общей заменой.
- Аналогичные примеры обращений к устройству, HTTP endpoint/path, top-level Observe field, имени X-header и единиц измерения вынесены за пределы controls. Необязательные поля явно обозначены; инструкции поиска/Playground остаются отличимыми placeholders. Настоящие presets (ON/OFF, boolean body, bounds, timeout) продолжают быть реальными значениями.

Regression: unit/state tests проверяют «Цех», «Склад», «Лабораторный стенд», пустые/whitespace speech, group constraints, отсутствие validate requests до готовности, сохранение вводимых значений в candidate и технических backend details. Production Chromium через видимые controls проверяет видимые примеры отдельно от пустых значений, русские inline errors/focus, отсутствие отправки incomplete candidate, сохранение имени и настоящий speech payload после ввода. Дополнительный снимок: [required speech error](../build/b2-acceptance/zone-required-speech.png).

## Исправления последнего review PR #1

По [review владельца от 2026-10-03](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/1#pullrequestreview-5401207412) закрываются все шесть замечаний:

1. **Пробелы при наборе списков.** Общий `LineListInput` сохраняет raw строки, конечные пробелы и пустые строки во время ввода. Trim/удаление пустых строк/дубликатов выполняются при blur — после завершения редактирования поля. Это применяется к Zone mentions/locations, Device aliases, Action aliases и enum values. Незавершённый текст остаётся внутри TaskBuffer; кнопка завершения задачи сначала завершает редактирование поля, затем запускает строгую проверку.
2. **Переход к ошибке.** `TaskErrors` сам фокусирует и прокручивает адресуемое поле текущей формы, раскрывая содержащие его details. Backend path с индексом элемента списка (например `/speech/mentions/0`) адресует textarea всего списка. Для ошибки уровня сущности доступен переход в её редактор из общего review; `/import` открывает Advanced import. Кнопка отсутствует, если нет адресуемого поля или безопасного перехода. Незавершённая текущая задача автоматически не отменяется.
3. **Порт endpoint.** Подсказка показывает оба примера: `http://192.168.68.120` (порт 80) и `http://192.168.68.120:8765` (явный другой порт). Никаких изменений URL validation.
4. **Новый JSON key.** Поле начинается пустым, пример `on` находится отдельно. Рядом с недоступной кнопкой показана причина: пустое имя, существующий key либо GET без тела. После добавления поле очищается. Реальный preset `on: true/false` существующего power mapping сохранён. Остальные примеры и реальные presets проверены: повторения этой пары «default key уже существует → необъяснимый disabled» не обнаружены.
5. **Consumers wizard.** Step 5 и сборка candidate используют одну чистую проекцию pending Connection/mappings/Observe. Новый mapped device и Observe-only consumer видны до принятия задачи; удалённые связи не оставляют ложных consumers. Проекция не мутирует общий candidate.
6. **Simple review.** Подключения и mappings показываются как имя/endpoint, method/path, typed values, параметр запроса, Observe field → свойство и затронутые устройства. Executor, inherited timeout `0`, Content-Type, headers, IDs и точные структуры остаются в Advanced. Изменение только технического параметра не исчезает: review явно сообщает о нём и направляет в расширенный режим. Предупреждение о возможном сбросе evidence переведено в пользовательские термины. При открытии review очищается устаревшее сообщение «Изменения добавлены в план»; результаты Save/Apply и предупреждение о явном подтверждении сохранены.

Unit/state regression покрывает raw/normalized списки, pending/Observe-only consumers и их удаление, Simple contract/Advanced payload и technical-only diff, очистку старого outcome. Chromium вводит текст через `Input.insertText` с реальными пробелами и переводом строки; проверяет фокус по ссылке текущей ошибки и реальной backend ошибки ambiguous alias с индексом, переход к блокирующему импорту, отсутствие нерабочих links, обе подсказки порта, JSON key reasons, pending consumer, Simple/Advanced review и прежние hardware counts. Все сценарии работают через production bundle/C++ server и временные stores.

B3 AI-first conversation и Contextual AI Assist остаются требованиями ниже; AI calls не добавлены. Owner acceptance всё ещё открыта, PR остаётся Draft.

## Remaining B3: Contextual AI Assist inside manual editors

B3 должен сохранить **два равноправных AI-assisted способа** настройки:

1. **PRIMARY — AI-first conversation / voice**: запрос «Добавь зону Цех» → proposal card с названием, description, aliases, mentions, locations → review/edit → подтверждение в plan → отдельное явное Apply.
2. **CONTEXTUAL — «✨ Заполнить остальные поля с AI» внутри manual editor**: по явно нажатой кнопке AI предлагает настоящие editable TaskBuffer values с маркировкой «✨ Предложено AI — проверьте перед добавлением». Пользователь может изменить, принять или отказаться. **AI proposal ≠ placeholder**; никакого автоматического принятия в plan или Apply.

Contextual Assist нужен для Zone, Device, Action/Capability и language aliases/forms. Существующий `/api/admin/language-profile` и action-draft/suggestions старой админки остаются функциональной основой; перенос в V2 нельзя потерять за AI-first conversation. В B2 эти AI calls/кнопки **не реализованы**, manual flows работают без AI; старый UI остаётся fallback.

Для Connection AI может использовать только известные факты и запрашивать недостающее. Нельзя придумывать IP, port, method/path, payload, response field, credentials или protocol. Семантические и connectivity proposals подтверждаются отдельно; hardware execution и silent Apply для AI недопустимы.

## Точные API изменения

### `POST /api/connections/probe`

```json
{
  "connection": {
    "id": "connection_http_1",
    "name": "Контроллер цеха",
    "type": "http",
    "enabled": true,
    "timeout_ms": 2000,
    "http": { "base_url": "http://192.168.68.120", "base_path": "" }
  }
}
```

При активной setup session требуется её `session_id` в JSON, как у прежних scoped Admin operations. Mutation header/origin guards HttpServer обязательны и неизменны.

Успех: HTTP 200, `{ok:true, scope:"tcp_connect_only", saved:false, applied:false}`. Невалидная Connection либо неуспешный transport probe: HTTP 422, прежний `{ok:false, errors:[{code,path,message}]}`. Неверный setup owner: HTTP 409 с `setup_authorization_failed`, path `/session_id`. Неправильная форма запроса: 400; отсутствие mutation guard: 403.

`ConfigLoader::connectionFromJson` использует bundled schema `$defs/connection`, прежний decoder, общий identity-name validator и `validateConnections`. `HttpHardwareExecutor::testConnection` делает только TCP connect/close. Нет второго URL parser, HTTP bytes, Save, draft writes, DeviceStateStore updates, generation publication или execution events. Старый `/api/connections/test` сохраняется.

Для full config transport errors путь стал `/connections/<index>/<field>`, для isolated probe — `/connection/<field>`. Семантика error codes и строгой полной validation сохранена. Выделение общего name validator сохраняет ограничение непустого имени и 128 UTF-8 bytes в обоих путях.

### `POST /api/devices/command`

```json
{
  "text": "включи вентилятор в цехе",
  "expected_hash": "<64 lowercase hex characters>",
  "expected_action": {
    "target_id": "device_fan",
    "action_id": "action_on",
    "arguments": {}
  }
}
```

Приём в `RuntimeGenerations::Request` удерживает одну active generation и прежний operation lock. Hash проверяется до обработки. Используется прежний `CommandEngine → SafetyGate → domain validation`. Model adapter этой **дополнительной guarded дорожки** отказывает перед обращением к модели, если понадобился fallback. Обычный model adapter, production voice, parser, LanguageIndex и SafetyGate semantics сохранены.

Перед прежним Executor проверяются FastPath, безопасный/валидный результат и точное равенство GenericAction, включая типы scalar arguments. Expected action ничего не разрешает сам по себе. Door target отвергается в этой дорожке; явная ручная команда остаётся в Playground. Результат содержит прежние route/language_route/safety/domain/execution diagnostics. Новый diagnostic source: `admin-device`.

Rejections: `revision_conflict`, `deterministic_required`, `command_rejected`, `expected_action_mismatch`, `explicit_manual_intent_required`. Они дают HTTP 422, `ok:false`, **нулевое hardware execution**. Активный setup блокирует endpoint с HTTP 409. Guard errors возвращаются до Executor; raw model inference для них отсутствует. Предсуществующий Playground endpoint не изменяет полномочия.

Если admission прошёл, но transport вернул ошибку, прежний command envelope может иметь HTTP 200/`ok:true` и `execution.success:false`. UI проверяет **execution**, не HTTP status. Timeout не считается доказательством OFF или невыполненного физического действия; UI сообщает неизвестный результат и не повторяет POST. Binding missing отдельно сообщает, что действие не отправлено.

`POST /api/devices/poll` без изменений: explicit target + active expected hash, один GET; успешное независимое наблюдение даёт Confirmed. Автоматический status refresh читает только backend memory.

## Voice baseline и ограничения

В Playground доступны WAV upload и browser microphone capture (mono PCM16, до 15 секунд, локальный AudioContext). Capture закрывает tracks/context при остановке, ошибке и unmount; поздний transcript после ухода игнорируется. Текст всегда редактируемый. Secure-context требование отображается явно: localhost или SSH tunnel. LAN HTTPS/mobile deployment, cloud STT и B3 chat не реализованы.

Реальный microphone/STT/NPU smoke не является частью текущего автоматизированного browser fixture: там используется mock inference worker без transcriber. Существующие AdminTranscribe regression tests сохраняются. Нельзя трактовать browser acceptance как новую оценку качества русского STT или модели.

Ограничения backend сохранены: numeric IPv4 HTTP, плоские scalar bodies, один argument mapping, top-level Observe, существующие constraints/retired IDs/compatibility. Если capability не позволяет составить безопасную краткую фразу из уже заданных aliases и параметров, Device Detail предлагает Playground. Expected-action guard остаётся обязательным и может отклонить неоднозначную фразу. Credentials, auth, новые executors и runtime architecture rewrite отсутствуют.

## Проверка и артефакты

Проверено на Linux/aarch64: Node v22.23.2 (build/test tool), Chromium 120.0.6099.224, `DISPLAY=:0`. CMake зарегистрировал оба Chromium suites; отсутствие browser/toolchain/display в другом окружении не означает их успешного прохождения.

Команды из корня репозитория:

```sh
npm --prefix src/admin/frontend run typecheck
npm --prefix src/admin/frontend run test
npm --prefix src/admin/frontend run build
cmake -S . -B build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_ADMIN_DIST_DIR=/home/firefly/FireflyVoiceAssistantLinux/src/admin/frontend/dist
cmake --build build -j4
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^(AdminV2Chromium|AdminManualChromium|AdminManualApi|AdminGuardedCommand)$' --output-on-failure
git diff --check
```

Зависимости B1 не менялись, новые npm packages не добавлялись. CMake использует готовый dist и build-generated embedded allowlist из B1; сеть и npm install не запускаются. На runtime Node не нужен, `/` остаётся vanilla.

Frontend tests: DTO preservation, TaskBuffer isolation/cancel, all-pending/shared-consumer review, review invalidation, CAS/stale identities/generation, lost Save response, saved-not-active, ownership, import and voice blocking, typed values и pinned command intent.

`AdminGuardedCommand`: deterministic pipeline, mismatch/stale/door rejection и **нулевое число вызовов model inference**, включая fallback attempt.

`AdminManualApi`: настоящий C++ server, приватный store, локальный TCP/HTTP mock; валидный A и invalid B, no HTTP bytes from probe, неизменные stored/active/state/history, strict full validation/Save rejection, header/origin/setup ownership, ON/OFF/poll counts, wrong hash/target/action/arguments, `INDEX_EXACT → Safety REJECT` и ноль дополнительных HTTP requests.

`AdminManualChromium`: production Vite bundle через C++ HttpServer. Happy path использует **видимые controls**, не присваивает Vue candidate через JS. Покрыты A–N в сочетании с API safety contracts и B1 evidence/stale suite: zone create/edit, no-zone device, Цех → Вентилятор, wizard/probe/inline focus, typed capabilities, all-pending review, edit invalidation, Cancel checkpoint, shared consumers и сохранение отдельных Connection references/Observe при выборе устройства, запрет завершения wizard по Enter, ON/OFF → Predicted → GET Confirmed, unfinished import B + probe A, import/restore/reconcile, Playground modes, Save-success/Apply-failure и отсутствие blind retry. Counts оборудования проверяются: основной ON/OFF/readback путь — POST/GET/POST/GET; отдельный явно подтверждённый Playground Production — ещё один POST. Preview, Mock, конфигурационные операции и refresh не добавляют HTTP requests оборудования.

Failure fixture ограничена test-only `ManualWorker.cpp`: marker включает отказ Apply после успешных review/Save, сохраняя реальную предыдущую runtime generation. Она не включена в production binary. Все stores временные; пользовательский `user-store` не используется.

Сохранены проверки B1: read-only browsing, active/stored separation, Unknown/Predicted/Confirmed/реальный Stale TTL, routes/reload, MIME/CSP/allowlist/offline. Ожидание placeholder для уже реализованных B2 разделов заменено ожиданием рабочих views; AI Setup остаётся placeholder. После reload тест теперь ожидает новый document (`performance.timeOrigin`), чтобы не принять старый DOM за завершённую перезагрузку.

Локальные артефакты (build directory не коммитится):

- [B2 acceptance + requests/counts](../build/b2-acceptance/acceptance.json), [server log](../build/b2-acceptance/server.log).
- [Wizard error 390×844](../build/b2-acceptance/wizard-error-390.png), [800×600](../build/b2-acceptance/wizard-error-800.png), [1280×800](../build/b2-acceptance/wizard-error-1280.png).
- [Final review](../build/b2-acceptance/review.png), [Device operational](../build/b2-acceptance/device-operational.png), [Saved, not active](../build/b2-acceptance/saved-not-active.png).
- [Frontend](../build/b2-frontend.log), [CMake configure](../build/b2-configure.log), [build](../build/b2-build.log), [full CTest](../build/b2-ctest.log), [focused acceptance](../build/b2-focused.log), [git status](../build/b2-git-status.txt).

В `build/v2-acceptance/` остаются отдельные B1 artifacts. Browser network interception отвергает внешние requests.

Результаты окончательной сборки после исправления всех шести замечаний review PR #1:

| Проверка | Результат |
| --- | --- |
| TypeScript / vue-tsc | PASS |
| Frontend unit/state после review | 24/24 PASS |
| Vite + offline artifact check | PASS; 4 объявленных локальных файла, hashed JS/CSS |
| CMake configure / build `-j4` | PASS |
| Полный CTest | **70/70 PASS**, 230.33 s |
| B1 `AdminV2Chromium` внутри полного CTest | PASS, 63.30 s; реальный Stale TTL |
| B2 `AdminManualChromium` внутри полного CTest | PASS, 32.28 s |
| Отдельный focused V2/API run | **4/4 PASS**, 99.90 s; B1 Chromium 63.30 s, guarded 0.08 s, API 4.25 s, B2 Chromium 32.26 s |
| `git diff --check` + whitespace новых файлов | PASS |

API contract suite содержит 3 integration-теста; CTest регистрирует их как один `AdminManualApi`. Оба Chromium suites зарегистрированы и фактически исполнялись через production static dist и C++ HttpServer; Chromium не заменён mock DOM.

Промежуточные отказы разобраны: гонка ожидания reload в B1 harness исправлена, пересечение пересборки test worker с запущенным CTest устранено последовательным запуском. Числа выше относятся к полностью завершённому запуску после финальной сборки.

После review проверки повторены последовательно: frontend → `cmake --build build -j4` с новым production bundle → focused Chromium/API → полный CTest. Логи: [frontend](../build/b2-review-frontend.log), [focused](../build/b2-review-focused.log), [сборка](../build/b2-review-build.log), [полный CTest](../build/b2-review-ctest.log). Ручная owner acceptance, включая повторное создание зоны через `/v2/`, остаётся обязательной до принятия milestone.

## Доступ и продолжение

После штатного запуска/перезапуска пересобранного C++ server:

- `http://localhost:8080/v2/#/overview`
- `http://localhost:8080/v2/#/devices`
- `http://localhost:8080/v2/#/connections`
- `http://localhost:8080/v2/#/advanced`
- `http://localhost:8080/v2/#/advanced/playground`

Production service в этой работе не перезапускается; acceptance поднимает и останавливает свои localhost servers. Доступ с другой машины — прежний SSH tunnel. Пользовательские изменения `user-store` не изменяются и не включаются в список B2.

B3 остаются AI-first conversation, semantic→connectivity coordinator, owned setup publication adapter, AI follow-up/no-zone create, durable transcript и Admin prompt compatibility work. Production model/prompt/SafetyGate/LanguageIndex не менялись. B2 не удаляет vanilla. Публикация: ветка `feature/web-admin-ux-v2-b2`, Draft PR в `master`; merge запрещён до отдельного решения владельца.

## Изменённые и новые файлы

Состав B2 относительно B1: **24 изменённых файла + 26 новых файлов**. В рабочем дереве также остаются исходные пользовательские изменения трёх `user-store/*.json` и один untracked backup; они не относятся к B2 и не затрагивались. Staging выполняется только по явному списку файлов B2, без `user-store`, build, dist, node_modules и local artifacts.

Полный список B2 (50 файлов), без пользовательского `user-store` и build artifacts:

- `docs/web-admin-ux-v2-b2.md`
- `include/firefly/admin/RuntimeGeneration.h`
- `include/firefly/config/ConfigLoader.h`
- `include/firefly/config/ConfigValidator.h`
- `src/admin/Api.cpp`
- `src/admin/CMakeLists.txt`
- `src/admin/RuntimeGeneration.cpp`
- `src/admin/RuntimeWorker.cpp`
- `src/admin/frontend/src/App.vue`
- `src/admin/frontend/src/adapters/catalog.ts`
- `src/admin/frontend/src/adapters/commands.ts`
- `src/admin/frontend/src/adapters/editing.ts`
- `src/admin/frontend/src/adapters/manualValidation.ts`
- `src/admin/frontend/src/api/admin.ts`
- `src/admin/frontend/src/api/client.ts`
- `src/admin/frontend/src/api/transport.ts`
- `src/admin/frontend/src/components/AdminVoice.vue`
- `src/admin/frontend/src/components/ConnectionWizard.vue`
- `src/admin/frontend/src/components/DeviceControls.vue`
- `src/admin/frontend/src/components/EntityEditor.vue`
- `src/admin/frontend/src/components/HttpMappingEditor.vue`
- `src/admin/frontend/src/components/PropertyEvidence.vue`
- `src/admin/frontend/src/components/TaskPanel.vue`
- `src/admin/frontend/src/components/forms/FieldSchema.vue`
- `src/admin/frontend/src/components/forms/LineListInput.vue`
- `src/admin/frontend/src/components/forms/ScalarInput.vue`
- `src/admin/frontend/src/components/forms/TaskErrors.vue`
- `src/admin/frontend/src/composables/useRefresh.ts`
- `src/admin/frontend/src/router/index.ts`
- `src/admin/frontend/src/stores/manual.ts`
- `src/admin/frontend/src/stores/system.ts`
- `src/admin/frontend/src/styles/base.css`
- `src/admin/frontend/src/types/wire.ts`
- `src/admin/frontend/src/views/AdvancedView.vue`
- `src/admin/frontend/src/views/ConnectionsView.vue`
- `src/admin/frontend/src/views/DeviceDetailView.vue`
- `src/admin/frontend/src/views/DevicesView.vue`
- `src/admin/frontend/src/views/DiagnosticsView.vue`
- `src/admin/frontend/src/views/OverviewView.vue`
- `src/admin/frontend/src/views/PlaygroundView.vue`
- `src/admin/frontend/src/views/ZonesView.vue`
- `src/admin/frontend/tests/manual.test.mjs`
- `src/config/ConfigLoader.cpp`
- `src/config/ConfigValidator.cpp`
- `src/config/Connectivity.cpp`
- `tests/admin/v2/ChromiumTests.js`
- `tests/admin/v2/GuardedCommandTests.cpp`
- `tests/admin/v2/ManualApiTests.py`
- `tests/admin/v2/ManualChromiumTests.js`
- `tests/admin/v2/ManualWorker.cpp`
