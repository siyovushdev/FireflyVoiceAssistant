<script setup lang="ts">
import { computed } from 'vue';
import { commandTrace } from '../adapters/commandTrace.js';
import type { ConfigurationDto } from '../types/wire.js';
const props = defineProps<{ record: unknown; configuration?: ConfigurationDto | null }>();
const trace = computed(() => commandTrace(props.record, props.configuration));
</script>
<template><section v-if="trace" class="form-section" aria-label="Разбор команды">
  <h3>Разбор команды</h3>
  <p>Распознано: <strong>{{ trace.input }}</strong></p>
  <p>Обработка: <strong>{{ trace.route }}</strong></p>
  <p v-if="trace.target && trace.action">Выбрано: {{ trace.target }} → {{ trace.action }}</p>
  <p v-if="trace.reason" role="status">Причина: {{ trace.reason }}</p>
  <p v-if="trace.executed">Исполнитель принял команду. Подтверждённое состояние показано отдельно по сообщению устройства.</p>
</section></template>
