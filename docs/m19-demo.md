# M19: AI Setup → HTTP → voice → Confirmed

Owner live acceptance found missing `лампу` ([#16](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/16)).
See the [correction, evidence limits and remaining owner gate](m19-russian-device-profile.md).
The revised genuine Qwen proposal and real `source=voice` POST are still awaiting
separate live acceptance; a scripted demonstration does not close that gate.

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
| **Начать настройку**. «Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать». | Предложение: Кабинет, Лампа, Питание (boolean), Включить/Выключить; язык зоны **в кабинете**, устройство **лампа / лампу**. Проверить поля; **Изменить: …** при необходимости. |
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
python3 scripts/m19-private-demo.py --build-dir build
```

Скрипт запускает только `firefly-admin` и **test-only** `m19_test_worker`, Python
HTTP mock и новый временный ConfigStore с правами 0700. Порты 18080 / 8765;
при конфликте остановится, не трогая чужой процесс. Для свободных портов:

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
cmake -S . -B build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python
cmake --build build -j2
ctest --test-dir build -R '^M19DemoVerticalSlice$' --output-on-failure
ctest --test-dir build --output-on-failure
```

Сборка real appliance разрешена; запуск `firefly-demo` / live benchmarks здесь
не выполняется. CTest использует fake RKLLM API и speech seams там, где нужны
модели/устройства. Новый E2E требует `m19_test_worker`, который имеет file driver
исключительно в test build, не новый product endpoint. Voice test создаёт
synthetic PCM, KWS/VAD и STT transcript, затем запускает настоящий
`runWakeVoiceLoop → RuntimeWorker::processVoice → CommandEngine → SafetyGate →
BindingResolver → HttpHardwareExecutor`. HTTP-команды не инжектируются через
`/api/test-command` или кнопки управления.

Artifacts: `build/m19-acceptance/results.json`, `server.log`, PNG на каждом этапе
для 1280×800 и 390×844. В results есть binary SHA256, runtime source/build
identity, HTTP журнал и видимые ошибки. Существующие API/browser suites отдельно
проверяют ownership/CAS, отсутствующие факты, отмену, malformed proposals,
activation failure, TCP probe, SSRF/HTTP validation и no blind retries.
Инженерный результат и список известных ограничений: [отчёт](m19-engineering-report.md).

## Отдельный owner gate: настоящие Qwen / голос / устройство

**Не выполнен в M19 engineering run.** Production уже владеет NPU и микрофоном.
Нужны отдельное разрешение владельца и эксклюзивное владение runtime на время
проверки. Ни скрипт, ни агент не останавливают production и не переключают release.

1. Chief Engineer выбирает конкретный commit; владелец явно разрешает окно
   эксклюзивного теста. Сохранить текущий release/store для rollback по существующей
   M18.6 процедуре. Подтвердить, что нет второго RKLLM, STT или capture owner.
2. В этом окне подготовить новый private 0700 store, независимый mock и live
   appliance из принятого build, на другом Admin-порту, с существующими shared
   models. Пример **только после разрешённого освобождения runtime**:

   ```sh
   umask 077
   M19_LIVE_DIR=$(mktemp -d /tmp/firefly-m19-live-XXXXXX)
   mkdir -m 700 "$M19_LIVE_DIR/store"
   python3 scripts/mock-http-device.py --port 8765 --request-log "$M19_LIVE_DIR/http.jsonl"
   ```

   Mock оставить в отдельном терминале. В другом, с тем же записанным
   `M19_LIVE_DIR`, запустить:

   ```sh
   build/firefly-demo --store "$M19_LIVE_DIR/store" \
     --init-config config/empty-home.json --admin-port 18080 \
     --model /opt/firefly/voice-assistant/models/runtime/Qwen3-4B_W8A8_RK3588_3core.rkllm \
     --admin-model /opt/firefly/voice-assistant/models/admin/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm \
     --voice --stt-model-dir /opt/firefly/voice-assistant/models/stt \
     --vad-model /opt/firefly/voice-assistant/models/vad/silero_vad.onnx \
     --audio-device plughw:CARD=Array,DEV=0 --wake-enabled true \
     --wake-python /opt/firefly/voice-assistant/current/share/firefly/wake/python/bin/python \
     --wake-model /opt/firefly/voice-assistant/current/share/firefly/wake/amina-v3.tflite \
     --wake-cutoff 0.99 --wake-window 7 --playback-device plughw:CARD=rockchipes8388,DEV=0
   ```

3. Пройти таблицу выше с настоящим Qwen. Проверить естественные `лампу` и
   `в кабинете` в предложении; если нужно, поправить видимыми полями до Apply.
   После Apply дождаться runtime ready и `WAITING_WAKE`, затем сказать ON/OFF.
   Собрать исходный transcript, route `INDEX_EXACT`, SafetyGate PASS,
   execution source `voice`, latency и точный HTTP журнал. При pause после wake
   ожидается существующий BEEP; настройки frozen не менять.
4. Проверить «Амина, не включай лампу в кабинете», «Амина, включи и выключи
   лампу в кабинете», неизвестную зону и речь без wake: ноль команд. Проверить
   POST → Predicted и явный GET → Confirmed. Повторить no-zone на отдельном
   новом store. Malformed/type/HTTP-error Observe и LKG recovery уже покрыты
   изолированным C++/Chromium E2E; они не требуют повторного захвата NPU.
5. Перезапустить **только тестовый** процесс с тем же store. До GET — Unknown,
   ноль команд; после явного GET — Confirmed. Закончить тест, сохранить evidence,
   восстановить production исключительно по отдельному owner плану. Затем
   удалить только записанный temporary каталог, после остановки его процессов.

Live speech/model quality, реальный hardware API, физическая лампа и production
reboot/cutover — отдельные acceptance gates; localhost mock их не заменяет.
Ничего не правит записанную baseline ошибку двери «открытвер на входе».

## Rollout / rollback / cleanup — только после отдельного разрешения

M19 меняет test harness и документацию; production binaries/schema/протокол
не требуют изменения ради этого доказательства. Можно сначала принять только
процедуру. Если владелец выбирает новый release, выполнить clean build выбранного
commit и существующие `stage-release.py stage/verify/install/activate` из
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
