# M20 G2 / #35 — локальный Mosquitto, изолированный кандидат

> Статус: файловая реализация/тестовый кандидат. **Не устанавливать, не включать и не менять production Firefly** без отдельного явного согласования окна работ. Источник истины: Issues #29/#34/#35/#36/#37.

## 1. Что делает Web Admin

Во вкладке «Подключения» раздел **«Локальный брокер Mosquitto»** показывает реальные поля `installed/enabled/running` либо честную ошибку `unavailable`. Кнопки Enable/Disable требуют отдельного browser confirmation и передают `{"operation":"enable|disable","confirm":true}` через прежний loopback-only HTTP API `/api/mqtt/local-broker` (GET для status, POST для mutations).

Save/Review/Apply MQTT Connection **не вызывают** эту системную операцию. Сбой локального брокера не завершает Runtime/NPU/voice. Возврат с неопределённой сетью не считается успехом — требуется повторно прочитать статус.

Admin не запускает shell, `sudo` или `systemctl`; он пишет только один из фиксированных socket-verbs `STATUS/ENABLE/DISABLE` в `/run/firefly-mqtt-control/control.sock`. Клиент принимает ответ только от проверенного UID root. Сервисное управление выполняет **другой процесс** `firefly-mqtt-control-helper` через заранее установленную socket-activated unit. Доступ к сокету допускается только группе `firefly`; сам helper дополнительно сверяет `SO_PEERCRED.uid` с реальным UID `firefly`. Допустимые операции жёстко ограничены одной systemd unit `firefly-mqtt-broker.service`. Helper вызывает `/usr/bin/systemctl` **без shell и произвольных аргументов**, с отдельным timeout и проверяет последующий реальный `is-enabled/is-active`. Неподтверждённое изменение возвращает ERROR, не Success. Для `enable` используется `Type=exec` и дополнительная короткая проверка после запуска, чтобы быстро упавший из-за ошибочного конфига Mosquitto не показывался как Running. Это **не** гарантия долгосрочной работоспособности: поздняя ошибка обнаруживается отдельным GET status. API/client имеют ограниченные, но достаточно длинные тайм-ауты для последовательности системных запросов; при сетевом истечении срока результат неизвестен, и UI требует перечитать реальный статус.

Управляющий сокет может быть включён только при отдельной подготовке образа. **Брокер остаётся disabled/stopped по умолчанию**, даже если управляющий сокет активирован.

## 2. Offline staging (не установка)

Оператор в тестовом PC-каталоге может подготовить **только файлы**:

```sh
python3 packaging/mqtt/stage-local-mqtt.py \
  --output /tmp/firefly-mqtt-new-staging \
  --lan-ip 192.168.1.50
```

IP в примере — **заменяемый пример**. Требуется реально назначенный Firefly постоянный IPv4 из RFC1918; разрешены только 10/8, 172.16/12, 192.168/16. Публичные, wildcard, Tailscale/100.64/10 и loopback адреса для LAN-listener не принимаются. Никогда не угадывать LAN IP автоматически; адрес должен быть закреплён DHCP reservation или статически. При смене IP придётся повторно подготовить конфиг и выполнить разрешённую перезагрузку брокера.

Скрипт не выполняет `systemctl`, не создаёт accounts/passwords, не ставит пакет и не трогает `/etc` хоста. Он создаёт **новый** каталог с шаблонами unit, явно подставленным LAN-listener и SHA256 manifest; существующий каталог перезаписать нельзя.

`packaging/mqtt/mosquitto.conf.template` содержит только два listener: `127.0.0.1:1883` и явный RFC1918 LAN IPv4:1883. `allow_anonymous false`, `password_file /etc/firefly/mqtt/passwords`, persistence OFF, без wildcard/WAN listener. Никаких default passwords в Git.

## 3. Сервисные аккаунты и доступ к файлам

После **отдельного согласования настоящего provisioning** образ должен предоставить:
- системного пользователя `mosquitto` без интерактивного входа и отдельную `firefly` identity (voice/admin unit уже использует `User=firefly`);
- `/etc/firefly/mqtt/`: root:mosquitto, режим `0750`; конфиг root:mosquitto `0640`, парольный Mosquitto файл root:mosquitto `0640`;
- `/var/lib/firefly/mqtt/credentials/`: firefly:firefly, `0700`, секреты Firefly-клиента — `0600` (существующие `0400` допустимы);
- именно два MQTT broker-account на steady-state: `firefly_svc` (Firefly executor) и один `firefly_iot` (сторонние реле/датчики). Не предоставлять встроенные admin/anonymous аккаунты;
- парольный файл брокера генерируется интерактивным предустановленным `mosquitto_passwd`: сначала `mosquitto_passwd -c /etc/firefly/mqtt/passwords firefly_svc`, затем `mosquitto_passwd /etc/firefly/mqtt/passwords firefly_iot`. Не использовать ключ `-b` с паролем в argv, не хранить plaintext в script/environment/history. Доступ по сети должен быть включён **только после** появления обоих credentials.

Для Firefly client во вкладке «Учётные данные MQTT» создаётся защищённая ссылка на пользователя `firefly_svc` с тем же паролем, затем MQTT Connection выбирает этот `credentials_id`. Пароль IoT пользователь получает от оператора *однократно* через согласованный безопасный канал для настройки устройств. Web Admin не администрирует сторонние broker accounts, не возвращает существующие passwords и не создаёт учётные записи systemd.

**Ротация пароля:** штатная операция #34 меняет **только** клиентский секрет Firefly; она не меняет хеш в Mosquitto. Перед ротацией спланировать атомарную замену password file брокера, перезагрузку учётных данных в отдельном окне, контроль подключения Firefly/IoT, резервный rollback предыдущего файла и повторного Apply при необходимости. *Автоматически* переотправлять команды устройств или перезапускать voice runtime запрещено. Полностью безопасная broker-side credential migration / rollback остаётся критерием #36/#37; до проверки нельзя заявить ZERO-DOWNTIME ротацию.

### Sandbox существующего voice unit

Текущий production `firefly-voice-assistant.service` использует `ProtectSystem=strict` и `ReadWritePaths=/var/lib/firefly/store`. Для записи MQTT credential файлов потребуется отдельное owner-reviewed расширение доступа в **#37** (`/var/lib/firefly/mqtt/credentials`), не автоматическое изменение этого service unit здесь. Нельзя чинить ошибку прав запуском voice/admin как root.

## 4. Offline/systemd tests и ответственность владельца

Тестовые `MqttLocalBrokerControl` и `MqttLocalBrokerStaging` должны работать **без root**: standalone test helper принимает фиктивный systemd socket FD и вызывает тестовый `systemctl` из временного каталога. Его test-only compile flag недоступен production helper. Negative-тесты: чужое название unit, restart, дополнительные аргументы, неполный request, неподтверждённый enable, controller недоступен, absent Mosquitto, повторные запросы и подтверждённые transitions.

Для реального production activation нужно отдельно авторизовать установку/broker credentials и разрешить системное окно. **Никаких** enable/start/disable на Firefly этим набором изменений не выполняется. Перед релизом #36/#37 требуется проверить настоящие systemd/failure/reboot transitions в изолированном Linux VM/namespace, Chromium через реальный HttpServer, права UID и staging/package linkage (в том числе aarch64).

Если helper socket отсутствует, API возвращает ошибку `mqtt_broker_control_unavailable`; UI обязан показывать «статус неизвестен», а не «брокер выключен» или «успешно включён». `/api/mqtt/local-broker` не выполняет hardware publish и не меняет ConfigStore revision/hash.
