# Web Admin UX V2 + AI-first setup

Статус: **исторический утверждённый design**. Реализация B1–B3 завершена; default migration — [B4](web-admin-ux-v2-b4.md). Дата: 2026-10-03. Изученная рабочая точка: `bc52022` — «Добавить Device Connectivity и HTTP bindings».

Firefly — универсальная локальная embedded/edge automation platform. Новый Web Admin строится вокруг задачи пользователя: описать систему, проверить предложенные устройства и возможности, настроить связь с оборудованием, явно применить изменения. AI и голос — основной вход в настройку; визуальный интерфейс показывает результат и обеспечивает ручную работу.

Предлагается новый frontend на Vue 3 + TypeScript + Vite поверх существующего C++ backend. ConfigStore, immutable validated catalog, Draft, validation, diff, CAS, Apply, runtime generations, LanguageIndex, SafetyGate, Bindings, Executor и DeviceStateStore сохраняются. Это не переименование старых форм и не backend rewrite.

В этом документе «предлагается» относится к состоянию design на 2026-10-03; актуальная реализация и delivery описаны в отчётах B1–B4. Исторические результаты тестов отделены от критериев будущей приёмки.

## 1. Current-state analysis

### 1.1. Что изучено

| Источник | Существенное для V2 |
| --- | --- |
| [index.html](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/index.html), [admin.css](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/admin.css) | Одна длинная страница с якорями, карточками-формами и несколькими dialog; Simple/Advanced переключается CSS. Универсальные названия уже частично введены. |
| [admin.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/admin.js) | Глобальные draft/stored/expected/active, ручное построение DOM, общий `run()` блокирует почти все controls; ошибки копируются в общую панель и открытые dialog. Здесь же setup lifecycle, WAV capture, API, review, apply, состояния. |
| [admin-model.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/admin-model.js) | Полезные чистые функции: отображение diff/errors/state, числовые ограничения, completeness bindings, voice state machine. Их поведение и тесты можно перенести в TS; глобальные зависимости переносить не нужно. |
| [simple-editor.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/simple-editor.js), [typed-editor.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/typed-editor.js) | Уже есть человекочитаемые описания effects и типизированные boolean/integer/number/string/enum. Но карточка совмещает просмотр, редактирование схемы и настройку исполнения. |
| [connection-editor.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/src/admin/web/connection-editor.js) | Редактирует общий draft немедленно; создание Connection вставляет `Новое подключение` с пустым URL. ON/OFF и Observe раскрываются в том же длинном dialog. |
| [Api.cpp](../src/admin/Api.cpp), [AdminDraft.cpp](../src/admin/AdminDraft.cpp) | Уже существуют validation, diff, CAS save/apply, restore/import candidates, state/poll и strict error envelopes. Diff connectivity сейчас агрегирует целые массивы. |
| [AdminCopilot.cpp](../src/admin/AdminCopilot.cpp) | Structured semantic/connectivity operations, hash-bound proposal acceptance, отдельный запрет смешивать их в одном proposal. Acceptance меняет только возвращаемый candidate. |
| [SetupSession.h](../src/admin/SetupSession.h), [DialogueOperations.h](../src/admin/DialogueOperations.h), [RuntimeWorker.cpp](../src/admin/RuntimeWorker.cpp) | Server-owned setup draft, owner token/version, resume, heartbeat, rollback; текущий dialogue decoder ограничен семантическими операциями. Это отдельная дорожка относительно полного Copilot. |
| [HttpServer.cpp](../src/admin/HttpServer.cpp), [CMakeLists.txt](../src/admin/CMakeLists.txt) | CMake копирует семь известных assets; сервер загружает их по фиксированному allowlist. CSP self-only, no-store, same-origin mutation guard, `X-Firefly-Admin: 1`. Node runtime для UI отсутствует. |
| [schema-v2.json](../config/schema-v2.json), [Configuration.h](../include/firefly/config/Configuration.h) | Уже универсальные Location/Device/ActionDefinition; `Room = Location`, nullable `room_id`, metadata hierarchy, connections/observations. JSON Schema не заменяет C++ reference/domain validation. |
| [Device Connectivity & Binding Admin](device-connectivity-binding-admin.md), [диалоговая настройка](dialogue-setup.md) | Зафиксированные lifecycle, transport limits, state evidence и существующая приёмка. |

Изучены связанные Setup, Web Admin, Copilot и Connectivity tests; конкретная карта сохранения покрытия приведена в разделе 11. Production references/model files для этой задачи не нужны и не изменяются. Имеющиеся пользовательские изменения в `user-store/` не относятся к работе над документом.

### 1.2. Сильные стороны

Backend уже отделяет семантику устройства от транспорта. Connection может обслуживать несколько Devices; Binding соединяет конкретные target/action с транспортом. Observe отдельно подтверждает физическое состояние. Переименование сохраняет stable IDs; изменение физического mapping сбрасывает несовместимые свидетельства состояния.

Настройка уже не требует доверять свободному тексту модели: strict decoder создаёт candidate, validator проверяет граф, Apply публикует generation. Существуют отказ по stale proposal, CAS конфликт, setup ownership и восстановление после неудачного Apply. Manual editing и диагностика не зависят от успешного AI ответа.

Browser acceptance уже проходит через реальные C++ HTTP endpoints и локальный mock; это хорошая база для проверки нового интерфейса без реального оборудования.

### 1.3. Почему UX перестал масштабироваться

| Наблюдение | Причина в текущей реализации | Решение V2 |
| --- | --- | --- |
| Проверяется ESP32 Test, ошибка относится к другому Connection | `/api/connections/test` сначала вызывает `ConfigLoader::fromJson` для **всего** configuration | Отдельный безопасный probe одного Connection; invalid local forms не попадают в общий candidate |
| Результат Preview далеко от кнопки, ошибка теряется сверху | Один scrollable dialog; результат после кнопок, общая error rendering | Отдельный Review step и error рядом с полем/операцией, sticky footer |
| Нужно понимать Save / Apply / оставить draft | UI отражает стадии хранения и публикации | В Simple одна задача «Сохранить и применить» с обязательным итоговым review |
| Пользователь создаёт свет, а заполняет language/state/assignments/bindings | Все уровни domain model одновременно редактируются | AI proposal + Device Detail + отдельный connection wizard |
| Непонятно, работает ли подключение | Completeness считает наличие bindings; это не доступность и не подтверждение API | Разделить «настроено», «TCP проверен», «последний обмен» и «состояние подтверждено» |
| Любая операция замораживает всю страницу | Общий `operation` и глобальное отключение controls | Раздельные loading/error scopes при сохранении серверной сериализации mutations |
| Чат выглядит общим, но connectivity там не поддерживается | `/setup/message` использует ограниченный dialogue format; полный `/admin/copilot` — отдельный API | Один conversation coordinator с явными семантической и connectivity фазами |

Важная подробность: `SetupSession::split()` делит сообщение по точкам, `;` и newline. Отправлять туда HTTP-текст с `192.168.68.120` и JSON как обычное semantic message нельзя: кроме отсутствия соответствующих dialogue operations, адрес разбивается на шаги. Для connectivity V2 использует полный Copilot endpoint, не «чинит» адрес после split.

В текущем setup prompt ещё встречается «настройка дома», а dialogue location получает metadata type `room`. Это реальное наследие, а не основание менять доменную архитектуру. UI переводит сущность как «Зона»; изменение **Admin-only** формулировок предлагается отдельно от frontend и требует regression. Production command prompt не меняется.

## 2. Product model

### 2.1. Термины и wire compatibility

| Product entity | Значение и представление | Существующее хранение |
| --- | --- | --- |
| System / Workspace — «Система» | Одна управляемая локальная конфигурация Firefly с именем пользователя: «Лабораторный стенд», «Склад», «Автомобиль», «Офис» | `system`; V2 не вводит multi-tenant workspaces |
| Zone / Location — «Зона» | Необязательное место/участок: кабинет, линия, стеллаж, секция транспорта. Иерархия и группы опциональны | `rooms[]`, `Location`, alias `Room`, `metadata.parent_id/container`, `speech` |
| Device — «Устройство» | Логическая управляемая единица: свет, насос, привод, тестовый стенд; может быть без зоны | `devices[]`, nullable `room_id`; `kind` и metadata остаются |
| Capability — «Возможность» | Что пользователь может сделать и увидеть: включить, изменить скорость, выбрать режим, прочитать температуру | UI projection из `action_ids`, `actions[]`, parameters, assignments, state fields; новый backend entity не нужен |
| Action — «Действие» | Конкретная разрешённая операция, её аргументы и ограничения | `ActionDefinition`, `GenericAction`; названия operation enum сохраняются |
| State — «Состояние» | Значение свойства плюс источник знания и время | Схема в configuration; physical evidence в DeviceStateStore; Mock state отдельно |
| Connection — «Подключение» | Общий endpoint/канал контроллера, потенциально для нескольких Devices | `connections[]`; не считается устройством автоматически |
| Binding — «Настройка действия» | Связь действия с HTTP method/path/body | `bindings[]`, ключ `(target_id, action_id)`; отдельного binding ID сейчас нет |
| Observation — «Чтение состояния» | Отдельный GET и соответствие поля ответа свойству устройства | `observations[]`; не Action ON/OFF и не симуляция |
| Integration — «Интеграция» | Название раздела для транспорта и общего оборудования | UI grouping; новый registry/plugins backend не предлагается |

`rooms`, `room_id`, `Room`, `roomId`, `light`, `entrance_door`, `mock.assign.v1`, compatibility mappings и исторические имена fixtures можно оставить. `mock.assign.v1` описывает существующую semantic operation; само это имя не означает, что HTTP device остаётся виртуальным.

Navigation и empty states не содержат обязательного Home/Room. Домашние примеры допустимы наравне с «Цех → Вентилятор» и «Стенд без зоны → Реле». Выбор типа деятельности не меняет SafetyGate и не открывает новые executors.

### 2.2. Границы универсальности

Это универсальная организация **поддерживаемых** сущностей и scalar contracts, а не обещание управлять произвольным оборудованием без интеграции. Schema v2 допускает до 64 зон, 128 устройств, 32 actions, 128 connections и 128 observations; фактическая admission дополнительно ограничена domain/model context. Нельзя выдавать schema maximum за проверенную ёмкость AI setup.

Для generic Device зона необязательна уже сейчас. Но Copilot `create_device` требует разрешимую location, а dialogue `power_device` — непустую зону. В V2 ручное создание «Без зоны» обязательно с первого parity этапа. Полная AI-поддержка такого создания потребует узкого Admin operation change: explicit no-location selection → nullable location, без автоматического создания фиктивной зоны. Это отдельный пункт API/Admin impact, не backend migration.

## 3. Information architecture

Предлагаются route views вместо якорей одной страницы. Маршруты ниже — hash routes (`/#/...`), stable IDs в URL не отображаются как пользовательские названия.

| Navigation / route | Содержание и основной CTA |
| --- | --- |
| **AI Настройка** `/#/setup` | Главный вход: текст/микрофон, conversation, proposal cards, текущая задача, review, состояние setup session |
| **Обзор** `/#/overview` | Имя системы, readiness, состояние ассистента, неприменённая задача, зоны/устройства, последние результаты. «Настроить с AI», «Добавить вручную» |
| **Зоны** `/#/zones` | Опциональное дерево, поиск, список дочерних зон и устройств; `/#/zones/:id` — detail, breadcrumb, add device |
| **Устройства** `/#/devices` | Поиск и фильтры по зоне, configured/partial/unconfigured, physical state; группа «Без зоны»; карточки или компактный список |
| Device Detail `/#/devices/:id` | Обзор, Действия, Подключение, Язык команд; Advanced tab с техническими деталями |
| **Подключения** `/#/connections` | Общие Connection, используемые устройства, endpoint и последние известные результаты; `/#/connections/:id` — consumers и редактирование |
| **Состояния** `/#/states` | Сводка physical properties и evidence, фильтр неизвестных/устаревших/ошибок; ссылки на Device Detail |
| **Дополнительно** `/#/advanced` | Возможности/схемы, bindings, конфигурация, импорт/экспорт, версии, Command Playground; дочерние routes `/capabilities`, `/configuration`, `/revisions`, `/playground` |
| **Диагностика** `/#/diagnostics` | Runtime/LLM/STT/setup health, последние ошибки, language route, raw proposal и execution diagnostics, generation details |

В sidebar AI Настройка визуально главный пункт. Empty system открывает AI Настройку с предложением начать; настроенная система открывает Обзор и сохраняет видимый AI composer entry. Загрузка страницы сама по себе не начинает setup и не приостанавливает ассистент.

В global header: имя системы, «Работает / Настройка / Ограниченный режим», индикатор неприменённых изменений, переключатель «Обычный / Подробный». Пока активна setup session, её banner присутствует на всех routes: «Идёт настройка. Рабочие команды приостановлены» + «Вернуться к настройке».

Обзор не выводит raw hashes и не считает `dashboard.simple.executor = mock` описанием всей системы: это текущий агрегат, недостаточный для production HTTP. View adapter использует active bindings, health и отдельные execution/state records.

Ошибки доступны из любого режима через «Подробнее» без необходимости искать диагностический экран. Device Detail отображает **active** состояние; ещё не применённое устройство имеет явную метку «В плане» и не имеет активных физических controls.

Пример после Apply и независимого успешного Observe:

```text
Свет                                      [Изменить с AI]
Детская

Питание: Включено
Подтверждено устройством · прочитано 12:41:03

[Включить]  [Выключить]  [Прочитать состояние]

Подключение: ESP32 Детская
Настроены 2 действия и чтение состояния
Последний успешный обмен: 12:41:03
[Настроить подключение]
```

«Available» не постоянная истина: показывается свежесть последнего свидетельства. TCP test result существует отдельно и не меняет physical state.

## 4. AI-first setup UX

### 4.1. Общие правила conversation

Текст и голос используют один pipeline подготовки предложения. Пользователь сначала нажимает «Начать настройку» с пояснением, что рабочие команды временно приостановятся. UI ждёт подтверждённого server setup status, показывает фазу, даёт отменить с учётом синхронного inference. Нельзя обещать мгновенную отмену vendor вызова.

Предложение показано карточкой с конкретными именами, возможностями, before/after и источником «Предложено AI». Свободный `summary` модели — пояснение, но фактический review строится из валидированного candidate/diff. AI не создаёт зелёные success badges своим текстом.

Три разных события названы явно:

1. **«Принять в план»** — человек согласился с proposal; это не production.
2. **«Сохранить и применить»** — открыть общий итоговый review, validate/diff; пока ничего не сохранять.
3. **«Подтвердить применение»** — единственное разрешение на CAS save + Apply данного рассмотренного candidate.

Несколько proposal cards могут составлять одну задачу с одним итоговым Apply. Semantic proposal подтверждается до connectivity proposal. Пользователь также может закончить на семантике, применить её отдельно и позже продолжить ту же визуальную беседу новой setup session. UI показывает границу «Уже применено»; отмена следующего шага не отменяет предыдущий commit конфигурации.

Conversation context — текущий validated draft, выбранные человеком entity references и последние относящиеся к задаче реплики, а не неограниченный prompt transcript. Длинная история не отправляется модели целиком. Неизвестные IP/path/payload/credentials остаются вопросами. Невозможность выразить запрос текущими operations показывается как ограничение с ручным fallback.

Текущий strict decoder проверяет структуру и допустимость catalog, но сам по себе не доказывает, что каждый network fact взят из просьбы пользователя. Поэтому connectivity review связывает адрес, method/path, payload и response fields с явно введёнными данными либо существующим выбранным mapping. Новый факт без такого источника помечается как требующий ввода/подтверждения и блокирует принятие карточки. Нельзя превращать правдоподобный AI-generated IP в «известный адрес» зелёной галочкой validation. Platform defaults отображаются отдельно от сведений об оборудовании; credentials не запрашиваются.

### 4.2. A — пустая система → зоны → устройства → возможности

| Этап | Что видит пользователь | Поведение |
| --- | --- | --- |
| Вход | «Опишите систему или отдельное устройство». Примеры: «Создай зону упаковки и вентилятор», «Добавь оборудование стенда». `[Начать настройку] [Вручную]` | Нет скрытого setup/start |
| Просьба | «Создай детскую, добавь туда свет и телевизор. Оба только включаются и выключаются.» | `/setup/message`; видимый прогресс без фиктивных процентов |
| Proposal | «Зона: Детская. Свет: Включить, Выключить. Телевизор: Включить, Выключить. Подключения ещё не заданы». `[Принять в план] [Изменить] [Отклонить]` | Ровно power-only contracts; яркость, громкость, каналы не добавляются |
| Принятие | «В плане: 1 зона, 2 устройства. Теперь можно настроить подключение». `[Подключить Свет] [Подключить Телевизор] [Сохранить и применить]` | Следующая physical фаза использует IDs из проверенного семантического candidate |
| Review | Те же сущности, примеры команд и «Физическое управление пока не настроено». `[Назад] [Подтвердить применение]` | Full validation/admission, diff, explicit final confirmation |
| Результат | «Созданы Детская, Свет и Телевизор. Ассистент работает». Device cards: «Подключение не настроено», physical state неизвестен | Не показывать initial virtual value как прочитанное оборудование |

Текущий `/setup/message` уже помещает результат в **server draft** до review человеком. V2 честно считает его непринятым proposal и блокирует переход к connectivity/Apply до «Принять в план». Отклонение возвращает предыдущее полное валидное snapshot через `/setup/draft` с текущей version. Это frontend review state, а не ложное утверждение, что server draft ещё не менялся. После reload неподтверждённый snapshot требует повторного review.

### 4.3. B — одно устройство в существующей зоне

Из карточки «Цех упаковки» пользователь выбирает «Добавить с AI». Над composer виден chip «Зона: Цех упаковки» с возможностью снять его. Просьба: «Добавь вентилятор, только включение и выключение».

UI показывает явный контекст выбранной зоны; adapter передаёт его в запросе как контекст настройки, не угадывает по последнему открытому экрану. Если сервер не принимает отдельное context поле, adapter добавляет видимую пользователю строку контекста к Admin тексту. Production command input это не затрагивает.

Proposal: «Добавить Вентилятор в Цех упаковки. Возможности: включить/выключить. Существующие устройства и язык зоны не меняются». Далее принять → подключить или review/apply. Если найдено два устройства с таким именем, AI предлагает уточнить, а не перезаписывает первое.

Для устройства без зоны пользователь явно выбирает «Без зоны». До отдельного расширения Admin operations доступен тот же manual form; создание скрытой комнаты ради AI недопустимо.

### 4.4. C — HTTP Connection + ON/OFF + Observe

После принятия semantic proposal пользователь остаётся в беседе:

> Свет подключён к ESP32 по адресу 192.168.68.120. Включение POST /relay/1 {on:true}, выключение {on:false}, состояние GET /relay/1, поле on.

Это вход в **connectivity phase**, с выбранным target «Свет · Детская». Текст с IP/JSON передаётся целиком в `/api/admin/copilot`, не в sentence splitter `/setup/message`. При переходе из обычного composer UI предлагает карточку «Настроить подключение Света»; неоднозначная классификация задачи требует выбора, а не автоматического пробного исполнения.

| Этап | Экран |
| --- | --- |
| Уточнение сокращений | «Для выключения также POST /relay/1? Подключение по HTTP? Поле on соответствует питанию Света?» Выбранные ответы видны в transcript. Не считать пропущенный OFF method/path известным только потому, что ON задан. |
| Proposal | «Новое подключение: ESP32 Детская; адрес http://192.168.68.120. Включить: POST /relay/1, on = Да. Выключить: POST /relay/1, on = Нет. Читать состояние: GET /relay/1, on → Питание (Да/Нет)». Дополнительно раскрывается точный JSON `{"on":true}` / `{"on":false}`. |
| Проверка | `[Проверить доступность адреса]` → «TCP соединение установлено. API и действия ещё не проверялись». Состояние подключения к endpoint и проверка configuration отображаются раздельно. |
| Принятие | `[Принять подключение в план] [Изменить вручную] [Отклонить]`. Карточка перечисляет shared consumers, если изменяется общий Connection. |
| Итог | `[Сохранить и применить]` → review semantic + physical изменений всей задачи → `[Подтвердить применение]`. Напоминание: «Команды оборудованию сейчас отправляться не будут». |
| После Apply | «Подключение настроено». `[Прочитать состояние]` делает один отдельный GET. ON/OFF доступны в Device Detail после завершения setup. |

Для нового mapping wizard предлагает явные редактируемые platform defaults: timeout 2000 ms, JSON Content-Type, ожидаемый status 200. Это не факты об ESP32; review показывает их, пользователь сверяет с контрактом оборудования. Неизвестный device-specific body/path/status не помечается «подтверждён устройством».

Observe необязателен: «Устройство не предоставляет чтение состояния» — допустимый выбор. В таком случае review честно сообщает «После команды будет только ожидаемое состояние». Отсутствующее Observe не блокирует Apply корректного command mapping.

На review галочки имеют подписи «Настройки проверены», а не «Действие работает». ON/OFF не вызываются для проверки wizard. Даже GET состояния выполняется только по отдельному действию пользователя после Apply через active mapping.

Если единый connectivity proposal не укладывается в существующий output/context budget, задача делится на короткие physical proposals: Connection → ON → OFF → Observe, с компактными карточками принятия внутри той же беседы и одним финальным review. Partial proposal после malformed output не принимается; параметры модели не увеличиваются ради UX.

### 4.5. D — недостаточно данных

Пользователь: «Подключи насос к контроллеру». Ответ: «Для Насоса в Зоне упаковки нужен адрес контроллера и HTTP команды. Какой адрес использовать?» Рядом карточка с известными данными и пустыми пунктами «Включение», «Выключение», «Чтение состояния».

После адреса следующий вопрос касается method/path/payload. Неизвестные поля остаются незаполненными. При двух насосах сначала показывается выбор с зоной. Ответ «как обычно» не создаёт `/on`, GPIO, пароль или ESP32 contract.

Semantic clarification использует существующие `/setup/message`, `questions` и `unresolved_text`. Connectivity follow-up собирается coordinator из исходной просьбы и явных ответов, затем повторно отправляется целиком через `/admin/copilot`. Его rejection приходит как `ok:false`, `errors` и иногда `draft.summary`; transport adapter не теряет эту информацию из-за HTTP 422.

LLM summary не исполняется как instructions UI. Можно выбрать «Заполнить вручную»: известные проверенные поля переносятся в wizard, неизвестные остаются пустыми. После ручного редактирования старый AI proposal считается устаревшим.

### 4.6. E — validation error

Пример: поле ответа `on` выбрано для числового «Давление», либо сохранён невалидный URL другого draft Connection.

Экран остаётся в той же задаче. Ошибка помещается у соответствующего поля и в коротком summary над sticky footer: «ESP32 Резерв: не указан адрес. Изменения не применены». Ссылка «Перейти к подключению» ведёт к конкретному объекту; если ошибка вне текущей задачи — это явно написано. Уже введённые значения не очищаются.

Нельзя автоматически удалить чужую незавершённую сущность, исправить тип, отключить SafetyGate или применить только валидный фрагмент. Для full review нужно исправить либо явно исключить незавершённые изменения из плана. Отдельный TCP probe выбранного исправного Connection при этом разрешён.

Malformed AI output: «AI не подготовил проверяемое предложение. План этого запроса не принят». Кнопки «Повторить просьбу» и «Заполнить вручную»; нет карточки с частичным успехом. `message_incomplete` требует предусмотренного сервером полного повторения сообщения либо отмены, ручной patch не снимает этот флаг.

CAS/stale proposal: «Конфигурация изменилась в другой вкладке. Сравните обновлённую версию». Локальный ввод сохраняется; автоматического merge/повторного Apply нет. После разрешения конфликта нужен новый review.

### 4.7. F — изменение существующего устройства

Пользователь: «Переименуй Вентилятор в зоне упаковки в Вытяжку». Proposal показывает «Вентилятор → Вытяжка», изменения речевых aliases и сохранение Connection/actions; stable ID не показывается в Simple. Принять → общий review → Apply.

Изменение адреса общего контроллера показывает **все** затронутые устройства. По умолчанию предлагается явный выбор: «Изменить общее подключение для 3 устройств» или «Создать отдельное подключение для Вытяжки». Вторая операция создаёт Connection и явно переназначает mappings выбранного target в candidate; фонового перепривязывания нет.

Изменение capabilities и connectivity делится на отдельные proposals. Удаление action/state, на которые ссылается binding/observation, не проходит full validation. Если семантическое изменение невозможно без очистки ссылок, сначала отдельный подтверждаемый physical step, затем semantic step, затем новое physical предложение; все последствия входят в итоговый review. Существующий decoder запрет не отключается.

Удаление устройства показывает удаляемые mappings и возможные exclusive actions. Общий Connection не удаляется автоматически. После изменения физического mapping возможен сброс state evidence; это видно в review. Ограничения защищённой входной двери и legacy targets остаются в силе; Copilot отказ не превращается в предложение «обойти» защиту ручным JSON.

### 4.8. Голос как основной способ ввода

В composer крупная кнопка записи и равноправное текстовое поле. Состояния: idle → permission request → recording с таймером → transcribing → **редактируемый transcript** → явная отправка. Распознанная речь не отправляется автоматически и не считается подтверждением Apply. Особенно явно проверяются IP, path и JSON scalars.

Сохраняются существующие WAV/local STT, лимит записи setup 60 s, вне setup 15 s, Admin admission 4000 code points / 16000 UTF-8 bytes и отдельный production limit 192 UTF-8 bytes. При segmented STT показывается просьба проверить стыки. Raw audio не сохраняется; при уходе/отмене tracks закрываются, late response игнорируется. Apply недоступен во время записи/распознавания.

Browser microphone требует secure context: localhost подходит, plain `http://192.168.x.x` обычно не предоставляет `getUserMedia`. Это ограничение платформы браузера, а не STT Firefly. Текущая политика [M18.4](m18-admin-network-security.md) разрешает только loopback/SSH tunnel; прямой LAN HTTP Admin запрещён. Проверка secure context в UI остаётся защитным fallback, а не поддержкой LAN deployment. См. [MDN getUserMedia](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getUserMedia) и текущий [voice setup guide](dialogue-setup.md).

Baseline V2: голосовая настройка в браузере через localhost/существующий локальный tunnel. Удобный голос с телефона по LAN потребует отдельного решения по trusted HTTPS; нынешний origin guard ожидает HTTP, поэтому простой reverse proxy без анализа origin не является готовым решением. Native «Амина» остаётся рабочим command path, не становится незаметно каналом Admin setup. TTS и голосовое подтверждение Apply в этот milestone не входят.

## 5. Manual UX, review и состояние

### 5.1. Карточки и editors

Карточка показывает имя, зону, 1–2 главных свойства, evidence и readiness подключения. Редактирование открывается отдельно; поля IDs/speech internals/assignments не занимают карточку. Поиск и zone filter работают локально по загруженному catalog.

Manual task: название → необязательная зона → явные возможности → review. Шаблон предлагает редактируемую схему, но название «Телевизор» не разрешает автоматически громкость/каналы. Доступны boolean, числа с границами/единицами, enum и текст; это не только ON/OFF frontend. «Как обращаться к устройству» — понятные речевые имена и примеры; технические mentions/locations остаются в Advanced.

Незавершённые поля живут в `TaskBuffer`, а не в production config и не немедленно в общем server draft. В configuration candidate добавляется целиком заполненный task. Поэтому второе нажатие «Создать подключение» не создаёт скрытого invalid объекта в остальном плане.

### 5.2. Connection wizard

Wizard — отдельный экран или полноразмерная панель на небольшом дисплее, с одной scroll area, заголовком target, step indicator и sticky footer `[Отмена] [Назад] [Далее]`. Ошибка/результат отображаются непосредственно в текущем шаге; фокус переходит к первой ошибке.

| Шаг | Поля и действия | Когда можно продолжить |
| --- | --- | --- |
| 1. Способ подключения | HTTP / REST; существующее подключение или новое. MQTT, Serial/RS-485, CAN отмечены «Запланировано» и не выбираются как работающие | Выбран доступный HTTP transport |
| 2. Адрес | Имя, `http://IPv4[:port]`, `[Проверить доступность]`; при existing connection показаны consumers | Endpoint валиден; неуспешный TCP test допускает настройку офлайн, но результат/непроверенность видны в review |
| 3. Действия | Строки реальных capabilities, например Включить и Выключить; method, path, typed key/value body, ожидаемый status | Все выбранные mappings валидны. Пропущенное действие явно отмечено «Не подключать сейчас» |
| 4. Чтение состояния | «Есть / не предоставляется»; GET path; поле ответа → существующее свойство с типом | Нет coercion, JSONPath, nested body или auto state schema creation |
| 5. Review | Endpoint, actions, Observe, consumers, warnings, «Оборудование ещё не проверено командами» | `[Отмена] [Сохранить и применить]` открывает общий финальный review |

Для ON/OFF Simple показывает `on: Да/Нет`; wire value остаётся boolean, не строки `"true"`/`"false"`. Точный JSON доступен раскрытием. Технический ключ внешнего API `on` нельзя скрыть так, чтобы человек не смог проверить contract. Binding IDs и JSON всей configuration не нужны.

Существующие методы POST/PUT/PATCH/DELETE и body-free GET сохраняются; wizard не ограничивает backend только POST. GET для действия выбирается осознанно и не выдаётся за безопасную диагностическую проверку. Legacy URL bindings открываются без миграции; преобразование в shared Connection — отдельное видимое изменение host/path, включённое в diff.

### 5.3. Единая процедура публикации

```text
Local task buffer
  → assemble complete candidate
  → full validate + diff against stored AND active where they differ
  → human review of ALL changes + state reconciliation
  → explicit final confirmation
  → CAS Save → Apply → verify active identity
  → Applied / Conflict / Saved but not active / Recovery required
```

В Simple нет отдельного штатного Save. «Сохранить и применить» запускает проверку и **открывает review**, не пишет ConfigStore. Review показывает всё, что будет опубликовано: текущую задачу, ранее принятые изменения и shared consumers. Финальная кнопка — «Подтвердить применение». Изменение любого поля после review его инвалидирует.

Во frontend хранятся reviewed candidate snapshot, task version, expected stored identity, active identity и setup version. Перед публикацией проверяется, что они актуальны; backend CAS/setup checks остаются последним барьером. Нельзя применять новый active hash, незаметно полученный после review, как будто человек рассмотрел эту версию.

Для обычного manual task: `/config/validate` → `/config/diff` → review → `PUT /config` → `/config/apply`. Apply использует identity **из ответа Save**, а не старую base revision. Для активной setup session: синхронизация completed draft → validation/diff → review → `/setup/apply`; прямые Save/Apply запрещены существующим API. Setup Apply сохраняет собственные restore/preflight/CAS/rollback semantics.

Если структура валидна, но `apply_ready:false`, интерфейс не пишет «готово к применению». В setup окончательная production admission выполняется после восстановления рабочего контекста; этот этап отображается отдельно. Ошибка не приводит к автоматическому увеличению контекста/смене модели.

Manual Save и Apply — два существующих вызова, не общая транзакция. Если Save прошёл, а Apply нет: «Версия сохранена, но не активирована» + текущий active status. После неизвестного сетевого исхода UI читает stored/active/setup status; не повторяет mutation вслепую. В setup возможны `apply_failed_restored` и `restart_required` — это разные результаты, их нельзя заменить одним toast «ошибка».

### 5.4. Dirty draft, Cancel и продолжение

Один общий configuration candidate, одна активная редактируемая задача. Несколько независимых overlapping editors не публикуют свои полные snapshots поверх друг друга. Внутренние формы могут быть неполными только в local task buffer.

`Отмена` в editor удаляет изменения **этой задачи** относительно её checkpoint; предыдущие принятые proposals остаются. `Назад` из review возвращает в задачу без потери ввода. Закрытие dirty task предлагает «Продолжить редактирование / Отбросить изменения задачи»; это не скрытый Save.

В активной AI setup появляется отдельное `[Продолжить позже]`: сохраняется только уже валидный server draft, затем используется существующий setup cancel/restore для возврата ассистента. Надпись после завершения: «План сохранён, не применён». На startup только предложение resume, без автоматического Apply или загрузки Admin модели. Неполные local формы после закрытия вкладки не считаются server-saved; UI явно сообщает об этом.

В обычном manual режиме local buffer остаётся в памяти вкладки; уход внутри SPA не теряет задачу, reload предупреждает о потере. Advanced может отдельно сохранить валидную configuration без Apply с явным пояснением. Не вводить молчаливое browser localStorage сохранение всей configuration или audio.

При другой setup owner вкладке — read-only view, понятный статус, без «захвата» сессии. Token остаётся session-scoped; heartbeat и owner/version проверяются сервером. После reload история сообщений может быть неполной: существующий setup/status отдаёт snapshot, но не полный transcript. V2 восстанавливает план и требует review, не обещает восстановить весь чат.

### 5.5. State presentation

| Backend evidence / событие | Текст в Simple | Что нельзя утверждать |
| --- | --- | --- |
| Unknown | «Состояние неизвестно» | Начальное schema/Mock value не является physical fact |
| Predicted после accepted HTTP command | «Ожидается: включено. Команда принята, состояние не прочитано» | HTTP 200 не означает Confirmed |
| Confirmed после отдельного GET | «Включено · подтверждено устройством в 12:41» | Это свидетельство на указанное время, не непрерывный мониторинг |
| Stale property | «Последнее значение: включено · данные устарели» | Нельзя скрыть stale общим зелёным badge |
| Timeout/error команды | «Результат неизвестен / запрос завершился ошибкой» по фактическому error | Timeout POST не доказывает, что оборудование не сработало |
| Observe failure | Ошибка рядом с «Прочитать состояние»; старое evidence и его время сохраняются | Не заменять последнее значение на OFF и не подтверждать Predicted |
| Contradictory successful observation | «Устройство сообщает: выключено» | Нельзя сохранять желаемое ON вопреки readback |
| Mapping изменён / процесс перезапущен | Unknown согласно DeviceStateStore | UI не восстанавливает прежний Confirmed из browser cache |

Knowledge отображается по свойствам: при разных источниках у нескольких полей нельзя ставить устройству общий «Confirmed». Transport reachability и completeness независимы. Aggregate Connection «успешный обмен» можно получить из evidence consumers, но не объявлять все устройства доступными после успеха одного.

Обновление `/devices/state` лишь читает память сервера. Это не `/devices/poll`, который делает физический GET. V2 допускает умеренное обновление status/state на видимой странице, с остановкой в скрытой вкладке и backoff при ошибке, но не добавляет автоматический hardware polling, heartbeat оборудования или retry команд.

### 5.6. Кнопки действий в Device Detail

AI setup composer никогда не вызывает execute. Device action — отдельное явное действие человека в operational view после выхода из setup. Для переключателей показываются две кнопки ON/OFF; UI не делает optimistic Confirmed. Для параметров показываются schema bounds и единицы.

Сейчас существует text endpoint `/api/test-command`, но нет target-pinned button API. Нельзя просто отправить LLM строку «включи свет» и считать, что нажата конкретная карточка. Для полноценной V2 предлагается узкий guarded command API (раздел 9): выбранный target/action/arguments и active identity должны совпасть с результатом существующего безопасного command pipeline **до** hardware execution.

До такого API кнопка может только открыть Command Playground с review текста; это переходный fallback, а не завершённая реализация direct buttons. Защищённая входная дверь требует явной команды пользователя по существующим правилам; одно нажатие на иконку не синтезирует намерение открытия.

## 6. Simple и Advanced

| Область | Simple | Advanced |
| --- | --- | --- |
| Система | Название, готовность, AI setup, заметный статус настройки | Active/stored revisions, hashes, generation IDs, runtime context |
| Зоны / устройства | Человекочитаемые имена, необязательная зона, действия и понятные bounds | Stable IDs read-only для сохранённых сущностей, metadata, typed schema |
| Язык | «Как обращаться», reviewed aliases и примеры команд | mentions/locations, LanguageIndex conflicts и matching evidence |
| Connection | Имя, endpoint, существующее/новое, consumers | Base path, enable flag, timeouts, supported non-secret headers |
| Mapping | Действие → method/path и typed body; Observe field → state | Точный JSON, argument mapping, expected status details, legacy URL |
| Review | Before/after, affected devices, неприменённый план, одна процедура Apply | Raw diff, state reconciliation, revision/CAS details, отдельный Save |
| State | Значение, predicted/confirmed словами, свежесть, последнее событие | Evidence metadata, sequence/generation, source, executor errors |
| Диагностика | Короткая причина + «Подробнее» | Traces, raw model output отдельно от final action, health/build details |
| Playground | Не занимает главный экран | Preview, Mock Execute и явно выделенный Production Execute |

Advanced расширяет видимость, а не полномочия: validation, same-origin guards, запреты credentials и SafetyGate одинаковы. В Simple доступны необходимые transport facts, а не только декоративная зелёная иконка. Timeout и headers не обязательны для стандартного HTTP wizard, но доступны через «Дополнительно» без потери заполненного task.

## 7. Vue architecture

### 7.1. Почему этот стек

Vue 3 SFC и Composition API дают отдельные компоненты задач, формы с локальным состоянием и управляемый lifecycle записи/запросов. TypeScript нужен прежде всего для различения config DTO, proposal, task state и runtime evidence. Проверки на сервере он не заменяет. Vite собирает локальные static assets; разработческий server не участвует в production.

Vue официально поддерживает TS/SFC; Vite сам по себе не выполняет type checking, поэтому сборка должна отдельно запускать `vue-tsc --noEmit`. См. [Vue: TypeScript overview](https://vuejs.org/guide/typescript/overview.html). Конкретные версии инструментов фиксируются lockfile в B1, после разрешения на implementation, а не выбираются или устанавливаются в этом design turn.

Предлагаемый минимум: Vue, Vue Router; stores сначала на typed reactive modules/composables с едиными methods. Pinia возможна при явной потребности, не обязательна. Не нужны Nuxt/SSR, BFF, большой UI kit, cloud SDK или клиентский LLM. Тестовый runner/component tools — только dev dependencies будущего milestone.

### 7.2. Структура будущего проекта

```text
src/admin/frontend/
  package.json, package-lock.json, vite.config.ts, tsconfig*.json
  index.html
  src/
    main.ts, App.vue
    router/index.ts
    views/
      OverviewView.vue, AiSetupView.vue, ZonesView.vue, ZoneDetailView.vue
      DevicesView.vue, DeviceDetailView.vue, ConnectionsView.vue
      ConnectionDetailView.vue, StatesView.vue, AdvancedView.vue
      RevisionsView.vue, PlaygroundView.vue, DiagnosticsView.vue
    components/
      shell/        # navigation, setup banner, pending task indicator
      setup/        # composer, voice recorder, proposal card, questions
      devices/      # card, capability form, physical state evidence
      connection/   # endpoint/action/observe steps, probe result
      review/       # readable diff, shared consumers, final confirmation
      forms/        # scalar controls, field errors, accessible dialogs
    stores/
      configuration.ts, setup.ts, tasks.ts, deviceState.ts, ui.ts
    services/
      setupConversation.ts, reviewAndApply.ts, commandIntent.ts
    api/
      client.ts, config.ts, setup.ts, copilot.ts, connectivity.ts, runtime.ts
    types/
      wire.ts, configuration.ts, proposals.ts, tasks.ts, runtime.ts
    adapters/
      catalog.ts, diff.ts, errors.ts, state.ts, proposals.ts
    composables/
      useVoiceInput.ts, usePendingTask.ts, useRuntimeStatus.ts
    styles/tokens.css, base.css
  tests/unit/, component/, e2e/, fixtures/
  dist/             # build artifact, не редактируется вручную
```

`wire.ts` сохраняет `rooms/room_id`; view types используют `ZoneView` и `locationId`. Adapter не должен пересобирать configuration только из видимых полей и терять compatibility/retired IDs/редкие supported fields. Payload проверяется на boundary; unknown protocol shape вызывает явную ошибку совместимости.

Stores разделены по источнику истины:

- `configuration`: immutable copies stored/active + expected revision/hash; не ссылка на редактируемую форму.
- `tasks`: local incomplete buffer, complete candidate, checkpoint, reviewed snapshot и invalidation.
- `setup`: server-owned session/version/status, owner token, pending proposals и explicit review markers.
- `deviceState`: production evidence отдельно от playground; update только из server replies.
- `ui`: route/filter/mode, локальные busy/error states; не содержит правил разрешения hardware.

ReviewAndApply — один coordinator для AI и manual flow с двумя существующими publication adapters (setup и обычный). Components не вызывают `PUT /config`, Apply или production execution непосредственно. Сетевые side effects не запускаются из watchers/computed при изменении form value.

Параллельно допустимы read-only status запросы; draft writes, proposal acceptance и Apply последовательны. Request version/attempt ID защищают от поздних ответов. `AbortController` отменяет ожидание браузера, но не означает отмену backend inference/Apply; после обрыва нужна сверка серверного состояния.

`setupConversation` выбирает capability-specific lane: semantic dialogue, полный Copilot или manual task. Для connectivity действует явный phase/target. При неоднозначном intent сначала уточнение. Это routing существующих Admin инструментов, не новый агент с полномочиями execute.

### 7.3. UI interaction baseline

Keyboard navigation, видимый focus, labels и aria-live для результатов, возврат focus после dialog, понятная отмена. Ошибки различаются текстом/иконкой, не только цветом. Целевые viewport для acceptance: 1280×800, 800×600 и 390×844; отсутствие горизонтального scroll у основных task screens. На длинном review header/footer доступны, details раскрываются по месту.

## 8. Offline build и deployment

```text
Vue/TS sources + locked build dependencies
  → typecheck + frontend tests + Vite production build
  → dist/index.html + hashed JS/CSS + explicit asset manifest
  → CMake copies/installs verified artifact
  → existing C++ HttpServer serves local assets + /api/*
  → browser, без Node runtime на Firefly
```

`vite build` предназначен для выпуска static production bundle: [Vite production build](https://vite.dev/guide/build.html). Vite dev/preview server не запускается на deployed Firefly. Сборка может происходить на подготовленной build машине; на target передаются C++ binary и dist из одного release.

Предлагаются два **будущих** CMake режима: явный frontend build target при наличии Node toolchain либо `FIREFLY_ADMIN_DIST_DIR` для готового проверенного artifact. Обычная core-only сборка `FIREFLY_BUILD_ASSISTANT=OFF` не должна требовать npm download. CMake не запускает `npm install` автоматически. При отсутствии нужного dist — понятная build/package error, не загрузка с CDN и не тихая подмена старым UI.

Offline runtime не равен сборке с пустым npm cache без сети. Первичная подготовка locked dependencies — отдельный разрешённый build-time шаг. Для offline rebuild нужны заранее подготовленный package cache/toolchain или готовый dist. В production package нет `node_modules`, npm, dev server, model downloads.

Необходимая малая правка transport: заменить фиксированный список семи asset names на **build-generated allowlist** dist files с MIME types. Сервер продолжает обслуживать только явно перечисленные assets; никаких произвольных filesystem paths, directory listing или SPA fallback для `/api/*`. Manifest создаёт packaging step, с перечнем URL/file/digest и проверкой отсутствия traversal/symlinks за пределами dist. Это не Vite manifest, который runtime должен интерпретировать как приложение.

Hash router избегает server history fallback: fragment не отправляется HttpServer; refresh `/#/devices/id` запрашивает `/`. См. [Vue Router history modes](https://router.vuejs.org/guide/essentials/history-mode.html). Не использовать cache-busting query strings: текущий HttpServer отвергает `?` в request path. Hashed filenames решают версионирование assets.

Сохраняются self-only CSP, `nosniff`, mutation headers и текущий no-store. Vue templates компилируются на build, runtime template compilation/eval и inline scripts не нужны. Все fonts/icons локальные; CSS без external imports. Dynamic chunks включаются в asset manifest. Audio helpers/worklets, если появятся, также локальные. Service worker/PWA cache не вводится: он усложнит stale UI/API recovery.

Release устанавливается как согласованный набор binary + assets; переключение версии только целиком. Проверить недостающий chunk, неверный MIME и несовместимый API до приёмки. UI assets не должны смешиваться между releases. Build metadata дополняется frontend artifact identity, чтобы диагностика показывала реально загруженную сборку.

## 9. Backend/API impact

### 9.1. Endpoint map

«Adapter» ниже означает TypeScript представление/координацию, без изменения C++ endpoint, если отдельно не указано иное.

| Existing endpoint | Решение | Как используется / ограничение |
| --- | --- | --- |
| `GET /api/status`, `/api/dashboard` | Adapter | Health, setup/voice readiness, active/stored mismatch; не выдавать агрегат `executor:mock` за отсутствие production HTTP |
| `GET /api/config`, `/api/config/active` | Как есть + adapter | Отдельные stored и active snapshots; candidate не подменяет runtime state |
| `POST /api/config/validate` | Как есть | Полный catalog/domain/context admission, language conflicts, preflight; не делать из него per-field probe |
| `POST /api/config/diff` | Adapter | `base:stored/active`; readable diff + state reconciliation. Агрегат connectivity раскладывается по stable connection ID, `(target_id,action_id)` и observation target |
| `PUT /api/config` | Как есть | Полная validation и CAS; Simple вызывает только после final confirmation |
| `POST /api/config/apply` | Как есть | Применяет сохранённую identity; после сбоя сверить stored/active, не обещать атомарность двух REST calls |
| `GET /api/setup/status` | Adapter | Server snapshot, version, questions/progress, ownership view; не содержит полного transcript |
| `POST /api/setup/start`, `/resume`, `/heartbeat`, `/cancel` | Как есть + adapter | Explicit entry, setup ownership, timeout/restore; `cancel_pending` требует ожидания статуса |
| `POST /api/setup/message` | Как есть для semantic lane | `session_id/version/request_id/text`; incomplete-message recovery; не использовать для HTTP/JSON/IP |
| `POST /api/setup/draft` | Как есть | Только полный валидный snapshot, owner/version/base checks; общий мост manual и Copilot в setup |
| `POST /api/setup/apply` | Как есть | Единственный publication path активной setup session; V2 всегда ставит review перед вызовом |
| `POST /api/admin/copilot` | Adapter | Connectivity и поддерживаемые полные structured operations; request `{text,configuration}` + session ownership при setup; ответ использует `draft`, а не `suggestion` |
| `POST /api/admin/copilot/accept` | Как есть + adapter | Hash-bound acceptance возвращает candidate; после этого `/setup/draft`, пока версия не изменилась; не Save/Apply |
| `POST /api/admin/transcribe` | Как есть | WAV, `X-Firefly-Setup`, explicit send transcript; никакого browser cloud SpeechRecognition |
| `GET /api/admin/templates`; `POST /api/admin/id` | Как есть | Manual templates/IDs; `/id` сейчас только location/device/action, не Connection |
| `POST /api/admin/draft`, `/suggest`, `/plan`, `/action-draft`, `/language-profile` | Adapter | Сохраняют поддерживаемые manual/advanced flows; AI calls требуют setup lifecycle; предложения всегда review |
| `POST /api/connections/test` | Legacy как есть; **новый scoped endpoint** | Существующий full-config probe не решает независимый test invalid draft; см. 9.2 |
| `GET /api/devices/state`, `/api/executions` | Adapter | Physical evidence, history, active identity; playground отдельным блоком; чтение не вызывает hardware |
| `POST /api/devices/poll` | Как есть | `{target_id,expected_hash}` из отображённого active snapshot; один явный GET, конфликт требует обновления view |
| `POST /api/devices/observe` | Удалён в [M18.3](m18-production-api-hardening.md), 404 | Public simulated observation запрещён; Confirmed: `POST /api/devices/poll` → configured device GET |
| `POST /api/test-command` | Как есть для Playground | Preview/Mock/явный Production; для target-pinned buttons нужен узкий guarded API, см. 9.3 |
| `GET /api/config/export`, `/revisions`; `POST /api/config/import`, `/revision`, `/restore-candidate`, `/migrate` | Как есть + adapter | Import/restore в candidate, обязательный review; ничего не применяется при выборе файла/версии |
| `POST /api/config/restore`, `/reconcile` | Advanced recovery | Сохранять различие stored и active; не вызывать автоматически ради закрытия баннера |

Для Connection UI может сохранять нынешнюю deterministic allocation схему `connection_http_N` с учётом occupied/retired IDs; отдельный API ID allocator не обязателен. Backend uniqueness/category checks остаются authoritative.

### 9.2. Обязательная UX-правка: isolated connection probe

Предлагается **новый** `POST /api/connections/probe` с payload `{connection: <один Connection>}`. Существующий `/api/connections/test` остаётся совместимым и после удаления vanilla UI: backend API сохраняется для диагностики и regression tests. Probe принимает отредактированный объект до Save, не требует полного configuration и не принимает URL из production voice command.

Server contract:

1. Строгий bounded JSON decode только одного Connection: известные поля, ID/name/types/size, transport contract. Reuse существующих C++ validation primitives; не новый permissive URL parser.
2. Только поддерживаемый включённый HTTP Connection, numeric IPv4, timeout 50–10000 ms. Сохраняются запреты credentials, DNS/HTTPS, authority-changing paths, redirects/proxies и control characters. Остальные транспорты отвечают «не поддерживается» без сетевого I/O.
3. Использовать существующий `HttpHardwareExecutor::testConnection`: connect/close, **ноль HTTP bytes**. Он уже проверяет transport contract одного Connection через `validateConnections` на минимальном объекте. Для endpoint дополнительно нужен строгий decode структуры/identity, не всего catalog.
4. Никакой записи draft/config/DeviceStateStore, runtime generation или production action. Сохранить same-origin/header guard и setup-owner authorization при активной setup session.
5. Ответ сообщает `scope:tcp_connect_only`, результат и диагностический code/path. UI связывает ответ с точным snapshot endpoint; после изменения host/port/timeout result помечается устаревшим. Elapsed time допустим как новое диагностическое поле, но не обязателен для semantics.

Здесь отсутствие cross-entity validation безопасно: probe не использует bindings/state/actions и не публикует catalog. Connection uniqueness/references всё равно проверяются при полном Apply. Нельзя реализовать обход на клиенте, удалив невалидные connections из полного draft и выдав результат за проверку всей конфигурации.

Regression: A валиден, B имеет пустой base URL. Probe A делает TCP connect; Probe B отклонён до network. Full validate/Save/Apply такого общего candidate отклонены. Snapshot stored/active, physical evidence и число HTTP requests не меняются.

### 9.3. Узкие дополнительные изменения и их приоритет

| Изменение | Приоритет / обоснование | Граница |
| --- | --- | --- |
| Generated static asset allowlist + CMake dist packaging | Обязательно B1: Vite hashed files отсутствуют в текущем allowlist | Только assets/build integration |
| `/api/connections/probe` | Обязательно B2/B3: устраняет подтверждённую UX-проблему | Scoped validation не ослабляет full validation |
| Structured setup errors | Небольшое рекомендуемое B2 изменение: сейчас setup exceptions превращаются в `409 setup_failed` со строкой причины | Сохранить совместимый envelope; добавить stable reason/code, field path где известен и retryability. Не парсить английские exception строки по всему Vue |
| Explicit no-zone в Admin create operation | B3 для универсального AI setup отдельного оборудования | Явное nullable location, одинаковая strict validation; не расширять production parser |
| Guarded Device command endpoint | Требуется для завершённых action buttons, отдельное согласование B2 | Не прямой вызов executor по произвольному target/action и не изменение LLM inference |

Предлагаемый guarded endpoint: `POST /api/devices/command` с `text`, `expected_hash` и `expected_action:{target_id,action_id,arguments}`. Backend берёт одну active generation, проверяет hash и проводит текст через прежний безопасный pipeline. До execution результат должен **точно** совпасть с ожидаемыми target/action/arguments. Для кнопок допускается только однозначный deterministic/LanguageIndex match; иначе отказ и открытие ручного command input. Expected action — дополнительное ограничение, не разрешение выполнить действие в обход SafetyGate.

Это требует узкого guard в существующем command admission/execution seam, включая atomic проверку относительно generation, а не только JS сравнения после POST. Нельзя сделать Preview одним запросом и позже неконтролируемый Execute другим: между ними может измениться generation или результат интерпретации. Нельзя сначала выполнить команду, а потом обнаружить другой target. Нет автоматических retries, новых LLM calls, prompt/config/model changes или отдельной execution architecture. Если владелец не принимает этот scope, direct buttons остаются за пределами V2 acceptance; доступен явный Playground fallback.

UI строит для generic buttons только проверяемую фразу из уже принятых aliases/аргументов и показывает её в command review. При невозможности составить однозначную фразу не добавляет aliases автоматически. Защищённая дверь остаётся text-only explicit intent; guards текущего SafetyGate действуют на полный исходный command input.

Для AI-first flows нового chat backend не требуется: coordinator использует `/setup/*`, `/admin/copilot`, `/accept` и `/setup/draft`. Semantic acceptance и physical acceptance разделены. Resume требует review текущего snapshot, поэтому durable transcript endpoint не обязателен. Если будет нужен полный conversation audit, это отдельное требование к хранению/retention.

Конкретная последовательность объединённой задачи: `/setup/start` → `/setup/message` → human semantic review → `/admin/copilot` на последнем server snapshot → human physical review → `/admin/copilot/accept` → `/setup/draft` с текущей session version → full review → `/setup/apply`. После каждого изменяющего draft ответа coordinator принимает новую server version. При semantic уточнениях используются последующие `/setup/message`; при connectivity уточнениях — новый `/admin/copilot` с явно собранными фактами. Вторая запись не выполняется поверх изменившегося checkpoint. Если человек явно применил семантику раньше, перед следующим AI шагом нужна новая `/setup/start`, хотя visual conversation продолжается.

Admin-only универсальная формулировка prompt и metadata default предлагаются как небольшой отдельный B3 change с non-home regression. Domestic prompt v5, model exports/parameters, STT/VAD и production LanguageIndex/SafetyGate не трогаются. Существующий Copilot inventory limit (в `scopedInventory` 900 bytes до добавления physical context) и output budget 384 tokens могут ограничить реальные HTTP proposals. UI не должен обещать, что выбор одного target уже уменьшает отправленный model context: сейчас API получает полный catalog. Scoping/rebudgeting не включать автоматически; если acceptance не проходит, фиксировать ограничение и согласовывать отдельную Admin задачу.

Не нужны atomic-save-and-apply rewrite, websocket server, BFF/Node service, новое хранилище states, смена protocol или endpoint для draft hardware execution. Diff можно адаптировать на клиенте без нового C++ формата; per-field validation не заменяет final full validation.

## 10. Migration strategy

Предлагаются четыре законченных этапа с чёткими gates. До B3 production default остаётся vanilla; V2 собирается отдельным целым artifact, а не встраивается островками в глобальный `admin.js`. Одновременно изменять одну configuration из двух UI нельзя без обычных setup ownership/CAS конфликтов.

| Этап | Результат | Gate |
| --- | --- | --- |
| **B1 — Vue foundation и delivery** | Vue shell/routes, typed API client, read-only overview/zones/devices/states, stores, asset allowlist/CMake packaging, offline checks | UI работает через настоящий C++ HttpServer, deep links refresh, нет внешних запросов/Node runtime; корректный degraded status |
| **B2 — законченный manual parity** | Local tasks, generic typed capabilities, manual connection wizard, isolated probe, review/apply/recovery, revisions/import/export, Playground, baseline voice input; guarded device controls при согласовании | Полный manual HTTP mock path, Cancel/stale/CAS/partial-save tests, старые поддерживаемые функции доступны; рабочий backend semantics сохранён |
| **B3 — AI-first setup** | Единая беседа, semantic→physical lane, proposals/follow-up, voice transcript review, resume/ownership; explicit no-zone и согласованный Admin terminology cleanup | Сценарии A–F проходят через visible UI; отдельная live NPU/voice приёмка фиксирует accuracy/latency без подмены моделями |
| **B4 — переключение и удаление vanilla** | V2 default, удаление `src/admin/web` старого frontend и obsolete asset list; port relevant tests; обновлённые docs/package | Acceptance matrix закрыта; удаляется старый UI, не working REST endpoints; доступен rollback целым предыдущим release |

B2 и B3 можно вести как две части одного release candidate, но нельзя объявлять AI-first milestone завершённым после shell. Продолжительная shadow migration с двумя разными редакторами и бесконечным parity backlog не предлагается. В B4 сохраняется test coverage поведения, а не зависимость от старых global JS names/DOM IDs.

Rollback — возврат предыдущего совместимого binary/assets package с существующей schema v2. Он не откатывает конфигурацию автоматически; для неё остаётся reviewed restore flow. Нет обязательной backend name migration или conversion всех legacy URL bindings при первом открытии V2.

## 11. Testing strategy

### 11.1. Сохранить существующее доказанное поведение

| Текущие тесты | Что переносится / сохраняется |
| --- | --- |
| [AdminModelTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin11/AdminModelTests.js), [SetupWebTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin/SetupWebTests.js) | Scalar limits/precision, error text, diff/state formatting, Unicode count, voice state/encoding |
| [WebAdminTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin11/WebAdminTests.js), [TypedWebTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin11/TypedWebTests.js), [FinalUxWebTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin11/FinalUxWebTests.js), [OfflineWebTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin11/OfflineWebTests.js) | Forms, typed capabilities, validation and manual fallback; сверить актуальные fixtures/selectors, не переносить старый DOM контракт буквально |
| [SetupTests.cpp](../tests/admin/SetupTests.cpp), [SetupWebBrowserTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin/SetupWebBrowserTests.js) | Ownership, version/CAS, duplicate request ID, partial-message rollback, recovery, cancellation, assistant pause/restore, запись tracks |
| [AdminDraftTests.cpp](../tests/admin11/AdminDraftTests.cpp), [AdminCopilotTests.cpp](../tests/admin12/AdminCopilotTests.cpp), [AdminTranscribeTests.cpp](../tests/admin13/AdminTranscribeTests.cpp) | Strict structured decoding, forbidden operations, proposal fingerprint, transcription limits; independent of real NPU |
| [ConnectionAdminTests.py](../tests/admin/ConnectionAdminTests.py), [ConnectivityConfigTests.cpp](../tests/execution/ConnectivityConfigTests.cpp) | Draft→Save→Apply isolation, validation, graph references, shared connections, compatibility |
| [ConnectivityWebTests.js](https://github.com/siyovushdev/FireflyVoiceAssistant/blob/a1fd5be64261ceb14abfaab4b49f703e135c683e/tests/admin/ConnectivityWebTests.js), [ConnectivityHttpTests.py](../tests/execution/ConnectivityHttpTests.py) | Реальный Chromium/C++ HTTP/mock: semantic setup, TCP probe без HTTP bytes, ON/OFF payload, Observe, Preview/Cancel/Apply, Predicted→Confirmed, stale mapping/restart/errors |
| DeviceState/HardwareBinding/LanguageAdmin suites | Evidence lifecycle, transport failures, no HTTP after semantic rejection, unchanged SafetyGate/LanguageIndex |

Исторический [Connectivity report](device-connectivity-binding-admin.md) фиксирует 65/65 CTest и Chromium acceptance на 2026-10-02. В этой design работе сборка, CTest, NPU и hardware smoke **не запускались**; это не новое подтверждение 65/65. Browser tests регистрируются условно при Node/Chromium/DISPLAY: зелёный core-only CTest без этих suites не считается UX acceptance.

### 11.2. Unit и component

Unit tests проверяют содержательные invariants: DTO→view→candidate без потери полей; task checkpoint/cancel; proposal invalidation; whole-config diff и shared consumers; различение stored/active; review candidate неизменен; request ordering/late responses; precision/type boundaries; evidence не становится Confirmed оптимистически.

Component tests: wizard navigation с сохранением значений, inline errors/focus, optional Observe, typed booleans, partial bindings, stale probe marker, proposal accept/reject, recording/processing blocks Apply, mode switch не сбрасывает form. Проверять ожидаемое пользовательское поведение, не внутренние reactive implementation details.

### 11.3. API contracts и browser E2E

Контрактные fixtures должны поступать от C++ test server, включая `ok:false` при HTTP 200 где применимо, 409 ownership/CAS и 422 validation. Vue response adapters не предполагают один формат `suggestion` для всех AI endpoints. Проверки exact paths/types и сохранения mutation guards обязательны.

Browser E2E выполняется на **production Vite dist через C++ HttpServer**, с изолированным test store и localhost equipment mock. Текущий Chromium/CDP harness сохраняется как gate; дополнительный runner возможен позже, но скачивание нового браузера не требуется для первого перехода. Сценарии выполняются через видимые controls; нельзя для основного happy path напрямую присвоить `draft = candidate`, как допускает часть прежних tests.

Обязательные regressions:

1. Empty system → power-only semantic review → physical proposals → full review → Apply; до final confirmation stored/active не меняются, hardware HTTP count = 0.
2. Валидный Connection A + invalid unrelated B: probe A успешен, B rejected, full Apply blocked; errors ведут к B.
3. ON: ровно один POST с boolean true → Predicted; явное readback: ровно один GET → Confirmed. OFF симметричен. UI status refresh не вызывает GET оборудования.
4. Cancel на любом wizard step возвращает checkpoint, сохраняет ранее принятые задачи. Edit после review инвалидирует Apply/probe evidence.
5. Shared Connection edit перечисляет всех consumers; rename сохраняет identity/evidence, endpoint change показывает Unknown после Apply.
6. Другой tab/version/hash, stale poll/proposal, дважды нажатый Submit, потерянный ответ Save/Apply: нет повторной публикации или повторной физической команды.
7. Guarded Device command: mismatched target/action/arguments/hash и отказ SafetyGate дают ноль HTTP requests; нет LLM fallback исполнения от кнопки.
8. AI partial/malformed/unsupported output, mixed semantic+physical proposal и unknown endpoint data не создают исполняемого candidate. После ошибки не replay старый proposal/action.
9. Setup pause/restore, refresh/resume, disconnect/inactivity, `cancel_pending`, `message_incomplete`, failure after Save/compensation остаются корректными.
10. Microphone denied/missing/insecure context/STT unavailable/late transcription: текстовый путь доступен, tracks закрыты, Apply не вызывается записью.
11. Independent device без зоны, Цех/Склад/Стенд, typed numeric/enum/text device; UI не навязывает комнаты/дом и ON/OFF всему оборудованию.
12. Import/restore → review, legacy bindings, body-free GET, non-default status, headers/timeouts, parameter argument mappings не теряются в round trip.

### 11.4. Offline, target и AI acceptance

Проверка offline: чистый browser profile, external egress заблокирован, все routes и lazy chunks загружаются; network log содержит только Firefly origin, а equipment traffic идёт только от C++ и по явным действиям. Нет CDN/fonts/telemetry/cloud STT. Проверить MIME, CSP, отсутствующий asset, refresh hash routes и запуск package на target без Node.

На текущем Chromium вручную оценить размещение result/error рядом с действием, чтение длинного review, keyboard/focus, touchscreen и три viewport. Screenshot alone не доказывает работу controls; сохранять browser log, requests и snapshots.

Structured proposal tests подают фиксированные model outputs, включая 4 connectivity operations, missing facts, смешанный proposal, чужой action ID, unsupported transport и stale base. Отдельный live NPU acceptance на штатных моделях проверяет реальные формулировки A–F, non-home сценарии и IP/path/scalars после STT. Отчитываться раздельно о semantic accuracy, clarification/false reject, wrong proposal, validation rejection и latency по фазам; mock decoder test не подтверждает качество live Copilot.

NPU runs не совмещаются со сборкой/другим NPU owner. Не используется пользовательский production store или HIDDEN data; существующий frozen DEV не правится. При плохом распознавании IP пользователь исправляет transcript сам; автоматическая коррекция STT не добавляется.

## 12. Acceptance criteria UX V2

| ID | Проверяемый критерий |
| --- | --- |
| UX-01 | Нет обязательной Home/Room модели; проходят система без зон и минимум три non-home fixtures: цех, склад, лабораторный стенд |
| UX-02 | Empty state ведёт к AI setup; opening page не останавливает ассистент. Режим настройки, pause/restore и ownership видны на всех screens |
| UX-03 | A–F доступны через один conversation UI с review каждой proposal phase и отдельным final confirmation; ни AI, ни STT не вызывают hardware/Apply |
| UX-04 | Запрос «оба только включаются и выключаются» даёт ровно две actions на устройство; иных capabilities нет |
| UX-05 | HTTP wizard проходит в Simple без ввода IDs, hashes, revisions, binding internals; method/path/payload остаются проверяемыми человеком |
| UX-06 | Probe A не блокируется invalid B, отправляет ноль HTTP bytes; полный invalid candidate никогда не Apply |
| UX-07 | «Сохранить и применить» сначала открывает полный readable review; после изменения candidate нужно повторное подтверждение; чужие pending changes не публикуются скрыто |
| UX-08 | Predicted/Confirmed/Unknown/stale, transport reachability и configured status различаются; HTTP ACK не выдаётся за физическое подтверждение |
| UX-09 | Device Detail показывает actions и active state; при согласованном guarded API кнопки target-pinned, без bypass SafetyGate. До этого gate direct controls считается незакрытым |
| UX-10 | Cancel, reload, resume, conflict и error после Save не теряют подтверждённую конфигурацию и не запускают автоматический retry execution |
| UX-11 | Все основные manual flows и typed contracts доступны без AI; недоступность модели не превращает false state в success и не блокирует просмотр/редактирование |
| UX-12 | Ошибка/результат текущего шага видны без прокрутки к верху другой формы; keyboard focus, labels и mobile layout проходят Chromium acceptance |
| UX-13 | Static dist работает через C++ offline на Linux/aarch64 без Node runtime и внешних assets; failed asset/API совместимость даёт понятную ошибку |
| UX-14 | Существующее backend regression coverage сохраняется; новые contract/component/E2E gates проходят; live NPU/voice результаты представлены отдельно, без заявления об идеальной AI accuracy |
| UX-15 | После B4 старый vanilla frontend удалён, V2 покрывает согласованный parity checklist, rollback artifact проверен |

Для пользовательской приёмки владелец выполняет два задания без подсказок разработчика: создать оборудование в non-home зоне и подключить power-only device к mock через AI либо wizard. Проверяется, что он может объяснить до подтверждения, какие устройства изменятся, отправлялись ли команды и является ли показанное значение подтверждённым. Фиксируются ошибки, непонятные шаги и время; обещание «за N секунд» до измерения не вводится.

## 13. Explicit non-goals

- ESP32 firmware, autodiscovery устройств, новые MQTT/TCP/UDP/Serial/RS-485/CAN executors.
- Cloud AI/STT, CDN, telemetry, online accounts, runtime Node, BFF, SSR.
- Auth/roles/credential storage, новые TLS/DNS transport capabilities; удобный trusted HTTPS Admin deployment только отдельным решением.
- Backend architecture rewrite, массовое переименование rooms/room_id, новая schema/config store или изменение compact protocol.
- Изменение RKLLM/model exports/quantization/sampling, production prompt v5, command parser grammar, LanguageIndex semantics, SafetyGate, STT/VAD/wake runtime. Guarded button admission — только дополнительный запрет перед прежним исполнением.
- Hardware requests из proposal/validation/review/Save/Apply; draft action testing, автоматический hardware poll/retry или optimistic Confirmed.
- Незаметное смешение semantic и connectivity operations в одном Copilot proposal, transcript corrections, автоматическое выдумывание network contract.
- Неограниченная история чата/контекст, новый agent framework, TTS setup или voice-only Apply.
- Новые multi-system tenancy, equipment fleet management, scheduled automation editor и гарантии промышленной функциональной безопасности.

## 14. Risks и решения владельца

| Риск | Решение в предложенном baseline |
| --- | --- |
| UI обещает больше, чем поддерживает Admin inference | Ограниченные lanes, явные follow-up, короткие proposals, manual fallback, отдельная live приёмка; никаких новых model budgets без согласования |
| Полный Copilot context/output не вмещает HTTP contract | Измерить на реальном inventory; сначала несколько physical proposals в одном task. Если не помогает — отдельная Admin context задача, не скрытый backend rewrite |
| Generic Vue forms теряют редкие config поля | Wire-preserving adapters, round-trip fixtures и full C++ validation |
| Общий Connection затрагивает незаметные устройства | Consumer list в editor/review, выбор shared edit или отдельного Connection |
| Simple скрывает важный failure между Save и Apply | Внутренние stages остаются раздельными; пользователь видит «сохранено, не активно» и recovery |
| Новый UI случайно выполняет действия из chat | Раздельные services и operational views, отсутствие execute dependency у setup, network-count regressions |
| LAN browser voice не работает, хотя STT готов | Secure-context capability check, текстовый fallback; localhost/tunnel baseline, решение HTTPS отдельно |
| Migration растягивается в два параллельных продукта | Четыре gate, один release candidate, удаление vanilla после parity/acceptance |

**Пять вопросов для владельца продукта:**

1. Принимаем ли «Система» как основной русский термин, «Зона» как необязательную сущность и обязательный AI/manual сценарий отдельного устройства без зоны? Рекомендация: да; для AI согласовать узкое nullable-location расширение Admin operation.
2. Подтверждаем ли процесс: каждое предложение принимается **в план**, semantic и connectivity идут раздельно, а задача публикуется одним итоговым review/Apply? Рекомендация: да; отдельное Apply семантики остаётся явным выбором пользователя.
3. Достаточен ли для V2 voice baseline через localhost/локальный tunnel, или требуется голос с телефона по LAN в этом же milestone? Рекомендация: baseline; trusted HTTPS/origin/deployment — отдельно согласованный scope.
4. Включаем ли узкий guarded Device command API ради полноценных кнопок ON/OFF с проверкой target/action/hash до исполнения? Рекомендация: включить как отдельный небольшой safety-reviewed change; без него честно ограничить кнопки переходом к Playground и пересмотреть UX-09.
5. Утверждаем ли B1–B4, включая отдельную non-home/live Copilot приёмку и удаление vanilla после parity, а также только необходимые Admin изменения (structured errors, no-zone, универсальная Admin терминология) без model/prompt production tuning? Рекомендация: да; превышение текущих model budgets оформлять отдельной задачей.

После согласования этих решений следующим действием может стать подробная постановка B1. Этот отчёт не разрешает начинать implementation, менять зависимости или публиковать новый UI.
