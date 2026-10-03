<script setup lang="ts">
import { manual } from '../stores/manual.js';
import { consumers } from '../adapters/editing.js';
</script>
<template><p>Общие HTTP endpoints и использующие их устройства. Наличие настройки не означает доступность оборудования.</p><button @click="manual.begin('connection', '@')">Создать подключение</button><div class="zone-grid"><article v-for="connection in manual.draft.value?.connections" :key="connection.id" class="panel"><h2>{{ connection.name }}</h2><p>{{ connection.http?.base_url }}{{ connection.http?.base_path }}</p><p>Устройства: {{ consumers(manual.draft.value!, connection.id).join(', ') || 'нет' }}</p><button :disabled="connection.type !== 'http'" @click="manual.begin('connection', '@' + connection.id)">Редактировать общее подключение</button><p v-if="connection.type !== 'http'">Транспорт запланирован; исполнение недоступно.</p></article></div></template>
