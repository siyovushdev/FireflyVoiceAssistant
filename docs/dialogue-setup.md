# Диалоговая настройка ассистента

> Historical frontend report. The current Vue Web Admin opens at `http://localhost:8080/`; legacy vanilla assets are removed. See [B4 delivery and rollback](web-admin-ux-v2-b4.md). Backend contracts below remain supported.

Исходная ревизия: `4c187a78671f736013045d89295e792646405db9`.
До изменений: чистое дерево, 58/58 CTest. Сохранённый baseline переиспользован после reboot; незавершённые команды до отключения питания не засчитывались. Все последующие config mutations выполнены на отдельных test stores.

## Аудит исходной архитектуры

| Существующий компонент | Что переиспользовано / что добавлено |
| --- | --- |
| ConfigStore / ConfigValidator | прежние revision/hash CAS, backup, LKG и validation; приватный draft хранится отдельно от active config |
| RuntimeGeneration | прежние DomainView, LanguageIndex и Executor одного snapshot; добавлен общий setup admission и ожидание request lease |
| RuntimeModel / RkllmEngine | один владелец handle, synchronous inference; отдельный проверяемый Admin4096 profile и teardown перед загрузкой |
| AdminCopilot / AdminDraft / Plan | прежний strict decoder, allocator IDs, templates и candidate validation; короткие операции и многоходовая SetupSession поверх них |
| Admin language profile | прежняя нормализация, bounds, dedup и conflicts для новых атомов; они остаются в редактируемом draft |
| AdminTranscriber / Web Admin | прежний browser WAV ingress, локальный STT и формы; общий диалоговый draft, code-point counter и отдельный Admin recording bound |
| Domestic command / wake | прежние компактный stable Qwen, SafetyGate, Bindings, seamless KWS; только lifecycle pause/restart |

Новая система хранения конфигурации, самостоятельный AI service и runtime learning
не создавались. Активная конфигурация остаётся единственным source of truth.

## Архитектура

Сохраняются ConfigStore, CAS/revisions/backups/LKG и атомарная RuntimeGeneration
с DomainView/LanguageIndex. Диалог расширяет AdminCopilot, применяя проверенные
операции к отдельному черновику. Конфигурация работает только после явного Apply.

RuntimeGenerations закрывает admission до ожидания текущего request lease.
В режиме настройки новые text/voice/Playground запросы отклоняются. Audio worker
заканчивает текущий запрос и освобождает capture; после возврата создаётся свежий
wake/VAD state. RuntimeModel подтверждает teardown старого handle до нового init.
Неуспешный teardown означает restart_required, а не вторую загрузку.

Модель рабочего режима: Qwen3-4B W8A8, context 1024, прежний compact command path.
Модель настройки: локальный CTX4096 экспорт с SHA256
`58e8e74d8f129ea6f68b8e2101aae65d512668f46bb847a89e88a6a6d6094e5b`.
Проверка SHA выполняется до загрузки; подмены на модель 1024 нет.

## Предварительная ресурсная проверка

До reboot отдельный probe подтвердил export `max_context_limit: 4096`, успешные
init/destroy и три коротких inference вместе с Admin STT:

| Показатель | Измерение |
| --- | --- |
| STT RSS | 244136 KiB |
| Qwen 4096 + STT RSS | 5040744 KiB |
| PSS | 5038269 KiB |
| Минимальный наблюдавшийся MemAvailable | 1618256 KiB |
| После освобождения Qwen RSS | 241888 KiB |
| Три коротких запроса | 4759 / 5745 / 4278 ms |

Один упрощённый план исходной просьбы: 109 output tokens, 28131 ms.
Это не доказательство надёжного диалога или достаточной памяти во всех случаях.
Последующий интеграционный probe оборвался на второй попытке после malformed
JSON. Возврат к 1024 в этом прогоне не подтверждён. Во время него также шла сборка;
причина зависания не установлена. В сохранённом kernel journal OOM/panic не найден.
После reboot NPU проверки выполняются отдельно от сборки/CTest с монитором памяти.

## Ограничения и восстановление

Admin message: 4000 Unicode code points / 16000 UTF-8 bytes. Domestic input по-прежнему
192 bytes. Это разные admission policies. Отдельный model step ограничен
консервативной byte-проверкой и фактическими token/cache/output guards RKLLM.
Точной tokenizer preflight API в установленном SDK нет.

SetupSession сохраняет общий versioned draft в приватном `setup-draft.json` внутри
store. Он не является активной конфигурацией. После перезапуска не применяется;
resume требует повторной проверки base revision/hash. Повтор transport request
с тем же ID не повторяет операции. Ручной draft обновляется с проверкой версии.

Inactivity: `FIREFLY_SETUP_INACTIVITY_SECONDS=1800` (300..7200).
Disconnect grace: `FIREFLY_SETUP_DISCONNECT_SECONDS=120` (30..1800).
Heartbeat отдельно от активности пользователя. Время синхронного планирования не считается бездействием: после ответа даётся новый интервал для heartbeat/уточнения. Обрыв связи не запускает Apply.

Apply: полный ConfigValidator/LanguageIndex admission → выгрузка Admin Qwen →
восстановление рабочего контекста при закрытом command admission → preflight →
CAS Save → atomic generation Apply → LKG → открытие admission. Ошибка после Save
требует compensating CAS revision старого каталога; ошибку восстановления нельзя
показывать как успешный результат.

Browser voice использует тот же AdminTranscriber и редактируемый текст; setup
recording limit — 60 секунд, бытовой лимит не меняется. По лимиту запись
останавливается и распознаётся. Экспорт Zipformer не принимает 60 секунд одним tensor: длинная Admin-запись распознаётся последовательными окнами до 17 секунд, с выбором тихой границы между 12 и 15 секундами. Все PCM samples покрываются ровно один раз. На стыках возможны ошибки слов, поэтому UI предупреждает и оставляет текст редактируемым; автоматической отправки нет. Бытовой STT-профиль и его предел остаются прежними. При отмене browser tracks закрываются, поздний результат старой записи игнорируется. Apply во время записи/распознавания предлагает сначала завершить ввод, чтобы не возвращать бытовой capture поверх записи браузера. Raw audio не сохраняется. Для plain HTTP нужен
localhost или SSH tunnel, например `ssh -L 8080:127.0.0.1:8080 firefly@HOST` и
`http://localhost:8080`. Настоящий микрофон и слышимость динамика не проверены.

## Аппаратная приёмка 2026-09-30

Полный реальный прогон и отдельная semantic validation прошли. Результаты:
[`rk3588-results.json`](../benchmarks/dialogue-setup/rk3588-results.json).
Исходные тексты, полный граф, per-call metrics и source fragments сохранены также
в `build/dialogue-recovery/acceptance-preserved-names.json`; лог и PSS/RSS рядом.
Проверялся отдельный пустой store, затем его подтверждённая версия. Пользовательский
store не использовался и hardware не вызывался.

| Сообщение | Результат | Content calls | Input / output tokens | Wall, с |
| --- | --- | ---: | ---: | ---: |
| Детская + «телевизер» + свет только ON/OFF | 1 зона, 2 power-only устройства | 9 | 232 / 191 | 100,64 |
| «Свет назови основным. Ещё добавь ночник…» | тот же draft, 3 power-only устройства | 5 | 116 / 73 | 44,67 |
| Телевизор → экран в подтверждённом доме | точное переименование | 1 | 15 / 6 | 4,39 |
| «Переименуй его» | вопрос, без model call | 0 | 0 / 0 | 0,065 |
| Уточнение: экран → телевизор | предыдущий план продолжен | 1 | 19 / 8 | 4,68 |
| Кабинет + вентилятор + кладовая + лампа; вентилятор не добавлять | 5 связанных шагов, вентилятор отсутствует | 13 | 315 / 353 | 171,80 |

Calls включают language proposals и format retries, но не bootstrap. Отдельно:
29 context bootstrap runs (125,664 с суммарно), 5 startup bootstraps (15,972 с),
1 заключительный runtime command. Всего 64 vendor runs, а не 29.
Общее время harness: 562,44 с. Admin-контексты меняются по задачам; большая модель
при этом остаётся загруженной. Обычный runtime сохраняет прежний stable compact.
В длинном сценарии часть языковых предложений отклонена strict validator; сохранены
исходные формы и видимое предупреждение, без принятия malformed результата.

| Ресурс / проверка | Фактический результат |
| --- | --- |
| Export context limits | 1024 и 4096 в vendor logs |
| Peak RSS / PSS | 5 149 668 / 5 147 969 KiB (около 4,91 GiB) |
| Минимальный MemAvailable | 1 916 952 KiB (1,83 GiB) |
| Admission / нижний резерв | 6 GiB до Admin load / 1 GiB перед inference; monitor 1 Hz |
| Модели | runtime → destroy → admin → destroy → runtime → destroy → admin → destroy → runtime → destroy |
| Запрос больше domestic limit | 199 UTF-8 bytes реально переданы Admin inference |
| 60 с recorded PCM при Admin4096 | реальный Sherpa STT, 2434,39 ms |
| Возврат к runtime compact | `[40]`, 4 tokens, prefill 1459,37 ms, generation 984,64 ms, wall 2459 ms |
| После harness | процесс завершён; загруженных RKLLM процессов не осталось |

Два setup cycles проверены в одном процессе. Перед запуском monitor проверял
`/proc/*/maps`; каждый следующий init следовал успешному destroy предыдущего.
Это вместе с тестом единственного владельца handle подтверждает отсутствие
одновременных больших instances в проверенных путях. Измерения не обещают
неограниченный RAM-запас: сложные задачи по-прежнему ограничены budget/admission.

Ранние неуспешные прогоны сохранены: malformed планы, неправильное имя при
переименовании и замена «кладовой» синонимом не считались приёмкой. Последний
semantic validator проверяет имена, граф и исключения отдельно от exit code.

## Контракт приложения и бюджет

Отдельный `AdminRequestPolicy` считает Unicode code points (не UTF-16 units и не
графемы): `Array.from(text).length` в браузере и валидирующий UTF-8 проход в C++.
Cyrillic и emoji считаются одинаково. JSON body ограничен 512 KiB с учётом
конфигурационного черновика; внутри сообщения строго 16000 bytes / 4000 code points.
Пустой, некорректный UTF-8 и управляющие символы отклоняются до inference.

Один application message — не один вызов модели. Последовательные предложения разделяют **один** кандидат и связи созданных IDs.
Ссылка «там» между шагами получает проверенную зону, явно созданную предыдущим
шагом той же просьбы. Неоднозначная ссылка без такого контекста требует уточнения. Ограничения:
1800 UTF-8 bytes на группу, максимум 16 групп, 3 операции в группе, 10 запросов
языкового профиля на сообщение, одна форматная retry, 10 минут планирования,
32 сообщения в сессии. Отдельное слишком большое предложение отклоняется явно,
без нарезки посередине отрицания/ссылки. Короткий ответ уточнения обрабатывается вместе с исходной незавершённой просьбой
в одном bounded шаге; если такой контекст превышает 1800 bytes, приложение просит
сформулировать меньшую связанную часть. При ошибке/уточнении вся просьба откатывается к предыдущему
черновику; частичный кандидат не представляется завершённой работой.

Admin request reserve — 2048 tokens; до inference применяется консервативная
проверка размера шага в bytes, затем фактические RKLLM metrics/KV guards.
Диалог использует output budget 384, языковой профиль 256, однозначное
переименование 128. Bootstrap Admin context генерирует один служебный token,
его ответ не является планом. Deadline одного inference — 60 s; vendor abort
остаётся синхронным и не гарантирует hard deadline при зависании драйвера.

Общий каталог не отправляется модели. Контекст содержит только найденные
кандидаты/текущие зависимости (не более 12 объектов / 1200 UTF-8 bytes). При
превышении запрашивается более точный scope; кандидаты не обрезаются до первого.
C++ проверяет ссылки по **полному** каталогу, включая точные отображаемые имена
и подтверждённый язык. Обычный voice resolver не меняется. Для однозначного
переименования target/zone закрепляются сервером, модель возвращает только name.

## Черновик и возможности

Поддержаны создание зоны, создание generic power-only устройства, переименование,
удаление устройства/отмена неприменённого создания, явное добавление яркости и
изменение границ существующего числового поля. Остальные операции требуют
уточнения/ручных подробных настроек; произвольные аппаратные возможности не
изобретаются. `power_only` содержит boolean power и ровно ON/OFF. Это отдельная
декларативная сборка, а не полные TV/light шаблоны с лишними функциями.

C++ назначает IDs через существующий Copilot allocator, проверяет все ссылки,
защищённые операции, schema и conflicts. Новые языковые atoms проходят прежние
strict decode/normalization/dedup; неуспешное языковое предложение оставляет
исходные формы и предупреждение. Существующие ручные aliases не перезаписываются. Явно запрошенное новое имя устройства добавляется как нормализованный, проверенный alias вместе с переименованием; конфликт не обходится.
Подключений у новых устройств нет. UI сообщает об этом отдельно от программных
capabilities. Qwen не может выбирать transport, секреты, shell, paths или Apply.

`setup-draft.json` содержит исходные сообщения, source fragments шагов, кандидат,
base revision/hash, версии и обезличенные token/latency metrics. Permissions 0600,
размер до 1 MiB, atomic rename + fsync. Raw audio и секреты в trace не пишутся.
Секреты существующего каталога остаются только в приватном store и его черновике.
Тот же candidate показывают формы и диалог. Ручная правка требует новой версии;
старые предложения проверяются по snapshot/hash и перестают быть применимыми.

## Управление жизненным циклом

```text
ASSISTANT_RUNNING
  → ENTERING_SETUP (новые команды отклонены; принятую команду дослушать/завершить)
  → release ALSA capture + voice workers → destroy runtime1024
  → verify SHA / memory admission → load admin4096
  → SETUP_ACTIVE (browser STT доступен, бытовой микрофон остановлен)
  → RESTORING_ASSISTANT (destroy admin4096 → init/warm runtime1024)
  → candidate preflight → CAS Save → atomic Apply → LKG
  → ASSISTANT_RUNNING (fresh capture/KWS/VAD)
```

Простое открытие страницы ничего не переключает. Production Admin AI endpoints
проверяют SETUP_ACTIVE; обходного скрытого переключения рабочего контекста нет.
Новые бытовые запросы во время setup получают `setup_in_progress` и не сохраняются
в очередь. Старый request lease удерживает свою generation до завершения.
Во время setup health отдельно отмечает недоступность бытового runtime/hardware.

При отмене CAS Save отсутствует: прежний подтверждённый domain остаётся активным,
после model restore assistant снова доступен. Черновик сохраняется с явным статусом.
Во время inference отмена помечается pending, ждёт синхронного завершения, затем
восстанавливает модель. При невозможном teardown/restore — `restart_required`,
admission закрыт, автоматической повторной загрузки поверх старого handle нет.

| Сбой | Результат |
| --- | --- |
| Проверка SHA/RAM или Admin init | попытка вернуть прежний runtime; черновик не Apply |
| malformed/truncated/model deadline | просьба не принимается, рабочий store неизменён |
| потеря браузера / inactivity | bounded grace, cancel+restore; черновик остаётся |
| stale base/manual version | конфликт; явное обновление/resume, без перезаписи |
| validation/preflight до Save | прежняя конфигурация, ошибка видна |
| ошибка после Save | compensating CAS старого каталога, Apply+LKG; при сбое restart_required |
| power loss/process restart | обычный startup подтверждённого ConfigStore/LKG; draft не Apply |
| vendor destroy/abort завис | второго instance нет; требуется операторский restart |

Save — durable commit point конфигурации; Apply — атомарный runtime commit point.
После power loss между ними применяется существующий startup/LKG recovery.
Успех кнопки объявляется только после Apply, LKG и прогрева/открытия admission.

## Запуск и проверка в новой сессии

Из корня repository (уже установленные модели остаются на своих местах):

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j2
./scripts/firefly-dialogue.sh /путь/к/существующему/store
```

Для **отдельного нового дома**, не текущей установки:

```sh
./scripts/firefly-dialogue.sh "$PWD/build/dialogue-demo-store" --new
```

Helper проверяет загруженные RKLLM процессы, не останавливает их и запускает один
`firefly-demo` с wake + STT и loopback Admin. `--new` инициализирует отсутствующий
store из `config/empty-home.json`; существующую конфигурацию не заменяет.
По умолчанию используются `build/stt/model`, `build/stt/vad-ab/silero_vad.onnx`,
`build/wake-venv/bin/python`. Для другой установки поддержаны переменные
`FIREFLY_STT_MODEL_DIR`, `FIREFLY_VAD_MODEL`, `FIREFLY_WAKE_PYTHON`,
`FIREFLY_ADMIN_PORT`. Никаких model path переключений между сообщениями нет.

1. Откройте `http://localhost:8080` через SSH tunnel; само открытие не останавливает wake.
2. Нажмите «Начать настройку». Дождитесь освобождения рабочего runtime и загрузки Admin.
3. Отправьте исходную просьбу о детской, TV и свете только ON/OFF.
4. Уточните: «Свет назови основным. Ещё добавь ночник только с включением и выключением».
5. При желании запишите голос, остановите и исправьте распознанный текст до отправки.
6. Проверьте дерево, действия, предупреждения и «Посмотреть изменения».
7. Подтвердите «Применить настройки и включить ассистента» либо отмените.

Новые устройства без Binding не управляют физическим оборудованием. Для настоящих
команд нужен отдельно подтверждённый connection. Manual forms доступны через
«Подробные настройки», но не нужны для каждой сущности в основном сценарии.

Автоматическая проверка без физического микрофона:

```sh
DISPLAY=:0 cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
DISPLAY=:0 cmake --build build -j2
DISPLAY=:0 ctest --test-dir build --output-on-failure
# Только после окончания build/CTest и проверки отсутствия другого RKLLM:
python3 benchmarks/dialogue-setup/Monitor.py \
  --prefix build/dialogue-recovery/new-acceptance
```

Chromium тест запускается при доступных Node, Chromium и DISPLAY. Fake browser
media flags используются только тестом для синтетического источника, не являются
инструкцией отключать защиту реального браузера. Runtime STT replay отдельно
использует реальный Sherpa/Zipformer и сохранённый DEV WAV. Ни одна из этих проверок
не доказывает качество настоящего микрофона пользователя или слышимость динамика.

### Короткие операции диалога

Диалоговый model ingress использует отдельный ограниченный wire codec:

```json
{"ops":[["location","Новая зона"],["power_device","Новая зона","Устройство"]]}
```

Либо ровно `{"question":"Уточнение"}` / `{"unsupported":"Причина"}`.
Кортежи имеют фиксированные имена/типы/число аргументов. Дополнительные поля,
неизвестные операции, неверные типы и обрезанные ответы отклоняются. C++ заполняет
проверенные постоянные поля (`type=room`, `template=power_only`) и передаёт операции
**существующему** AdminCopilot decoder. Это адаптер входа диалога, не второй Config
или AI service. Обычный Qwen compact protocol бытовых команд не изменяется.

Формат избегает длинного повторного вывода описаний, template и summary. Если
переименование однозначно, сервер уже закрепил target и запрашивает только новое
имя. Отмена однозначно указанного неприменённого создания (в том числе «последнее
устройство пока не добавляй») вообще не требует угадывания Qwen: C++ использует
проверенные ID и порядок операций текущего draft. Текст с несколькими целями
остаётся неоднозначным; подтверждённое устройство такой отменой не удаляется.

Планирование ограничивает и набор предлагаемых операций на шаг. Для явно
положительной просьбы о создании модель видит только контракт зоны/power-device,
а не список нерелевантных операций переименования, удаления и изменения диапазонов.
Точные имена и schema проверяются C++. Это снижает сложность model output без
повышения temperature, output/context лимитов или ремонта некорректного ответа.

## Проверки и границы V1

Focused suites: `RkllmContextRefresh`, `AdminTranscribe`, `LanguageAdminChromium`,
`DialogueSetup`, `DialogueSetupWebModel`, `DialogueSetupChromium`. Полный CTest
прошёл 61/61 suite за 105,66 с; focused-проверки — 6/6 за 38,32 с. Проверены LanguageIndex, compact Qwen, seamless wake, SafetyGate,
Bindings/Executor, CAS/LKG и существующий Admin. Новые deterministic проверки
покрывают Unicode admission, malformed operations, exact power-only capabilities,
request leases, немедленный API/voice admission barrier, pending cancellation,
stale ownership, restore failure, post-Save rollback, private storage и reconnect.

Chromium language-profile regression теперь входит в настройку перед AI, проверяет
отсутствие autosave, подтверждает общий draft одной кнопкой и проверяет INDEX_EXACT
после выхода. UI предлагает «Начать настройку» для всех ресурсоёмких AI операций.

Поддержаны ограниченные типы операций, перечисленные выше; произвольные физические
capabilities и подключения не придумываются. Сложный/неоднозначный scope требует
уточнения. Одно предложение более 1800 bytes отклоняется явно, даже если общий
application message укладывается в 4000 code points. Политика 4000/4001 и Unicode
проверена детерминированно; реальный связанный сценарий состоял из пяти шагов.

Модель может ошибаться в непротестированных формулировках. Нужен просмотр плана
перед подтверждением; это не обещание безошибочного понимания любого текста.
Переключение модели и vendor inference синхронны; отмена ждёт завершения текущего
шага. При невозможном teardown — restart_required, без второго instance.
Не заявляются ручной тест микрофона пользователя, слышимость динамика или работа
новых виртуальных устройств с физическим оборудованием без Binding.
