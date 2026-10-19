# M20 G3: воспроизводимая изолированная приёмка

Это developer acceptance для #33/#36, а не независимое ревью или аппаратная приёмка. Итоговый SHA, команды, версии, результаты, хеши fixtures и evidence manifest фиксируются в отдельном `REPORT.txt` после выполнения проверок. Исторические отчёты на прежних SHA не переносятся на новый HEAD. Отдельная сессия должна повторить критические воспроизведения перед принятием G3.

## Границы и зависимости

Ubuntu x86_64, C++17, CMake, Python 3, Node и уже подготовленные frontend dependencies; настоящие headers/library libmosquitto и Mosquitto 2.0.18. Для браузера — Chromium и настоящий Vue bundle через C++ HttpServer. Системные пакеты и production не изменяются. Все брокеры слушают только `127.0.0.1`, используют уникальные порты, временные конфигурации и синтетические credentials. ConfigStore/backup/LKG — только disposable fixtures.

DNS regression дополнительно требует Linux user/network/mount namespaces, `unshare`, `mount`, `ip` и разрешённого rootless `unshare -Ur -m -n`. Namespace-root не является root хоста. Настройка resolver и доверенной CA допустима только внутри private mount namespace. При отсутствии этих возможностей результат — NOT_RUN/BLOCKED, не PASS. Pre-provisioned динамические зависимости брокера должны быть доступны загрузчику, например через `LD_LIBRARY_PATH` в окружении тестов.

## Чистые обязательные сборки

Использовать два **новых** каталога сборки и чистый checkout точного candidate SHA. Frontend dependencies берутся из проверенного lockfile без его изменения.

```sh
npm --prefix src/admin/frontend run typecheck
npm --prefix src/admin/frontend test
npm --prefix src/admin/frontend run build
umask 0002
cmake -S . -B /tmp/firefly-g3-off \
  -DCMAKE_BUILD_TYPE=Release -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DBUILD_TESTING=ON \
  -DFIREFLY_BUILD_LOCAL_MQTT_CONTROLLER=ON -DFIREFLY_ENABLE_MQTT=OFF
cmake --build /tmp/firefly-g3-off --parallel 4
ctest --test-dir /tmp/firefly-g3-off --output-on-failure
cmake -S . -B /tmp/firefly-g3-on \
  -DCMAKE_BUILD_TYPE=Release -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DBUILD_TESTING=ON \
  -DFIREFLY_BUILD_LOCAL_MQTT_CONTROLLER=ON -DFIREFLY_ENABLE_MQTT=ON \
  -DFIREFLY_MOSQUITTO_INCLUDE_DIR="$TEST_MQTT_STAGE/usr/include" \
  -DFIREFLY_MOSQUITTO_LIBRARY="$TEST_MQTT_STAGE/usr/lib/x86_64-linux-gnu/libmosquitto.so" \
  -DFIREFLY_TEST_MOSQUITTO_SERVER="$TEST_MQTT_STAGE/usr/sbin/mosquitto" \
  -DFIREFLY_MQTT_NAMESPACE_TESTS=ON
cmake --build /tmp/firefly-g3-on --parallel 4
ctest --test-dir /tmp/firefly-g3-on --show-only
ctest --test-dir /tmp/firefly-g3-on --output-on-failure
```

Обязательна фактическая регистрация и выполнение `MqttBrokerIntegration`, `MqttInitialBrokerRecovery`, `MqttObservationSafety`, `MqttConnectProbe`, `MqttProbeDnsIsolation`, `GroundingAuthorizationMqtt`, credential и local-controller tests. Production helper только компилируется; запуск против настоящего systemd запрещён. Снимать DISPLAY; не включать owner/hardware test flags.

## Исправления PROBE-R2/R3 и постоянные регрессии

- POST_BUILD устанавливает worker mode **0755**, независимо от umask 0002. Защита executable owner/mode остаётся прежней. Не исправлять candidate вручную через chmod перед тестированием: положительный probe обязан работать на нетронутом артефакте CMake. Полная ARM64 доставка и release permissions относятся к #37.
- `poll` использует округлённый вверх остаток monotonic deadline. Истечение одного ожидания проверяет абсолютный deadline снова, а не объявляет исправный worker недоступным. Настоящие IPC/exec/permissions ошибки сохраняют `mqtt_probe_worker_unavailable`.
- `MqttConnectProbe` проверяет mode и delayed CONNACK с бюджетами 50/73/100/151 мс, точный код timeout и немедленный следующий CONNECT.
- `MqttProbeDnsIsolation` запускает настоящий UDP DNS с задержкой **1250 мс**, настоящую libmosquitto, C++ HttpServer и Mosquitto в private namespaces. Для пяти slow DNS и пяти never-answer запросов с бюджетом **100 мс** обязательны HTTP 422 `mqtt_probe_timeout` и полный wallclock 95–450 мс (допуск планирования ОС, не ожидание DNS). Дополнительно: busy, 11 SIGKILL/reap, неизменность FD/threads/store/active/state, отсутствие FD71 и секретов в argv/env, немедленный следующий успешный CONNECT. Missing/corrupt/writable private helper должен давать worker_unavailable, не timeout.
- `GroundingAuthorizationMqtt` применяет существующие 450 adversarial authorization cases к настоящему MQTT executor и независимому сетевому наблюдателю. Подставляется только inference/распознанный текст; SafetyGate, поколения, BindingResolver и транспорт настоящие. Негативные/противоречивые/неизвестные команды не должны отправлять PUBLISH. Положительные контроли доказывают работоспособность монитора. Это **не** wake/STT/microphone/NPU acceptance.

## Расширенный evidence bundle #36

Помимо CTest пакет должен содержать исходники самостоятельных acceptance harness, команды и свежие логи следующей матрицы:

| Область | Необходимое доказательство |
|---|---|
| Probe | Реальный accepted/rejected CONNACK, wrong/missing/malformed credentials, trusted/untrusted CA, hostname mismatch, unavailable/partial/drop/timeout, concurrency; полный frame tap: 0 PUBLISH и 0 SUBSCRIBE; worker UID/FD/permissions/kill/reap |
| Wizard/Chromium | Пустой каталог, A/B drafts и независимые подписки, explicit HTTP GET replacement/transfer, HTTP+MQTT, KeepAlive, Cancel, CAS conflict, ровно один Save→Apply, отсутствие JS ошибок и ложного success |
| Два брокера | A state принимается только от A, B — только от B; до operational commands 0 command PUBLISH; отдельные ON/OFF QoS1/0 и свежий state; retained/reconnect без replay |
| MQTT core | Reverse ticket ordering, early ON/contradictory OFF/disconnect, строгие JSON/types/duplicate keys, A/B isolation, delayed/rejected SUBACK/PUBACK, resource caps, shutdown |
| Config/lifecycle | Save/CAS/Apply/restore/reconcile, current/backup/LKG, холодное восстановление, retired generations не обновляются, нет потерь HTTP совместимости |
| Credentials/control | Symlink/hardlink/FIFO, rotate/read barriers, durability faults, redaction, Setup/DELETE serialization; rootless synthetic systemctl, failed-enable compensation, FD_CLOEXEC/FD4→3; никаких настоящих служб |

Файлы, унаследовавшие слово `independent` из прежних reviewer harness, при запуске разработчиком являются **self-acceptance**, а не новым независимым PASS. Все временные процессы должны завершиться; отчёт не публикует пароли, ключи или содержимое credential store. Manifest и reproducible runner должны позволять другой сессии повторить проверки с чистым checkout и явно указанными provisioned зависимостями.

## Готовность и открытые gate

`G3_READY_FOR_INDEPENDENT_REVIEW` означает зелёную developer-матрицу на одном HEAD и отсутствие известных P0–P2 в разрешённом scope. Это не `READY_TO_CLOSE` всего M20 и не разрешение rollout. #36 ожидает отдельного независимого ревью. #37 (aarch64 packaging/release) и #38 (физическая приёмка) не выполняются здесь. MQTT CONNACK/PUBACK подтверждает протокол, а не физическое состояние реле; Confirmed в fixtures подтверждается отдельной свежей telemetry от тестового устройства.

## Remediation EXEC-DNS-R1 и G3-REPRO-R1

Operational MQTT теперь выполняет одну команду в отдельном `firefly-mqtt-publish-worker`, а не в network thread родителя. Родитель выделяет один из 16 publication slots и задаёт **один абсолютный monotonic deadline до чтения credentials и запуска процесса**. В бюджет входят DNS, TCP/TLS, CONNECT/CONNACK, разрешение публикации, PUBLISH/PUBACK и cleanup. При timeout IPC закрывается, worker завершается SIGKILL и reaped до освобождения slot/ответа. Не остаётся resolver/client, который позже отправит команду. Смерть родителя также завершает worker; ожидаемый parent PID проверяется после установки PDEATHSIG, включая окно до старта child.

Worker использует исходное DNS-имя, стандартную CA/hostname/SNI verification и одноразовый `mosquitto_connect` + manual `mosquitto_loop`, без loop thread, reconnect или повторной публикации. Parent передаёт credentials/topic/payload только через ограниченный anonymous AF_UNIX IPC; argv не содержит данных команды или секретов. Cleanup закрывает socket без отправки/flush queued MQTT packets. SIGPIPE игнорируется только внутри worker, чтобы OpenSSL close_notify после shutdown не превращал уже полученный PUBACK в fatal cleanup signal. При истечении бюджета до publish permit возвращается `mqtt_timeout`; после выдачи одноразового publish permit неясный исход — `mqtt_publish_uncertain`, без Success/Confirmed и без replay. Timeout не отменяет пакет, уже переданный в сеть до deadline: неопределённая доставка должна оставаться неопределённой.

Для прежних гарантий G0 parent subscription callbacks и worker используют один sealed memfd с lock-free 64-bit arrival counter. Worker берёт submission ticket непосредственно перед настоящим publish API, parent переносит этот barrier под коротким gate mutex, отбрасывая tentative samples до actual submit. Ожидания IPC выполняются вне mutex; до actual submission и успешного transport outcome tentative samples не меняют Confirmed. Поэтому telemetry из промежутка между IPC permit и фактическим submit остаётся старой; свежий early reply до PUBACK сохраняется и может подтвердить только после успешного transport result/Predicted. В memfd нет credentials, bindings или DeviceStateStore. Поколения и callback ownership не передаются в worker; удерживаемый executor владеет своим counter до завершения запроса.

CMake автоматически создаёт protected 0755 publish worker под umask0002 рядом с runtime и native tests. Missing/corrupt/writable/SUID/SGID/symlink worker fail-closed. Доставка helper в ARM64 release и install ownership остаётся отдельным #37, здесь не выполняется.

Новый opt-in CTest `MqttExecutionDnsIsolation` использует настоящий DNS delay1250ms/never-answer, настоящий Mosquitto и direct + HTTP→RuntimeWorker→BoundDeviceExecutor. Budget100ms должен давать отказ в пределах 95–450ms, 0 поздних PUBLISH даже после DNS response, reaped workers, повторное использование slots и следующий положительный IPv4 action с точным QoS1 packet. Дополнительно parent-death cancellation, private unsafe worker copies, unchanged ConfigStore и Unknown/Predicted без ложного Confirmed. Тот же private namespace test выполняет10 дополнительных direct/HTTP auth+TLS cases: два настоящих trusted CA/hostname/password publish и восемь wrong/missing-login/hostname/untrusted-CA отказов без PUBLISH и без Confirmed. `MqttObservationSafety` отдельно проверяет stale ticket в IPC gap, early post-submit reply и отсутствие подтверждения без actual submission. Fault-injected stalled IPC child дополнительно не должен задержать соседний реальный publish с собственным timeout; положительная ветка выполняет untouched worker и доставляет настоящий MQTT packet.

### Полный reproducer без ручных обходов

В evidence bundle `REPRODUCE.py` — точная копия tracked `scripts/m20-g3-reproduce.py`. Указать чистый checkout SHA из `FINAL-HEAD.txt`, уже подготовленные matching `node_modules`, stage с реальными зависимостями и **новый** output path. Команда работает из произвольного cwd, включая paths с пробелами:

```sh
cd /tmp
python3 /absolute/path/to/verified-bundle/REPRODUCE.py \
  --repo /absolute/path/to/clean-detached-checkout \
  --stage /absolute/path/to/provisioned-root \
  --output '/absolute/path/to/new output directory'
```

Runner проверяет exact HEAD/clean source, dependency/lockfile hashes и `harness-input-manifest.json`, создаёт **обычный локальный detached clone** в output, автоматически переносит подготовленные frontend dependencies, генерирует `.test-build` и dist через frontend gates, затем выполняет весь build→CTest→extra graph. Никаких ручных worktree symlink replacements, копирования `.test-build` или смены cwd нет. Symlink допустим только для проверенного dependency staging; ConfigLoader/O_NOFOLLOW не ослаблен. Права private fixture directories (в частности historical-store0700) фиксируются input manifest и автоматически сохраняются при копировании независимо от umask. Harness scripts, зависящие от соседних imports, запускаются с cwd каталога evidence; build scripts — с layout root. Nested failures, включая run-more groups и browser .exit, дают итоговый nonzero FAIL. Existing output никогда не переиспользуется и не стирается.

`M20G3Reproduce` проверяет реальный detached layout из arbitrary cwd/output, автоматическую подготовку generated artifacts, private fixture directory/file modes, immutable source, отказ при dependency tampering/существующем output и propagation nested subprocess exit7. После добавления двух regressions ожидаемые registrations: OFF70 и ON75; фактические PASS/counts относятся только к final HEAD и свежему REPORT.txt, а не к этому ожиданию.
