<script setup lang="ts">
import { manual } from '../stores/manual.js';
import { system } from '../stores/system.js';
import { formatTime } from '../adapters/catalog.js';
import DeviceCard from '../components/DeviceCard.vue';
const { configuration, devices, zones, stored, active, status, executions, health } = system;
</script>
<template>
    <div class="inline-actions"><RouterLink to="/setup">Настроить с AI</RouterLink><button :disabled="manual.busy.value" @click="manual.begin('system')">Изменить название системы</button><button data-testid="voice-settings" :disabled="manual.busy.value" @click="manual.begin('system')">Голосовые ответы · F2</button><button data-testid="voice-input-settings" :disabled="manual.busy.value" @click="manual.begin('system')">Слитные команды</button></div><section class="hero-panel"><div><p class="eyebrow">ВАША СИСТЕМА, ОДНИМ ВЗГЛЯДОМ</p><h2>Устройства и их состояние</h2><p>Следите за оборудованием, зонами и последними результатами.<br>Настройка выполняется через локальный план и явное подтверждение.</p></div><span class="hero-orbit" aria-hidden="true">✳</span></section>
    <div class="metrics">
        <RouterLink to="/zones" class="metric"><span>Зоны</span><strong>{{ configuration ? zones.length : '—' }}</strong><small>Необязательная организация пространства ↗</small></RouterLink>
        <RouterLink to="/devices" class="metric"><span>Устройства</span><strong>{{ configuration ? devices.length : '—' }}</strong><small>Включая устройства без зоны ↗</small></RouterLink>
        <RouterLink to="/states" class="metric"><span>Подтверждённые свойства</span><strong>{{ configuration ? devices.flatMap(d => d.properties).filter(p => p.tone === 'confirmed').length : '—' }}</strong><small>По последним данным backend ↗</small></RouterLink>
    </div>
    <div class="section-heading"><h2>Оборудование</h2><RouterLink to="/devices">Все устройства →</RouterLink></div>
    <div v-if="devices.length" class="device-grid"><DeviceCard v-for="device in devices.slice(0, 3)" :key="device.id" :device="device" /></div>
    <div v-else class="empty-panel"><h3>{{ configuration ? 'В системе пока нет устройств' : 'Данные системы пока недоступны' }}</h3><p>Опишите систему AI или добавьте устройства вручную.</p><RouterLink to="/setup">Начать AI-настройку →</RouterLink></div>
    <div class="two-columns">
        <section class="panel"><h2>Рабочая конфигурация</h2><dl v-if="manual.advanced.value" class="facts"><dt>Сохранённая версия</dt><dd>{{ stored?.revision ?? 'Недоступна' }}</dd><dt>Активная версия</dt><dd>{{ active?.revision ?? 'Не активирована' }}</dd><dt>Состояние системы</dt><dd>{{ health }}</dd><dt>Распознавание речи</dt><dd>{{ status?.stt_ready ? 'Готово' : 'Недоступно' }}</dd></dl><p class="muted">Состояния устройств относятся к активной версии. Изменения публикуются после общего review.</p></section>
        <section class="panel"><h2>Последние действия</h2><p v-if="!executions.length" class="muted">История действий пуста или недоступна.</p><ol v-else class="event-list"><li v-for="event in [...executions].reverse().slice(0, 4)" :key="event.sequence"><strong>{{ configuration?.devices.find(d => d.id === event.execution.targetId)?.name ?? 'Устройство из прежней версии' }}</strong><span>{{ event.execution.success ? 'Команда принята' : 'Ошибка выполнения' }} · {{ event.source === 'admin-test' ? 'Виртуальная проверка' : event.source === 'voice' || event.source === 'admin-production' || event.source === 'admin-device' ? 'Рабочее выполнение' : event.source }}</span><small>{{ formatTime(event.timestamp_ms) }} · событие не подтверждает физическое состояние</small></li></ol></section>
    </div>
</template>
