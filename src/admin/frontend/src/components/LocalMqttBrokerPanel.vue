<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { adminRequest, issuesOf } from '../api/admin.js';

interface BrokerState {
  readonly ok: true;
  readonly installed: boolean;
  readonly enabled: boolean;
  readonly running: boolean;
  readonly scope: 'local_mosquitto_only';
}
const status = ref<BrokerState | null>(null);
const busy = ref(false);
const error = ref('');
const notice = ref('');
async function refresh() {
  if (busy.value) return;
  busy.value = true;
  // A status refresh supersedes any earlier Enable/Disable success.
  // If GET fails, status is unknown: never show an old success claim.
  notice.value = '';
  try {
    status.value = await adminRequest<BrokerState>('/api/mqtt/local-broker');
    error.value = '';
  } catch (e) {
    status.value = null;
    error.value = issuesOf(e).map(i => i.message).join(' ');
  } finally {
    busy.value = false;
  }
}
async function change(enable: boolean) {
  if (busy.value || !status.value || (enable && !status.value.installed)) return;
  const message = enable
    ? 'Включить локальный Mosquitto? Служба будет запускаться при перезагрузке. Это отдельная операция, не Apply конфигурации.'
    : 'Отключить локальный Mosquitto и запретить его автозапуск? Устройства на этом брокере потеряют соединение.';
  if (!window.confirm(message)) return;
  busy.value = true;
  notice.value = '';
  error.value = '';
  try {
    const next = await adminRequest<BrokerState>(
      '/api/mqtt/local-broker', { operation: enable ? 'enable' : 'disable', confirm: true });
    status.value = next;
    notice.value = enable
      ? 'Mosquitto включён и работает. При перезагрузке запуск разрешён.'
      : 'Mosquitto остановлен и автозапуск отключён.';
  } catch (e) {
    // A network timeout is indeterminate, never claim success until a fresh
    // status request has actually completed.
    status.value = null;
    error.value = issuesOf(e).map(i => i.message).join(' ') +
      ' Перечитайте состояние службы — операция могла завершиться.';
  } finally {
    busy.value = false;
  }
}
onMounted(() => { void refresh(); });
</script>

<template>
  <section class="panel" aria-labelledby="local-mqtt-broker-title">
    <h2 id="local-mqtt-broker-title">Локальный брокер Mosquitto</h2>
    <p>Установка Mosquitto выполняется заранее при подготовке Firefly. Включение и отключение ниже — отдельные подтверждаемые системные операции. Сохранение MQTT-подключения само службу не запускает.</p>
    <p v-if="status" role="status">
      Установлен: <strong>{{ status.installed ? 'да' : 'нет' }}</strong> ·
      Автозапуск: <strong>{{ status.enabled ? 'разрешён' : 'отключён' }}</strong> ·
      Служба: <strong>{{ status.running ? 'работает' : 'не работает' }}</strong>
    </p>
    <p v-else class="muted">Статус службы неизвестен — подтвердить её состояние пока не удалось.</p>
    <div class="inline-actions">
      <button type="button" :disabled="busy" @click="refresh">Проверить статус</button>
      <button type="button" :disabled="busy || !status?.installed || (status.enabled && status.running)"
              @click="change(true)">Включить Mosquitto</button>
      <button type="button" :disabled="busy || !status || (!status.enabled && !status.running)"
              @click="change(false)">Отключить Mosquitto</button>
    </div>
    <p v-if="notice" role="status">{{ notice }}</p>
    <p v-if="error" role="alert" class="notice warning">{{ error }}</p>
    <p class="notice">Устройства подключаются к IP-адресу Firefly в вашей локальной сети на порту 1883 (после отдельной настройки LAN listener). 127.0.0.1 подходит только самому Firefly. Узнайте LAN IPv4 в настройках сети Firefly; не используйте публичный адрес.</p>
    <p class="muted">Должны быть созданы два аккаунта: сервисный Firefly и один общий IoT-пользователь для устройств. Анонимный доступ запрещён. Пароль и конфигурация локального брокера готовятся отдельно; эта панель не устанавливает пакеты и не создаёт ACL.</p>
  </section>
</template>
