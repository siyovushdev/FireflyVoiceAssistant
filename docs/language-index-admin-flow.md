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
