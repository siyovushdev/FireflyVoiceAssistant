<script setup lang="ts">
import { computed, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { system } from '../stores/system.js';
import DeviceCard from '../components/DeviceCard.vue';
const route = useRoute(), router = useRouter();
const { devices, zones } = system;
const search = ref('');
const selectedZone = computed({ get: () => typeof route.query.zone === 'string' ? route.query.zone : '', set: zone => { void router.replace({ query: zone ? { zone } : {} }); } });
const filtered = computed(() => devices.value.filter(device => (!selectedZone.value || (selectedZone.value === 'none' ? device.locationId === null : device.locationId === selectedZone.value)) && `${device.name} ${device.location}`.toLocaleLowerCase('ru').includes(search.value.toLocaleLowerCase('ru'))));
</script>
<template>
    <p class="page-description">Оборудование системы, доступные действия и физические свидетельства состояния.</p>
    <div class="filters"><label>Поиск устройства<input v-model="search" type="search" placeholder="Название или зона"></label><label>Зона<select v-model="selectedZone"><option value="">Все зоны</option><option value="none">Без зоны</option><option v-for="zone in zones" :key="zone.id" :value="zone.id">{{ zone.path }}</option></select></label><span class="result-count">{{ filtered.length }} устройств</span></div>
    <div v-if="filtered.length" class="device-grid"><DeviceCard v-for="device in filtered" :key="device.id" :device="device" /></div>
    <div v-else class="empty-panel"><h2>Устройства не найдены</h2><p>Измените фильтр или проверьте доступность конфигурации.</p></div>
</template>
