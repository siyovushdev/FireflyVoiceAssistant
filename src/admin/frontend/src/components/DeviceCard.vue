<script setup lang="ts">
import type { DeviceView } from '../adapters/catalog.js';
import PropertyEvidence from './PropertyEvidence.vue';
import DeviceAvailability from './DeviceAvailability.vue';
defineProps<{ device: DeviceView }>();
</script>
<template>
    <article class="device-card" :data-device-id="device.id">
        <div class="card-top"><span class="device-glyph" aria-hidden="true">◇</span><span class="caption">{{ device.enabled ? 'Устройство' : 'Отключено в конфигурации' }}</span></div>
        <h2><RouterLink :to="`/devices/${device.id}`">{{ device.name }} <span aria-hidden="true">↗</span></RouterLink></h2>
        <p class="location">{{ device.location }}</p>
        <DeviceAvailability :device="device" />
        <PropertyEvidence v-for="property in device.properties.slice(0, 2)" :key="property.id" :property="property" />
        <p v-if="!device.properties.length" class="muted">Свойства состояния не заданы</p>
        <p v-if="device.properties.length > 2" class="muted">Ещё свойств: {{ device.properties.length - 2 }}</p>
        <div class="card-bottom"><span>{{ device.connection }}</span><small>{{ device.reachability }}</small></div>
    </article>
</template>
