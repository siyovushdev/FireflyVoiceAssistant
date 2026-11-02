<script setup lang="ts">
import { system } from '../stores/system.js';
import PropertyEvidence from '../components/PropertyEvidence.vue';
import DeviceAvailability from '../components/DeviceAvailability.vue';
const { devices } = system;
</script>
<template>
    <p class="page-description">Сведения от устройств обновляются на странице каждые 10 секунд. Чтение страницы не отправляет команды оборудованию. Доступность и включённое питание — разные сведения.</p>
    <div class="evidence-legend"><span class="evidence unknown">Неизвестно</span><span class="evidence predicted">Ожидается</span><span class="evidence confirmed">Подтверждено</span><span class="evidence stale">Устарело</span></div>
    <section v-for="device in devices" :key="device.id" class="panel state-row" :data-device-id="device.id"><div><h2><RouterLink :to="`/devices/${device.id}`">{{ device.name }} ↗</RouterLink></h2><p class="location">{{ device.location }}</p><DeviceAvailability :device="device" /><small>{{ device.reachability }}</small></div><div class="state-properties"><PropertyEvidence v-for="property in device.properties" :key="property.id" :property="property" /><p v-if="!device.properties.length" class="muted">Свойства состояния не заданы.</p></div></section>
    <div v-if="!devices.length" class="empty-panel"><h2>Нет данных устройств</h2><p>Состояния появятся после настройки системы и получения данных backend.</p></div>
</template>
