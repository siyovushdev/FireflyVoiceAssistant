<script setup lang="ts">
import { ref } from 'vue';
import { adminRequest } from '../api/admin.js';
import { system } from '../stores/system.js';
import CommandTrace from '../components/CommandTrace.vue';
const speechMessage = ref('');
async function stopSpeech() {
    try { await adminRequest('/api/voice/stop', {}); speechMessage.value = 'Запрошена остановка речи. Команды устройствам не отменяются и не повторяются.'; }
    catch { speechMessage.value = 'Не удалось запросить остановку речи.'; }
}
</script>
<template><section class="panel"><h2>Голосовые ответы · F2</h2><p>Состояние: {{ system.status.value?.speech?.state ?? 'Недоступно' }}</p><p v-if="system.status.value?.speech?.error">Ошибка: {{ system.status.value.speech.error }}</p><button type="button" @click="stopSpeech">Остановить речь</button><p role="status">{{ speechMessage }}</p></section><section class="panel"><h2>Последняя команда</h2><CommandTrace :record="system.status.value" :configuration="system.configuration.value" /></section><section class="panel"><h2>Статус и сборка</h2><pre>{{ JSON.stringify(system.status.value, null, 2) }}</pre></section><section class="panel"><h2>Последние исполнения</h2><p>Запись исполнения и transport ACK не подтверждают физическое состояние.</p><pre>{{ JSON.stringify(system.executions.value, null, 2) }}</pre></section></template>
