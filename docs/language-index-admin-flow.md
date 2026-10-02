# Язык команд: конфигурация, предложение ИИ и Apply

Qwen помогает один раз описать слова пользователя, затем известные команды
понимает deterministic LanguageIndex. Модель не является источником истины.
Источник — подтверждённая конфигурация; индекс полностью пересоздаётся из неё.

## Где хранится язык

Используются прежние поля schema v2:

| Сущность | Поля |
| --- | --- |
| Location | `speech.mentions`, `speech.locations`, `metadata.aliases` |
| Device | `spoken_aliases`, `metadata.aliases` |
| Action | `speech_aliases` |
| Аргумент | существующая typed parameter schema, включая enum values/unit/range |

Никаких таблиц тысяч полных предложений, schema v3 или SQLite. Фразы вроде
«кухонный свет включи» собираются matcher из атомов независимо от порядка слов.

## Предложение языка

В существующих карточках зоны, устройства и действия есть блок **«Язык команд»**
и кнопка **«Предложить варианты»**. Запрос отправляется по нажатию, не на каждый
символ. Новый endpoint `/api/admin/language-profile` принимает
`configuration`, `entity` (`location` / `device` / `action`), `id` существующей
сущности. Он использует прежний RuntimeWorker/ContextModel, сериализацию доступа
к единственному RKLLM и bounded retry (не более одной повторной попытки).
После запроса восстанавливается стабильный command context.

Для Action в контекст передаётся реальное определение, включая schema и
assignments, как DATA. Ответ может содержать **только aliases**. IDs, capability,
bindings, assignments, аргументы и безопасность не берутся из ответа модели.
Location дополнительно может предложить mentions и locations; Device — только
aliases. Decoder проверяет полный JSON без извлечения/repair, точный набор полей,
UTF-8, типы, размеры, управляющие/зарезервированные слова и синтаксис; приводит
регистр и границы слов к matcher-форме и удаляет дубликаты.

Limits: до 5 action aliases, до 6 device/location aliases, до 6 mentions,
до 4 location phrases; атом — до 96 UTF-8 байт и 8 слов; ответ — до 4096 байт.
Профиль возвращает conflict preview. Ошибка модели ничего не сохраняет.

Пользователь видит редактируемые поля, затем нажимает **«Проверить и добавить
в черновик»**. Если основной draft изменился после запроса, предложение нужно
создать заново: оно не накладывается на другую конфигурацию. Затем работают
обычные Validate → Save → Apply с прежними CAS/revision/LKG правилами.
Ни сама генерация, ни добавление в draft не сохраняют config автоматически.

Validate показывает language warnings и до пяти проверенных примерных команд.
Эти примеры — только preview, не источник runtime matching. Реальные hard
conflicts блокируют Validate/Save/Apply. Общий alias «свет» у устройств в разных
комнатах разрешён с предупреждением: без комнаты команда неоднозначна и не
попадает в Qwen; с однозначной комнатой выбирается соответствующий target.

## Playground и диагностика

JSON результата и Playground показывают `language_route` и `index_status`:
`INDEX_EXACT`, `INDEX_FUZZY`, `INDEX_MISS`, `INDEX_AMBIGUOUS`, `INDEX_INVALID`,
`LEGACY_FAST_PATH`, `LLM_FALLBACK`; отсутствие детерминированно выбранного
контекста отображается как `GROUNDING_REJECTED`. Также доступны hash, revision,
compiler version и fingerprint активного индекса. Никакого автоматического
сохранения успешно понятой Qwen формулировки нет.

## Новый дом

Инициализируйте store из `config/empty-home.json`. `compatibility: null` означает
отсутствие исторического фиксированного набора комнат. Создайте Location, generic
устройство и его реальные typed actions. Шаблон «Свет (настраиваемый)» содержит
ON/OFF/SET_BRIGHTNESS; в новом доме обычный шаблон light также преобразуется в
него. Сначала подтвердите язык, затем проверьте, сохраните и примените graph.
Отсутствие binding никогда не означает разрешение на production hardware:
Playground остаётся mock, production использует существующий BindingResolver.

## Реальная проверка Admin Qwen

`benchmarks/language-index/AdminSmoke.cpp` использует production RuntimeModel и
RuntimeWorker, одну Qwen3-4B W8A8, три language-only запроса: Кухня, Основной свет,
Включить питание. Фактический smoke прошёл 3/3: 45 / 19 / 10 output tokens соответственно,
одна попытка на сущность; `saved=false`, `applied=false`. Результаты, сырые ответы
и метрики — `benchmarks/language-index/admin-smoke.json`. Запускать отдельно от других RKLLM процессов:

```sh
./build/src/admin/language_admin_smoke \
  /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  tests/language/home.json build/language-admin-smoke.json
```

Предложение может быть неудачным; strict decoder и ручное редактирование остаются
обязательными. Модель не может подтвердить новое аппаратное свойство своими
словами. Controlled learning — отдельная будущая задача, здесь не реализована.

Подробности контракта, аудита и измерений:
[LanguageIndex architecture](language-index-architecture.md).

## Диалоговая настройка и владение моделью

В новом [режиме настройки](dialogue-setup.md) эти же языковые предложения
подготавливаются небольшими запросами для сущностей общего диалогового draft.
Production AI endpoints требуют SETUP_ACTIVE: runtime 1024 предварительно
освобождается, Admin 4096 остаётся загруженным на весь диалог. Предложение по-прежнему
не является источником истины; только явный Apply подтверждает Config и пересобирает
LanguageIndex одной RuntimeGeneration. Ручное редактирование использует тот же draft.

## Updating language without replacing the user's configuration

Location proposals request grammatical case forms and short synonyms, not
permutations of whole commands. The decoder still validates the model's bounded
response, then merges it with existing reviewed atoms. It preserves canonical
names/action words instead of silently replacing them. The merged result is
bounded by the existing schema (16 location metadata aliases, 32 other arrays).
A proposal exceeding those limits is rejected, never silently truncated.

For an existing home, explicitly enter setup, open the entity's **«Язык команд»**,
press **«Предложить варианты»**, inspect/edit the merged result and conflict
preview, and confirm it in the shared draft. Obsolete or poor aliases can be
removed explicitly in those fields. Finish with **«Применить настройки и включить
ассистента»**. Proposal alone does not Save or Apply. Automatic migration of the
user store is deliberately absent. Many existing adjective-named rooms already
benefit from the new derived grammar without changing any persisted atoms.

For new entities, dialogue setup invokes this same profile pipeline. The user
states the room/device/capabilities once; the matcher handles supported case
forms, prepositions, neutral «пожалуйста», and word-order permutations. Other noun
inflections and true synonyms remain reviewed language data, proposed by Qwen.
No claim is made to understand every possible Russian construction.

## On-board end-to-end text acceptance

Build first, and run the following **sequentially**, only when no other process
owns RKLLM and at least 6 GiB `MemAvailable` is available. Do not build or run
parallel tests while the models are resident. The executable rejects another
RKLLM owner and refuses any configuration with hardware bindings.

```sh
cmake --build build -j2
umask 077
mkdir -p build/language-fastpath-private
./build/src/admin/language_production_acceptance \
  build/language-fastpath-private/admin-created.json --create
```

This creates a separate private store, uses real dialogue and language proposals
for two rooms and three power-only devices, explicitly Applies that test draft,
and checks commands through `RuntimeWorker::processVoice` with injected text.
No aliases are hand-added to the acceptance catalog. The report verifies that
decoded language atoms actually survived Save/Apply, not merely that the model
returned a proposal. Logical successes, rejections, physical `binding_missing`
and actual runtime-model calls are recorded separately.

Take `test_store` from the generated report, then run a **new process**:

```sh
./build/src/admin/language_production_acceptance \
  build/language-fastpath-private/restarted.json --restart /absolute/path/to/test_store
```

For a read-only private copy of an existing configuration, `--config FILE` runs
the nine child-room command cases without setting up or saving anything. Never
commit the copy or reports derived from the user's installation. Model startup
and bootstrap are excluded from `text_parse_median_ms` / `text_parse_p95_ms`;
these measure CommandEngine understanding, excluding STT, VAD and transport.
The model is still loaded for fallback availability. Zero runtime calls means
zero per-command `infer`/`inferCommand` calls, not zero startup bootstrap work.
