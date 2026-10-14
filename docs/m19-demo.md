# M19: AI Setup → HTTP → voice → Confirmed

Latest isolated live acceptance **failed** on [Issue #17](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/17):
an unknown-room text request caused a mock POST through LLM fallback. Genuine
Qwen forms and Test Apply passed, but physical voice ON/OFF was not confirmed.
See [grounding remediation and production exposure](m19-grounding-safety.md).
The installed production release has not received this fix. Further live testing
requires independent review and fresh owner authorization for the new clean candidate.

The earlier missing `лампу` gate ([#16](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/16))
is documented in [the Russian profile report](m19-russian-device-profile.md).
Neither AI/Test Apply successes nor a scripted voice demonstration close live acceptance.

Этот сценарий использует существующие AI Setup, Connection/Binding/Observation,
CAS Apply, языковой индекс, SafetyGate и DeviceStateStore. Он не устанавливает
новый сервис, не меняет схему ConfigStore и не включает автоматическое чтение
состояния. Независимое подтверждение — только явная кнопка **Прочитать состояние**.

## Презентация на одном экране

Подготовьте стенд командой ниже. Откройте напечатанный адрес `/#/setup`.
Терминал нужен только для подготовки/завершения стенда. Во время показа не нужны
CLI-команды или редактирование JSON-конфигурации.

| Что показать / сказать | Что должно быть видно / произойти |
| --- | --- |
| **Начать настройку**. «Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать». | Предложение: Кабинет, Лампа, Питание (boolean), Включить/Выключить; язык зоны **в кабинете**, устройство **лампа / лампу**. В live-проверке обе формы должен самостоятельно предложить настоящий Qwen: **AI gate PASS**. Отсутствующая или неверная форма — **AI gate FAIL**, остановить AI-first приёмку. Ручная правка — отдельный fallback. |
| **Принять в план**. | Предложение принято в черновик. Активная система всё ещё пустая; команд устройству нет. |
| **Настроить подключение: Лампа → Настроить подключение вручную → HTTP / REST →**. Название «Лампа HTTP», Endpoint — напечатанный адрес mock (`http://127.0.0.1:8765` по умолчанию). **Проверить доступность → Далее**. | Только TCP connect, ноль HTTP-байтов. Это не проверка функциональности API. Пустой Endpoint вызывает понятную ошибку. |
| Отметить **Включить** и **Выключить**. Для обоих: Метод **POST**, Путь **/relay/1**, JSON поле **on**, тип **Да / Нет**, значение соответственно **Да** / **Нет**. **Далее**. | Контракт явный; IP, пути, методы и JSON-поля не сочиняются AI. Поле `on` предзаполняется как редактируемый пример; сверить его с API mock. |
| **Есть независимый GET**, Путь **/relay/1**, **Добавить поле Observe**, поле ответа **on**, свойство **Питание**. **Далее → Сохранить и применить**. | Один общий Review: семантика, оба POST, `on = Да / Нет`, независимый GET и `on → Питание`. До подтверждения ноль команд. |
| **Подтвердить применение → Назад к плану → Устройства → Лампа**. | «Изменения применены. Активная версия проверена». Физическое состояние **Неизвестно**. Сохранённая и активная версии совпадают. |
| На отдельно разрешённом live-стенде: «Амина, включи лампу в кабинете», затем «Амина, выключи лампу в кабинете». На offline-стенде — кнопки **Включить / Выключить**, честно обозначенные как ручное HTTP-управление. | Одна команда — один POST, тело `on: true` / `false`. После команды **Ожидается**, а не **Подтверждено**. **Обновить данные** обновляет только отображение; устройство не опрашивается автоматически. |
| После каждой команды **Прочитать состояние**. | Ровно один независимый GET; валидный boolean подтверждает значение. **Подтверждено** — результат ответа mock, не доказательство работы физической лампы. |

Дополнительный проверенный сценарий: «Добавь лампу без зоны, которую можно
включать и выключать»; grounded команды «Амина, включи лампу» / «Амина,
выключи лампу». Зона не создаётся, `room_id = null`. Для основной презентации
достаточно сценария Кабинет. В live-показе сначала проверить предложения
реального Qwen; scripted profile не доказывает его качество.

Если данных подключения нет, AI Setup задаёт вопрос об адресе/протоколе,
ON, OFF и Observe. Допустимо заполнить их пошагово в **Ваша просьба**; программа
детерминированно составляет предложение из этих фактов. Это другая полоса,
не генерация HTTP-контракта моделью. В автоматическом E2E предложение открывается
через **Изменить: Подключение** в существующем wizard, затем проходит тот же
общий Review.

## Подготовка private offline-стенда

После сборки из изолированного worktree:

```sh
python3 scripts/m19-private-demo.py --build-dir build-m19-integrated --admin-port 0 --mock-port 0
```

Скрипт запускает только `firefly-admin` и **test-only** `m19_test_worker`, Python
HTTP mock и новый временный ConfigStore с правами 0700. В основной команде оба порта выбираются автоматически; production mock на 8765
не используется. Если явно заданный порт занят, скрипт остановится, не трогая чужой
процесс. Для другой сборки можно явно указать её каталог:

```sh
python3 scripts/m19-private-demo.py --build-dir build --admin-port 0 --mock-port 0
```

Записывайте фактический напечатанный endpoint, а не порт из примера. Scripted
AI отвечает на две приведённые просьбы и примеры существующей B3 fixture;
это не универсальный AI и не Qwen. Микрофон, реальные KWS/VAD/STT/NPU не
открываются. Voice status в fixture синтетический. Кнопки управления отправляют
реальный HTTP к mock. Ctrl+C останавливает только созданные процессы, выводит
счётчик/журнал запросов и удаляет временный store. Повторный запуск начинает
пустую конфигурацию. Restart/persistence доказаны отдельным автоматическим E2E.

## Воспроизведение инженерных проверок

Linux/aarch64; нужны уже установленные Node 22.12+, Chromium и DISPLAY.
Зависимости не скачиваются автоматически. Используйте существующие pinned npm,
RKLLM 1.3.0 и sherpa 1.13.7 dependencies только для чтения; для frontend нужен
локальный `node_modules`. Не запускайте model-backed benchmark при работающем
production.

```sh
npm --prefix src/admin/frontend run typecheck
npm --prefix src/admin/frontend test
npm --prefix src/admin/frontend run build
cmake -S . -B build-m19-integrated \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python \
  -DFIREFLY_ADMIN_DIST_DIR="$PWD/src/admin/frontend/dist"
cmake --build build-m19-integrated -j1
ctest --test-dir build-m19-integrated -R '^M19DemoVerticalSlice$' --output-on-failure
ctest --test-dir build-m19-integrated --output-on-failure
```

Сборка real appliance разрешена; запуск `firefly-demo` / live benchmarks здесь
не выполняется. CTest использует fake RKLLM API и speech seams там, где нужны
модели/устройства. Новый E2E требует `m19_test_worker`, который имеет file driver
исключительно в test build, не новый product endpoint. Voice test создаёт
synthetic PCM, KWS/VAD и STT transcript, затем запускает настоящий
`runWakeVoiceLoop → RuntimeWorker::processVoice → CommandEngine → SafetyGate →
BindingResolver → HttpHardwareExecutor`. HTTP-команды не инжектируются через
`/api/test-command` или кнопки управления.

Artifacts: `build-m19-integrated/m19-acceptance/results.json`, `server.log`, PNG на каждом этапе
для 1280×800 и 390×844. В results есть binary SHA256, runtime source/build
identity, HTTP журнал и видимые ошибки. Существующие API/browser suites отдельно
проверяют ownership/CAS, отсутствующие факты, отмену, malformed proposals,
activation failure, TCP probe, SSRF/HTTP validation и no blind retries.
Инженерный результат и список известных ограничений: [отчёт](m19-engineering-report.md).

## Один последующий owner gate: настоящий Qwen и reSpeaker

Текущий интегрированный кандидат — `dd1ffa2`, merge `818157ec` в существующую
M19-ветку. Последующие docs-only commits не меняют его код. Для live-проверки
нужен clean build этого интегрированного кода с обновлёнными Admin binary и
Web Admin assets и записанной build identity. `d87d667`/`74850ba` не являются
безопасными кандидатами: они предшествуют исправлению original-request grounding.
Установленный `818157ec` содержит SafetyGate, но не M19 Admin language-profile/UI.
Схема ConfigStore и протокол исполнения не изменены.

Исторический настоящий AI gate и real-Qwen text evidence относятся к предыдущим
кандидатам, а не к новому интегрированному прохождению. Offline fixture не
доказывает новую генерацию Qwen или акустическое распознавание. Физический gate
[Issue #18](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/18)
остаётся открытым/отложенным; исходники wake/STT не меняются и пороги не подбираются.

После Chief Engineer review владелец отдельно разрешает **одно** эксклюзивное
окно. Всё, кроме произнесения двух фраз, готовит и проверяет агент:

1. До остановки сверить тогдашние PID, release, unit, argv, модели, ConfigStore,
   SHA256/backup и владение NPU/микрофоном. Подготовить независимое автоматическое
   восстановление именно тогдашнего production, не старого `dc7b142e`; проверить
   его до остановки. Планируемый лимит: 6 минут испытания + 4 минуты восстановления.
2. Подготовить свежий private 0700 store, localhost mock **127.0.0.1:18765** с
   новым request log и Admin **127.0.0.1:18080**. Старый mock 8765 не трогать.
   Не переключать release pointer и не изменять unit. Только после разрешения
   штатно освободить production NPU/ALSA, затем запустить один test runtime с
   настоящими Qwen, reSpeaker, KWS, VAD, STT и прежним BEEP.
3. Через проверенный browser harness выполнить AI Setup: «Создай зону Кабинет
   и лампу в кабинете, которую можно включать и выключать». Сохранить **исходное**
   предложение до правок. `лампа`, `лампу`, `в кабинете`, boolean Питание и ON/OFF
   должен предложить настоящий Qwen. Нет формы/она неверна — **AI gate FAIL**,
   без Apply, сохранить evidence и восстановить production. Ручная коррекция
   возможна как отдельный fallback, но не даёт AI gate PASS.
4. При PASS принять в план, ввести явный contract: POST `/relay/1` с `on=true`
   / `on=false`, GET `/relay/1` и `on → power`. Проверить общий Review, выполнить
   **один** Apply только в test store. До Apply ноль HTTP; runtime/LanguageIndex READY.
5. Владелец заранее подтверждает слышимость browser-приглашений в одном наушнике
   и явно даёт СТАРТ. Два сигнала, только после реального READY: произнести слитно
   **«Амина, включи лампу в кабинете»**. После проверки ON три сигнала: произнести
   **«Амина» → окончание BEEP → пауза около 0,5 с → «выключи лампу в кабинете»**.
   Вкладка остаётся подключённой; наблюдать терминал или настраивать Admin не нужно.
6. Для каждой команды агент связывает настоящий wake/VAD/STT transcript с
   `INDEX_EXACT`, SafetyGate PASS, `source=voice`, ровно одним POST и отдельным
   GET → Confirmed. Программные отрицательные проверки обозначаются как текстовые,
   а не как голос владельца. При wake/STT failure сохранить диагностику, пропустить
   следующий сценарий, не менять модели/пороги и не повторять окно автоматически.
7. При завершении, ошибке или timeout остановить только тестовые процессы,
   штатно восстановить исходный production, проверить READY/WAITING_WAKE,
   STT/LLM/wake, микрофон, sole ownership и ConfigStore SHA256/права. Сохранить
   proposal, browser trace/screenshots, runtime log, HTTP log и итог с SHA256.

Никаких ручных SSH, настройки Admin, Apply или разбора логов от владельца не
требуется. Настоящая лампа не используется: Confirmed подтверждает localhost mock.
Без этого свежего физического gate MVP не объявляется voice-accepted.

## Rollout / rollback / cleanup — только после отдельного разрешения

Commit `d87d667` меняет production Admin-код (`src/admin/AdminLanguage.cpp`)
и Web Admin assets. Схема ConfigStore и протокол исполнения команд не изменены.
Реальная проверка M19 требует clean build интегрированного кандидата с
новыми Admin binary/assets и проверенной build identity. Production `818157ec`
подтверждает установку SafetyGate, но не M19 AI-first language-profile/UI.
Если владелец отдельно разрешает новый release, использовать clean build
выбранного кандидата и существующие `stage-release.py stage/verify/install/activate` из
[M18.6 packaging](m18-relocatable-production-packaging.md). Проверить production
identity, права, свободное место и SHA256; повторно использовать уже установленные
shared models. Не копировать многогигабайтные модели для M19. Активировать и
перезапускать сервис только по отдельно утверждённому плану; не merge/deploy из
этого PR автоматически.

Rollback — существующая проверенная release-процедура с `previous-release` и
проверкой совместимости текущего store. Не заменять production store пустым demo
store; разрешённый пользовательский Setup не откатывается подменой JSON.
Сначала закончить и остановить тестовые процессы, сохранить evidence, затем
удалить только каталоги стенда/изолированной сборки при отсутствии процессов.
Не удалять installed release, shared models, rollback checkout или production
ConfigStore. После независимого review M19 агент останавливается.
