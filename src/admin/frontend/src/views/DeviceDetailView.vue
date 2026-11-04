<script setup lang="ts">
import { computed } from 'vue';
import { useRoute } from 'vue-router';
import DeviceControls from '../components/DeviceControls.vue';
import { manual } from '../stores/manual.js';
import { system } from '../stores/system.js';
import { formatTime } from '../adapters/catalog.js';
import PropertyEvidence from '../components/PropertyEvidence.vue';
import DeviceAvailability from '../components/DeviceAvailability.vue';
const route = useRoute();
const device = computed(() => system.devices.value.find(item => item.id === route.params.id));
</script>
<template><RouterLink :to="{ path: '/setup', query: { device: String($route.params.id) } }">Настроить это устройство с AI</RouterLink>
    <RouterLink class="back-link" to="/devices">← Все устройства</RouterLink>
    <template v-if="device"><div class="inline-actions"><button :disabled="manual.busy.value" @click="manual.begin('device', device.id)">Редактировать устройство</button><button :disabled="manual.busy.value" @click="manual.begin('connection', device.id)">Настроить подключение</button></div><div class="detail-heading"><span class="device-glyph large" aria-hidden="true">◇</span><div><h2>{{ device.name }}</h2><p>{{ device.location }}<span v-if="!device.enabled"> · Отключено в конфигурации</span></p></div></div><p v-if="device.description" class="page-description">{{ device.description }}</p>
        <div class="two-columns"><section class="panel"><h2>Физическое состояние</h2><PropertyEvidence v-for="property in device.properties" :key="property.id" :property="property" /><p v-if="!device.properties.length" class="muted">Свойства состояния не заданы.</p><p class="muted">Начальные значения конфигурации не являются данными оборудования.</p></section>
            <section class="panel"><h2>Подключение</h2><DeviceAvailability :device="device" /><p class="connection-summary">{{ device.connection }}</p><p v-for="name in device.connectionNames" :key="name">{{ name }}</p><p v-for="endpoint in device.endpoints" :key="endpoint">{{ endpoint }}</p><dl class="facts"><dt>Связь</dt><dd>{{ device.reachability }}</dd><dt>Последний успешный обмен</dt><dd>{{ formatTime(device.lastSuccess) }}</dd><dt>Чтение состояния</dt><dd>{{ device.observation ? 'Настроено' : 'Не настроено' }}</dd></dl><p class="muted">Настройка, доступность транспорта и физическое состояние — независимые сведения.</p></section></div>
        <DeviceControls :key="device.id" :id="device.id" /><section class="panel"><h2>Возможности устройства</h2><p class="muted">Схема действий активной конфигурации.</p><div class="capability-list"><article v-for="action in device.actions" :key="action.id"><strong>{{ action.name }}</strong><span v-if="!action.enabled" class="caption">Отключено</span><p>{{ action.description }}</p><p v-for="parameter in action.parameters" :key="parameter.id" class="muted">{{ parameter.name }}{{ parameter.unit ? ` · ${parameter.unit === 'percent' ? '%' : parameter.unit}` : '' }}</p></article></div><p v-if="!device.actions.length" class="muted">Действия ещё не заданы.</p></section>
    </template>
    <div v-else class="empty-panel"><h2>Устройство недоступно</h2><p>Его нет в отображаемой конфигурации или данные ещё загружаются.</p></div>
</template>
