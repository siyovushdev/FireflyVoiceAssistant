<script setup lang="ts">
import { system } from '../stores/system.js';
const { zones, devices } = system;
</script>
<template>
    <p class="page-description">Зоны помогают организовать оборудование. Для отдельного устройства зона не обязательна.</p>
    <div class="zone-grid"><article v-for="zone in zones" :key="zone.id" class="panel zone-card"><span class="zone-icon" aria-hidden="true">▦</span><p class="eyebrow">{{ zone.parentId ? 'ВЛОЖЕННАЯ ЗОНА' : 'ЗОНА' }}</p><h2>{{ zone.name }}</h2><p class="muted">{{ zone.path }}</p><p v-if="zone.description">{{ zone.description }}</p><RouterLink :to="{ path: '/devices', query: { zone: zone.id } }">Устройств: {{ zone.deviceCount }} →</RouterLink></article>
        <article class="panel zone-card unassigned"><span class="zone-icon" aria-hidden="true">◇</span><p class="eyebrow">САМОСТОЯТЕЛЬНОЕ ОБОРУДОВАНИЕ</p><h2>Без зоны</h2><p class="muted">Устройства напрямую в системе.</p><RouterLink :to="{ path: '/devices', query: { zone: 'none' } }">Устройств: {{ devices.filter(d => d.locationId === null).length }} →</RouterLink></article>
    </div>
    <p v-if="!zones.length" class="muted">Зоны ещё не созданы или конфигурация недоступна.</p>
</template>
