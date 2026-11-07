<script setup lang="ts">
import { manual } from '../stores/manual.js';
import { consumers, connectionEndpoint } from '../adapters/editing.js';
import MqttCredentialsPanel from '../components/MqttCredentialsPanel.vue';
import LocalMqttBrokerPanel from '../components/LocalMqttBrokerPanel.vue';
</script>
<template><p>Подключения HTTP, MQTT, Modbus TCP/RTU и использующие их устройства. Наличие настройки не означает доступность оборудования.</p><button :disabled="manual.busy.value" @click="manual.begin('connection', '@')">Создать подключение</button><div class="zone-grid"><article v-for="connection in manual.draft.value?.connections" :key="connection.id" class="panel"><h2>{{ connection.name }}</h2><p>{{ connectionEndpoint(connection) }}</p><p>Устройства: {{ consumers(manual.draft.value!, connection.id).join(', ') || 'нет' }}</p><button  :disabled="!['http', 'mqtt', 'modbus_tcp', 'modbus_rtu'].includes(connection.type)" @click="manual.begin('connection', '@' + connection.id)">Редактировать общее подключение</button><p v-if="connection.type === 'mqtt'">Учётные данные MQTT настраиваются отдельно ниже.</p></article></div><MqttCredentialsPanel /><LocalMqttBrokerPanel /></template>
