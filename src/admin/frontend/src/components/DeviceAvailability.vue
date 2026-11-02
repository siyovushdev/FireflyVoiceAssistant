<script setup lang="ts">
import type { DeviceView } from '../adapters/catalog.js';
import { formatAge } from '../adapters/catalog.js';
defineProps<{ device: DeviceView }>();
</script>
<template>
    <div class="device-availability" :data-availability="device.availability">
        <span class="evidence" :class="device.availability === 'online' ? 'confirmed' : device.availability === 'offline' ? 'stale' : 'unknown'">
            <span class="evidence-dot"></span>{{ device.availability === 'online' ? 'Онлайн' : device.availability === 'offline' ? 'Нет связи' : 'Доступность неизвестна' }}
        </span>
        <p class="muted">Данные от устройства: {{ formatAge(device.observationAge) }}</p>
        <small v-if="device.availability === 'offline'">{{ device.availabilityReason === 'observation_disconnected' ? 'Потеряно подключение для чтения состояния. Проверьте также связь с брокером.' : 'Нет свежих данных больше 60 секунд. Это не означает, что питание выключено.' }}</small>
    </div>
</template>
