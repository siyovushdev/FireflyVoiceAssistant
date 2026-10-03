# Web Admin UX V2 — B1 foundation and delivery

Дата: 2026-10-03. Основа: [утверждённый дизайн](web-admin-ux-v2-design.md), commit `f666e7c`. B1 добавляет отдельный read-only Vue frontend; production vanilla UI остаётся на `/`.

## Доступ и границы B1

После запуска **пересобранного** C++ Admin/assistant server на штатном порту 8080:

- прежний UI: `http://localhost:8080/`;
- V2: `http://localhost:8080/v2/#/overview`;
- зоны: `http://localhost:8080/v2/#/zones`;
- устройства: `http://localhost:8080/v2/#/devices`;
- состояния: `http://localhost:8080/v2/#/states`.

На другой машине можно использовать существующий localhost/SSH tunnel:

```sh
ssh -L 8080:127.0.0.1:8080 firefly@FIREFLY_HOST
```

Маршруты hash router работают без server-side SPA fallback. Refresh device detail возвращает `/v2/`, а fragment остаётся в браузере. `/api/*`, неизвестные assets и неизвестные server paths не получают HTML приложения.

Реализованы shell/sidebar/header, Overview, Zones, Devices с поиском и фильтром зоны, read-only Device Detail, States. «Без зоны» — полноценное представление устройства. AI Настройка, Подключения, Дополнительно и Диагностика явно показывают «Будет в следующем этапе».

B1 не содержит редакторов, микрофона, AI setup, Save/Apply, command controls, connection probe или hardware polling. В Vue API client предусмотрены только пять GET endpoints. Ссылка на прежний UI является обычным переходом в отдельный интерфейс.

## Frontend и данные

Исходники: `src/admin/frontend/`. Runtime libraries: Vue 3.5.43, Vue Router 4.6.4. Build tools: TypeScript 5.9.3, Vite 8.3.2, `@vitejs/plugin-vue` 6.0.9 и `vue-tsc` 3.3.12. Последние два — необходимые build-time средства компиляции и проверки Vue SFC. Pinia, UI kits и отдельный test framework не добавлены; unit tests используют Node test runner и предварительный `tsc` emit.

`types/wire.ts` сохраняет `rooms/room_id` и неизвестные поля. `api/client.ts` проверяет используемые поля ответов и рекурсивно замораживает исходный DTO, не реконструирует configuration и ничего не сериализует обратно. `adapters/catalog.ts` создаёт `ZoneView`, `DeviceView.locationId` и представление evidence. Начальное значение schema и legacy/Mock state не читаются как physical evidence.

`stores/system.ts` хранит отдельно stored, active, status, state response и execution history. Основной каталог — active; если его нет, stored показывается с явной надписью «не активна». Различие stored/active обозначено баннером; просмотр его не устраняет.

Evidence допускается только при совпадении hash/revision/generation ID с отображаемой active configuration. При рассогласовании или ошибке state/active endpoint значения становятся Unknown. Последний доступный catalog при сетевой ошибке сохраняется для просмотра с предупреждением об устаревании. History не подменяет device state; Mock execution явно подписан как виртуальная проверка.

Пять GET выполняются последовательно: legacy IPC serializes state/history через общий runtime operation lock и при параллельных чтениях может отвечать 409. Ошибка одного GET не останавливает остальные. Повторное обновление не запускается поверх незавершённого; интервал 10 секунд действует только для видимой вкладки. Кнопка «Обновить данные» читает backend memory, а не оборудование.

Unknown, Predicted, Confirmed и backend `stale` показываются отдельно по каждому свойству. Timestamp выводится рядом с evidence. Completeness mappings, reachability и property knowledge имеют разные подписи. Accepted HTTP command не превращается в Confirmed в браузере. Произвольные булевы свойства показываются как «Да/Нет»; подпись «включено/выключено» относится только к `power`.

## Воспроизводимая сборка

Подготовленная build machine: Node 22.12+ (проверено 22.23.2), npm, Python 3, штатные CMake/C++ tools. Первичная установка npm packages выполняется явно; `package-lock.json` фиксирует все версии и integrity. Offline rebuild требует заранее подготовленного npm cache или готового dist. Runtime полностью offline.

```sh
cd /home/firefly/FireflyVoiceAssistantLinux/src/admin/frontend
npm ci
npm run typecheck
npm run test
npm run build
```

`build` выполняет Vite production build и `tests/offline.mjs`. Последний проверяет manifest digests, наличие hashed JS/CSS, локальные ссылки HTML/CSS, отсутствие inline scripts/styles и query-string cache-busters. Зависимости не скачиваются при `npm run build`.

```sh
cd /home/firefly/FireflyVoiceAssistantLinux
cmake -S . -B build \
    -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
    -DFIREFLY_ADMIN_DIST_DIR=/home/firefly/FireflyVoiceAssistantLinux/src/admin/frontend/dist
cmake --build build -j4
ctest --test-dir build --output-on-failure
git diff --check
```

Флаг `FIREFLY_ADMIN_DIST_DIR` — явный opt-in. Без него новый чистый build оставляет vanilla-only delivery; backend/core не требуют npm или готового frontend. Если флаг задан, но manifest отсутствует, конфигурация завершается объяснимой ошибкой. CMake никогда не запускает `npm install`, `npm ci` или network fetch. Используется разрешённый вариант delivery через **готовый dist**, без автоматического frontend build target.

Проверка отдельного backend build без V2, Node tool invocation и inference:

```sh
cmake -S . -B build/b1-core-only \
    -DFIREFLY_BUILD_ASSISTANT=OFF \
    -DFIREFLY_BUILD_VOICE=OFF \
    -DBUILD_TESTING=OFF \
    -DFIREFLY_ADMIN_DIST_DIR=
cmake --build build/b1-core-only --target firefly-admin -j4
```

## Как доставляются assets

```text
Vue/TS + package-lock
  → Vite production dist
  → dist/firefly-assets.json (explicit paths + SHA-256)
  → CMake build-time package-admin-v2.py
  → verified AdminV2Bundle.h (bytes + URL/MIME allowlist)
  → linked C++ HttpServer
  → /v2/ + /v2/assets/<hashed-name>
```

В B1 выбран embedded variant упаковки: bundle bytes встраиваются в transport binary вместо чтения новых произвольных filesystem paths на runtime. Это делает V2 artifact и binary одной версией. Manifest не раздаётся HTTP и не управляется клиентом. В dist сейчас четыре обслуживаемых файла: HTML, JS, CSS, SVG. Build output сообщает точные размеры.

Упаковщик разрешает только `index.html` и известные типы файлов в плоском `assets/`, проверяет digest, дубликаты, bounds, существование regular files и отсутствие symlinks в asset path. Traversal/absolute paths/query/неподдерживаемые расширения отклоняются. Необъявленный файл даже внутри dist не встраивается. Сервер использует прежний map lookup, без directory listing и filesystem resolution по URL.

Старый allowlist и vanilla files не изменены. Его текущая build-directory доставка (`build/src/admin/web`, путь `FIREFLY_ADMIN_ASSETS`) по-прежнему нужна при запуске существующего binary; B1 не меняет packaging прежнего UI. V2 не требует dist, Node, npm или `node_modules` после линковки. Runtime package должен содержать штатные binaries/libraries/vanilla assets; frontend source/dependencies/dev server в него не входят.

CSP self-only, no-store, nosniff, same-origin mutation guards и `X-Firefly-Admin` остались без изменений. UI использует только локальный SVG и system fonts. На стороне frontend нет cloud SDK, telemetry, CDN или service worker.

Source fingerprint CMake теперь перечисляет исходные поддеревья без обхода `frontend/node_modules`, `dist` и `.test-build`. Это необходимо после добавления npm project: прежний `src/*` recursive glob включил бы зависимости и generated outputs в fingerprint/reconfigure.

## Проверки и артефакты

Frontend unit tests покрывают сохранение редких DTO fields, immutable snapshot, non-home zones/no-zone, отсутствие physical values из `initial`, false/zero, все evidence modes, разделение reachability/mapping, generation mismatch, malformed API, read-only endpoint allowlist, partial failures, последовательное чтение IPC и stored-only degraded view.

`AdminV2Packaging` выполняет четыре теста допустимого manifest, необъявленного файла, traversal, неизвестного расширения, corrupt/missing asset, дубликатов и symlinks.

`AdminV2Chromium` запускает production C++ `firefly-admin` с существующим fake inference worker и приватным temporary store. API, ConfigStore, RuntimeGeneration, SafetyGate, executor и state lifecycle — реальные. Test controller подготавливает HTTP mock/evidence и saved-but-not-active revision; сам браузер выполняет только GET. C++ server запускается с `PATH=/nonexistent`, без доступного Node executable в PATH.

Acceptance проверяет старый `/`, V2 module assets/MIME/CSP, каждый route, reload Device Detail, no-zone filter, Цех/Склад/Лабораторный стенд, stored/active separation, Unknown/Predicted/Confirmed, отдельный Stale после **реального 60-секундного TTL** backend, отсутствие горизонтального scroll при 1280×800, 800×600, 390×844. Перехват запросов отклоняет и записывает любые внешние или не-GET browser requests. Hardware request count, config identities, underlying state evidence и history должны оставаться неизменными после работы UI.

```sh
ctest --test-dir build -R '^AdminV2' --output-on-failure
```

Для Chromium нужны Node, установленный Chromium и `DISPLAY` при CMake configure. Если их нет, CMake явно предупреждает, что `AdminV2Chromium` не зарегистрирован. Такой build не является полной B1 acceptance.

Артефакты реального browser run: `build/v2-acceptance/acceptance.json`, `server.log`, `overview-1280.png`, `overview-800.png`, `overview-390.png`. Это тестовые данные и не tracked production assets.

Результаты проверки:

- `npm ci --no-fund --no-audit`: воспроизводимая установка из lockfile прошла.
- `npm run typecheck`: PASS.
- `npm run test`: 8/8 PASS.
- `npm run build`: PASS; offline verification — 4 объявленных локальных assets.
- Полный CMake build: PASS; отдельный core-only build без V2: PASS.
- Полный CTest: **67/67 PASS**, 195.44 секунды, включая прежние Chromium suites и реальный `AdminV2Chromium` (63.27 секунды).
- Финальный повтор `ctest --test-dir build -R '^AdminV2' --output-on-failure` после сборки окончательного bundle: **2/2 PASS**, 62.41 секунды; Chromium — 62.28 секунды.
- `git diff --check`: PASS; отдельно проверены whitespace и завершающие newlines всех 31 новых B1 files.

Логи: `build/b1-build.log`, `build/b1-ctest.log`, `build/b1-v2-final.log`. Финальные browser artifacts относятся к bundle `index-DHYXaKYz.js`.

Пользовательский `user-store` не участвует ни в тестах, ни в fixture setup; исходные незакоммиченные изменения там сохранены. Production service в этой работе не перезапускался: acceptance поднимает и останавливает собственный C++ server на временном localhost port.

## Файлы B1

Изменены `.gitignore`, `src/admin/CMakeLists.txt`, `src/admin/HttpServer.cpp`.

Добавлены:

```text
docs/web-admin-ux-v2-b1.md
scripts/package-admin-v2.py
src/admin/frontend/
  package.json
  package-lock.json
  tsconfig.json
  tsconfig.tests.json
  vite.config.ts
  index.html
  src/
    main.ts
    App.vue
    types/wire.ts
    api/client.ts
    adapters/catalog.ts
    stores/system.ts
    composables/useRefresh.ts
    router/index.ts
    components/DeviceCard.vue
    components/PropertyEvidence.vue
    views/OverviewView.vue
    views/ZonesView.vue
    views/DevicesView.vue
    views/DeviceDetailView.vue
    views/StatesView.vue
    views/PlaceholderView.vue
    styles/base.css
    styles/mark.svg
  tests/adapters.test.mjs
  tests/offline.mjs
tests/admin/v2/
  system.json
  PackagingTests.py
  ChromiumTests.js
```

`node_modules`, `dist`, `.test-build` и `build` исключены из Git. Production references, модели, prompt, inference, LanguageIndex, SafetyGate, config schema и REST endpoints не менялись. B2/B3 не начаты; commit/push не выполнялись.
