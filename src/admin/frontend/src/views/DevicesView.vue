<script setup lang="ts">
import { computed, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { manual } from '../stores/manual.js';
import { system } from '../stores/system.js';
import DeviceCard from '../components/DeviceCard.vue';
const route = useRoute(), router = useRouter();
const { devices, zones } = system;
const search = ref('');
const selectedZone = computed({ get: () => typeof route.query.zone === 'string' ? route.query.zone : '', set: zone => { void router.replace({ query: zone ? { zone } : {} }); } });
const filtered = computed(() => devices.value.filter(device => (!selectedZone.value || (selectedZone.value === 'none' ? device.locationId === null : device.locationId === selectedZone.value)) && `${device.name} ${device.location}`.toLocaleLowerCase('ru').includes(search.value.toLocaleLowerCase('ru'))));
</script>
<template>
    <div class="inline-actions"><button :disabled="manual.busy.value" @click="manual.begin('device')">Создать устройство</button></div><div v-if="manual.candidate.value" class="panel"><h2>Устройства в плане</h2><div v-for="d in manual.candidate.value.devices" :key="d.id" class="inline-actions"><strong>{{ d.name }}</strong><span>{{ manual.candidate.value.rooms.find(z => z.id === d.room_id)?.name ?? 'Без зоны' }}</span><button :disabled="manual.busy.value" @click="manual.begin('device', d.id)">Редактировать</button><button :disabled="manual.busy.value" @click="manual.begin('connection', d.id)">Настроить подключение</button></div></div><p class="page-description">Оборудование системы, доступные действия и физические свидетельства состояния.</p>
    <div class="filters"><label>Поиск устройства<input v-model="search" type="search" placeholder="Название или зона"></label><label>Зона<select v-model="selectedZone"><option value="">Все зоны</option><option value="none">Без зоны</option><option v-for="zone in zones" :key="zone.id" :value="zone.id">{{ zone.path }}</option></select></label><span class="result-count">{{ filtered.length }} устройств</span></div>
    <div v-if="filtered.length" class="device-grid"><DeviceCard v-for="device in filtered" :key="device.id" :device="device" /></div>
    <div v-else class="empty-panel"><h2>Устройства не найдены</h2><p>Измените фильтр или проверьте доступность конфигурации.</p></div>
</template>
