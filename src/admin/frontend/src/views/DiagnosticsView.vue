<script setup lang="ts">
import { system } from '../stores/system.js';
import CommandTrace from '../components/CommandTrace.vue';
</script>
<template><section class="panel"><h2>Последняя команда</h2><CommandTrace :record="system.status.value" :configuration="system.configuration.value" /></section><section class="panel"><h2>Статус и сборка</h2><pre>{{ JSON.stringify(system.status.value, null, 2) }}</pre></section><section class="panel"><h2>Последние исполнения</h2><p>Запись исполнения и transport ACK не подтверждают физическое состояние.</p><pre>{{ JSON.stringify(system.executions.value, null, 2) }}</pre></section></template>
