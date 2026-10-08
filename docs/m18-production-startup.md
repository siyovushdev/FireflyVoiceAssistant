# M18.1 — Production startup

Дата: 2026-10-08. Baseline: `a4310f4` (`master`, PR #5), B1–B4 owner accepted.
Ветка: `hardening/production-startup`. **M18.1 owner acceptance COMPLETE / PASS**.
Ниже записаны подтверждённые владельцем production результаты, включая реальный
reboot/autostart и voice cycle. При этом documentation update агент не выполнял
service install/start/enable/reboot. M18.2 не начат.

`git fetch origin --prune` и `git pull --ff-only origin master` были предприняты,
но HTTPS GitHub credentials недоступны (`could not read Username`; `gh auth status`
также не авторизован). `git switch master` успешен, локальные HEAD и origin/master
точно совпали с ожидаемым `a4310f4`; status и diff check были чистыми. Ветка создана
от этого baseline. Свежесть remote сверх этого SHA не подтверждена.

## Проблема и exact audit

| Настройка до M18.1 | `scripts/firefly-dialogue.sh` по умолчанию | Старый systemd unit |
| --- | --- | --- |
| Executable | `$ROOT/build/firefly-demo` через `exec` | `/home/firefly/FireflyVoiceAssistantLinux/build/firefly-demo` напрямую |
| Store | обязательный аргумент helper | `/var/lib/firefly/store` |
| Init config | `$ROOT/config/empty-home.json` только с `--new` | `/home/firefly/FireflyVoiceAssistantLinux/config/current-m4.json` |
| Runtime model | `/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm` | тот же |
| Admin | `127.0.0.1`, `${FIREFLY_ADMIN_PORT:-8080}` | `127.0.0.1:8080` |
| `--voice` | есть | **нет** |
| `--stt-model-dir` | `${FIREFLY_STT_MODEL_DIR:-$ROOT/build/stt/model}` | нет; DemoMain мог найти executable-relative STT для Admin, но это не включало voice |
| `--vad-model` | `${FIREFLY_VAD_MODEL:-$ROOT/build/stt/vad-ab/silero_vad.onnx}` | нет |
| `--audio-device` | `plughw:CARD=Array,DEV=0` | нет; совпадающий default не используется без voice |
| `--wake-enabled` | `true` | нет; default true не включает voice сам по себе |
| `--wake-python` | `${FIREFLY_WAKE_PYTHON:-$ROOT/build/wake-venv/bin/python}` | нет |
| `--wake-model` | `$ROOT/assets/wake/amina-v3.tflite` | нет |
| Cutoff/window | implicit C++ defaults `0.99`/`7` | те же defaults, voice не запущен |
| Playback | implicit C++ `plughw:CARD=rockchipes8388,DEV=0` | тот же default, voice не запущен |
| Preflight / libraries | read-only competing RKLLM check + STT/VAD/interpreter checks, затем exec | `LD_LIBRARY_PATH=/home/firefly/demo_Linux_aarch64/lib`, без shell/preflight |

Аудит: `DemoMain.cpp`, `RuntimeModel.h`, unit/helper, M17, dialogue-setup,
v1-amina-wake и CMake. Production unit теперь включает полный принятый runtime;
helper дополнительно передаёт playback/cutoff/window явно с прежними значениями.
Никаких изменений model/prompt, STT/VAD, wake algorithms, SafetyGate, CommandEngine,
LanguageIndex, B4 UI или Admin publication semantics.

## Startup contract и ExecStart

Выбран минимальный вариант **B**: прямой unit + явный documented contract, защищённый
`ProductionStartupContract`. Не вводятся shell launcher, отдельный config parser
или дополнительные сервисы. Тест разбирает unit и **исполняет настоящий helper**
в private fixture с argv-recorder вместо binary; сравнивает полный набор аргументов,
не только наличие строк. Намеренные различия — выбранный helper store, `--new` и
существующие диагностические env overrides. Любое расхождение default mandatory
arguments, пропуск `--voice`, demo seed, non-loopback bind, root user или weakening
зафиксированных unit policies проваливают тест. Список обязательных значений —
`tests/ProductionStartupContractTests.py:REQUIRED`; его изменение требует review.

Production unit: `packaging/firefly-voice-assistant.service`:

```ini
Environment=LD_LIBRARY_PATH=/home/firefly/demo_Linux_aarch64/lib
ExecStart=/home/firefly/FireflyVoiceAssistantLinux/build/firefly-demo \
  --store /var/lib/firefly/store \
  --init-config /home/firefly/FireflyVoiceAssistantLinux/config/empty-home.json \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-bind 127.0.0.1 \
  --admin-port 8080 \
  --voice \
  --stt-model-dir /home/firefly/FireflyVoiceAssistantLinux/build/stt/model \
  --vad-model /home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab/silero_vad.onnx \
  --audio-device plughw:CARD=Array,DEV=0 \
  --wake-enabled true \
  --wake-python /home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python \
  --wake-model /home/firefly/FireflyVoiceAssistantLinux/assets/wake/amina-v3.tflite \
  --wake-cutoff 0.99 --wake-window 7 \
  --playback-device plughw:CARD=rockchipes8388,DEV=0
```

`systemd → firefly-demo → Admin/ConfigStore/RuntimeGeneration → RKLLM + local
STT/VAD + one ALSA capture → owned Amina Python worker → voice pipeline`.
Это один appliance main process; существующий owned KWS child остаётся его частью,
а не отдельным systemd service. После boot helper запускать не требуется.

## Filesystem/build/audio prerequisites

Сохраняется принятый board layout; перенос установки и Admin4096 model deferred.
Все пути выше читаются `firefly`. Требуются:

- native aarch64 binary с `FIREFLY_BUILD_ADMIN=ON`, `FIREFLY_BUILD_ASSISTANT=ON`,
  `FIREFLY_BUILD_VOICE=ON`, embedded B4 dist;
- RKLLM 1.3.0 library/model по путям unit; прежний Admin4096 verified model
  остаётся по пути из `RuntimeModel.h`, M18.1 его не переносит;
- STT directory: `encoder.int8.onnx`, `decoder.onnx`, `joiner.int8.onnx`, `tokens.txt`;
  прежний Silero VAD file;
- sherpa 1.13.7 CPU bundle в `build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib`;
  текущий CMake binary содержит RUNPATH к нему и RKLLM. `ldd` с ровно unit
  `LD_LIBRARY_PATH` разрешил все libraries, без `not found`;
- wake interpreter с LiteRT 1.2.0, pymicro-features 2.0.2, numpy 2.2.6 (проверены
  installed metadata), frozen Amina model, worker `build/share/firefly/wake/amina_worker.py`
  и assets, которые размещает CMake. Никаких downloads;
- USB reSpeaker XVF3800 `Array` (stereo 16 kHz, ASR channel 1), board playback
  `rockchipes8388`. Для validated XVF3800 + RK3588S / Linux 6.1.118 appliance
  требуется USB firmware **2.0.10 или новее**; фактически проверена **2.0.10**.
  **2.0.6 явно отвергнута** результатами acceptance как production-unsafe на этой
  конфигурации. Более новые версии отдельно этим acceptance не проверялись;
  подробные признаки отказа и подтверждение firmware приведены ниже.

`User=firefly`, `Group=firefly`; фактические supplementary groups уже включают
`audio` и `video`. `/dev/snd` имеет `root:audio`, mode 0660; board playback доступен.
DRM card/render nodes доступны текущему UID на read/write. Модели STT/VAD/wake и
interpreter доступны на чтение/exec. Не добавлены root execution, chmod 777,
SupplementaryGroups или широкие device exceptions без необходимости.

`ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges=true`,
`PrivateTmp=true` сохранены; модели/код читаются, ALSA/DRM не скрываются через
PrivateDevices/DevicePolicy. Единственный explicit writable path — private
`/var/lib/firefly/store`, который владелец создаёт до первого start. Доступ,
необходимый для production startup и voice cycle в реальном systemd sandbox,
подтверждён owner acceptance ниже.

## ConfigStore, failure, shutdown

`--init-config` читается **только при отсутствии `configuration.json`**, как уже
реализовано в DemoMain. Новый store пуст: нет zones/devices/actions/bindings.
Исторические filename/`system_home`/interpretation profile внутри принятого seed
не создают demo entities и не меняются в этом milestone. Существующий valid store
не перезаписывается даже с другим или malformed init seed. Остаются прежние
reconcile/LKG rules; после restart физическое состояние cold-start Unknown.

Модель/STT/VAD/mic/wake startup exceptions оставляют процесс/Admin доступными,
health degraded, voice unavailable, wake failed. При отсутствии transcriber
status sync может отображать STT `unavailable/not_configured`, а конкретная ошибка
остаётся в startup journal. Это сохранённая health semantics, не новая recovery.
Missing shared library, invalid CLI/build, занятый Admin port или недоступный store
могут мешать самому запуску процесса — это другие классы ошибок.

Admin socket создаётся до model/STT initialization, но до HTTP event loop в main
есть synchronous Admin STT construction. Отсутствующий file даёт исключение и
быстрый degraded startup; vendor init hang всё ещё может задержать обслуживание.
Автоперезапуска voice/microphone внутри приложения не добавлено. M18.2 отдельно.

SIGTERM: admission закрывается, `RuntimeWorker::stop()` отклоняет новую работу,
service/voice thread joins, ALSA/KWS/STT освобождаются через RAII, HTTP clients
завершаются. `SHUTDOWN complete` печатается после joins; финальные model destructors
заканчиваются перед process exit. Тест проверяет и marker, и destructor, и exit 0.
`Restart=on-failure`, `RestartSec=5`, `TimeoutStopSec=45`, `KillSignal=SIGTERM`,
`KillMode=mixed`, journald/start limits сохранены. Нормальный stop не использует
SIGKILL; прежние emergency timeout fallback systemd и bounded wake-child teardown
не изменены. Vendor hang может превысить graceful budget — гарантия не расширялась.

## Свежие automated checks

```sh
npm --prefix src/admin/frontend run build
cmake -S . -B build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_ASSISTANT=ON -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python
cmake --build build -j2
ctest --test-dir build -R 'ProductionStartup|HealthReliability|VoicePipeline|WakeVoiceGating|SeamlessWake|WakeSessionUx|AminaWorkerLifecycle|WakeLiveHelper' --output-on-failure
ctest --test-dir build --output-on-failure
systemd-analyze verify packaging/firefly-voice-assistant.service
git diff --check
```

- Build + offline gate PASS; B4 frontend source не менялся, Node frontend tests
  отдельно не запускались. Full CTest включает зарегистрированные Chromium suites.
- `ProductionStartupContract`: 3 stdlib unittest cases PASS; hardware/models не нужны.
- Focused CTest: **9/9 PASS**, 17.65 s. `ProductionStartup`: 5 starts of actual
  DemoMain with fake RKLLM (fixture не линкует vendor library) и missing speech paths:
  fresh empty seed; existing named catalog; restart with empty seed; restart with
  malformed seed; model failure. Hash/bytes existing store неизменны, Admin HTTP 200,
  zero executions, degraded/voice unavailable, SIGTERM exit 0 + model destructor.
  Тесты используют только temporary stores; physical ALSA release этим fixture
  не заявляется. Amina child reaping/gating/shutdown проверяются отдельными suites.
- Первый focused run: 8/9 из-за неверного ожидания нового теста `stt=failed` вместо
  фактического `unavailable` после status sync. Исправлено ожидание по source;
  production runtime не менялся. Финальный focused результат выше.
- Полный CTest: **71/71 PASS**, 256.08 s; `ProductionStartup` 5.61 s.
  Зарегистрированные реальные Chromium: root **63.68 s**, manual **47.29 s**,
  AI Setup **42.29 s**, все PASS (test workers, не live NPU/microphone).
- `systemd-analyze verify`: exit **0**, unit syntax errors нет. Честно сохранены
  предупреждения о **других** system units:
  `netplan-ovs-cleanup.service: Failed to open /run/systemd/system/netplan-ovs-cleanup.service: Permission denied`;
  `Configuration file /lib/systemd/system/rkaiq_3A.service is marked world-writable. Please remove world writability permission bits. Proceeding anyway.`
  Это не warnings о неустановленном Firefly unit; чужие units не изменялись.
- `bash -n scripts/firefly-dialogue.sh`, `git diff --check`: PASS.

Логи: `/tmp/firefly-m18-{frontend-build,configure,build,focused,focused-final,ctest}.log`,
`/tmp/firefly-m18-systemd-verify.log`.
Все результаты этого раздела свежие; исторические B4/voice acceptance не выдаются
за systemd/reboot acceptance M18.1.

## Final owner acceptance — COMPLETE / PASS

Источник: подтверждённые владельцем production проверки на целевом appliance.
Это результаты реального systemd/microphone/voice/reboot acceptance, а не выводы
из automated fixtures. M18.1 завершён; M18.2 не начат.

### 1. Fail-soft: reSpeaker физически отсутствует

- systemd service остался active, Admin остался доступен;
- runtime/config/LLM/STT стали ready;
- voice/microphone стали unavailable, overall health — degraded;
- точная причина: `Cannot open ALSA capture plughw:CARD=Array,DEV=0: No such device`.

### 2. reSpeaker подключён, service перезапущен

Подтверждены `runtime_ready=true`, `voice=WAITING_WAKE`, `microphone=connected`,
STT ready, wake ready и overall health ready.

### 3. Реальный voice cycle

Обнаружено «Амина», затем `BEEP_START -> BEEP_END`, `LISTENING_COMMAND`,
`SPEECH_START / SPEECH_END`. Точный transcript: **«какая погода»**.
Пустой production domain безопасно вернул **Unsupported / target_not_grounded**;
hardware action не выполнялся. State вернулся в `WAITING_WAKE`, acknowledgement
health — ready.

### 4. Graceful systemd shutdown

- service чисто перешёл в inactive;
- `firefly-demo` и amina worker завершились;
- port 8080 освобождён, RKLLM owners отсутствовали, microphone device освобождён;
- зафиксированы `VOICE_STATE stopping -> stopped`, `SHUTDOWN complete` и
  уничтожение RKLLM;
- systemd сообщил `Deactivated successfully`.

### 5. Реальный reboot/autostart

Service enabled и автоматически active после host reboot; ручного запуска
Firefly/helper не было. SHA256 `/var/lib/firefly/store/configuration.json`
сохранился:

```text
36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be
```

Подтверждены `runtime_ready=true`, `startup_phase=ready`, `needs_apply=false`,
`microphone=connected`, `voice=WAITING_WAKE`, health overall=ready и wake=ready.
Финальный post-reboot cycle **«Амина» + «какая погода» PASS**, с возвратом в
`WAITING_WAKE`.

### 6. Hardware prerequisite: XVF3800 USB firmware

На **2.0.6** USB/ALSA enumeration проходила успешно, native format был
`S16_LE / 16 kHz / stereo`. ALSA PCM переходил в `RUNNING`, но `hw_ptr` оставался
`0`. `arecord` создавал только 44-byte WAV header и завершался с `Input/output error`;
kernel выдавал flood `retire_capture_urb` callbacks. Firefly корректно обнаруживал
`Wake audio stalled for 2 seconds` и безопасно переходил в degraded. Restart
Firefly не помогал; прямой `arecord` подтвердил отказ ниже уровня Firefly.

Vendor `xvf_host REBOOT 1` немедленно восстанавливал PCM: USB re-enumerated,
прямой 3-second `arecord` проходил успешно, размер файла **192044 bytes**.
До upgrade через `xvf_host` подтверждены `VERSION 2 0 6` и
`BLD_MSG ua-io16-sqr`. Firmware обновлена через официальный **DFU Upgrade alt=1**;
после upgrade подтверждены `VERSION 2 0 10` и `BLD_MSG ua-io16-sqr`.

На **2.0.10** прямой capture прошёл успешно. Реальный host reboot **без
`xvf_host REBOOT`** также прошёл успешно, direct capture после reboot остался
healthy. После ещё одного reboot Firefly production autostart достиг стабильного
`WAITING_WAKE`: без `Wake audio stalled` и без `retire_capture_urb` flood.
Финальный реальный voice test прошёл успешно.

Это внешнее hardware/firmware поведение, устранённое обновлением firmware
устройства; **Firefly voice code для этого не изменялся**. Для validated appliance
USB firmware **2.0.10 или новее — production prerequisite**, с непосредственно
подтверждённой версией 2.0.10; **2.0.6 не допускается** по этому acceptance evidence.
`xvf_host REBOOT 1` был диагностическим восстановлением, а не требованием
production startup и не реализацией M18.2 recovery.

## Manual acceptance runbook (для повторной проверки)

Acceptance выше уже завершён. Команды ниже сохранены как процедура повторной
проверки владельцем; firmware prerequisite обязателен.

### A. Install/update, start, health, stop/restart

Сначала подключить Array и проверить конкурирующие процессы. Если найдены чужие
listeners/audio/NPU consumers, выяснить владельца; не выполнять pkill/killall.
Свой прежний manual helper завершить в его терминале через Ctrl-C.

```sh
cd ~/FireflyVoiceAssistantLinux
id firefly
cat /proc/asound/cards
pgrep -af 'firefly-demo|firefly-assistant|amina_worker.py'
ss -ltnp '( sport = :8080 )'
sudo fuser -v /dev/snd/*
systemctl status firefly-voice-assistant --no-pager
```

Код 1 у pgrep/fuser при отсутствии процессов ожидаем. До start убедиться, что порт
и microphone свободны. Уже установленный **этот** service перед update остановить
через `sudo systemctl stop firefly-voice-assistant` и проверить shutdown ниже.

```sh
sudo install -d -m 700 -o firefly -g firefly /var/lib/firefly/store
sudo install -m 644 packaging/firefly-voice-assistant.service /etc/systemd/system/firefly-voice-assistant.service
sudo systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/firefly-voice-assistant.service
sudo systemctl start firefly-voice-assistant
systemctl is-active firefly-voice-assistant
systemctl status firefly-voice-assistant --no-pager
systemctl show firefly-voice-assistant -p MainPID -p NRestarts -p ExecStart
curl -fsS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/
curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
journalctl -u firefly-voice-assistant -b --no-pager -n 150
```

Ожидается active, HTTP 200, runtime/STT ready, wake ready, microphone connected,
`VOICE_STATE WAITING_WAKE`, без роста NRestarts. Playback readiness фактически
подтверждается успешным beep при separated wake, не только наличием ALSA card.
Удалённый браузер: на своём компьютере
`ssh -N -L 8080:127.0.0.1:8080 firefly@<board-address>`, затем
`http://127.0.0.1:8080/`. LAN bind не меняется.

Проверка нормального stop и сохранности valid configuration (между hash reads не
редактировать/Apply config). Эти команды ничего не удаляют из store:

```sh
sudo sha256sum /var/lib/firefly/store/configuration.json
firefly_service_pid=$(systemctl show -p MainPID --value firefly-voice-assistant)
pgrep -a -P "$firefly_service_pid"
sudo systemctl stop firefly-voice-assistant
systemctl show firefly-voice-assistant -p ActiveState -p MainPID -p Result
journalctl -u firefly-voice-assistant -b --no-pager -n 80
pgrep -af 'firefly-demo|amina_worker.py'
sudo fuser -v /dev/snd/*
sudo systemctl start firefly-voice-assistant
sudo sha256sum /var/lib/firefly/store/configuration.json
```

Stop: `SHUTDOWN requested/complete`, service MainPID=0, owned demo/wake child gone,
Array capture больше не удерживается Firefly; не должно быть timeout/SIGKILL.
Hash существующей configuration совпадает. Повторить health/WAITING_WAKE checks.
Обычный дальнейший restart: `sudo systemctl restart firefly-voice-assistant`.
Для негативного startup smoke: после graceful stop отключить microphone и start;
ожидать process alive/Admin 200/degraded/wake failed/voice unavailable без crash
loop. После подключения нужен явный service restart (не M18.2 recovery).

### B. Boot без ручного helper

Только после PASS этапа A и с подключённым microphone:

```sh
sudo systemctl enable firefly-voice-assistant
systemctl is-enabled firefly-voice-assistant
sudo reboot
```

После reboot открыть SSH без запуска helper/STT/wake/runtime вручную:

```sh
systemctl is-active firefly-voice-assistant
systemctl show firefly-voice-assistant -p MainPID -p NRestarts
curl -fsS http://127.0.0.1:8080/api/status | python3 -m json.tool
journalctl -u firefly-voice-assistant -b --no-pager -n 150
```

Дождаться WAITING_WAKE; для принятого пустого production domain произнести «Амина»,
затем **«какая погода»**. Ожидается точный transcript, безопасный
Unsupported / target_not_grounded без hardware action и возврат в WAITING_WAKE.
Настраивать устройство для этого acceptance не требуется; seed не должен сам
добавлять demo kitchen или binding. Зафиксировать wake, beep, transcript,
route/result и возврат в WAITING_WAKE. Этот документ не уполномочивает агента
автоматически устанавливать/enabling/start service или reboot.

## Ограничения и изменённые файлы

M18.2 USB microphone recovery/backoff **deferred**. Также deferred: path relocation,
Admin4096 relocation, packaging installer, watchdog/vendor-hang hardening, Host/auth/
HTTPS/LAN security, другие transports, backups/clock/history work. Нет новых AI
возможностей. Production startup/voice в systemd sandbox, fail-soft без microphone,
graceful shutdown и реальный reboot/autostart подтверждены owner acceptance PASS.
Firmware prerequisite и граница проверенной версии зафиксированы выше; M18.2
recovery/backoff не реализовывался и не проверялся.

Изменены: `packaging/firefly-voice-assistant.service`, `scripts/firefly-dialogue.sh`,
`CMakeLists.txt`, `src/admin/CMakeLists.txt`,
`docs/milestone-17-runtime-service-reliability.md`.
Новые: `tests/ProductionStartupContractTests.py`, `tests/admin/ProductionStartupTests.py`,
`docs/m18-production-startup.md`.
`user-store` не читался/не изменялся. Commit/push/merge не выполнялись.

Финальный Git status: 5 modified + 3 untracked files, ровно перечисленные выше;
HEAD остаётся `a4310f4`, branch `hardening/production-startup`. `git diff --check`: PASS.
