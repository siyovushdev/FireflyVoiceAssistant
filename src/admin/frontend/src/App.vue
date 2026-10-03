<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue';
import { issueMessage } from './adapters/manualValidation.js';
import TaskPanel from './components/TaskPanel.vue';
import { manual } from './stores/manual.js';
import { useRoute } from 'vue-router';
import { navigation } from './router/index.js';
import { system } from './stores/system.js';
import { useRefresh } from './composables/useRefresh.js';
import { formatTime } from './adapters/catalog.js';

const route = useRoute();
const { configuration, stored, active, errors, loading, updatedAt, pending, stateMismatch, health } = system;
useRefresh();
const leave = (event: BeforeUnloadEvent) => { if (manual.dirty.value) { event.preventDefault(); event.returnValue = ''; } };
onMounted(() => window.addEventListener('beforeunload', leave));
onUnmounted(() => window.removeEventListener('beforeunload', leave));
const focusMain = () => document.getElementById('main-content')?.focus();
</script>

<template>
    <a class="skip-link" href="#main-content" @click.prevent="focusMain">К содержимому</a>
    <div class="app-shell">
        <aside class="sidebar">
            <RouterLink class="brand" to="/overview" aria-label="Firefly — обзор системы"><span class="brand-mark">f</span> firefly<span class="version">V2</span></RouterLink>
            <p class="sidebar-label">ЛОКАЛЬНАЯ СИСТЕМА</p>
            <nav aria-label="Основные разделы">
                <RouterLink v-for="item in navigation" :key="item.path" :to="item.path" :class="{ planned: item.planned }">
                    <span class="nav-number">{{ item.number }}</span><span>{{ item.title }}</span><small v-if="item.planned">скоро</small>
                </RouterLink>
            </nav>
            <div class="sidebar-foot"><span class="local-dot"></span> Работает локально<p>Данные остаются на Firefly</p><a href="/">Открыть прежний интерфейс ↗</a></div>
        </aside>
        <div class="workspace">
            <header class="topbar">
                <div><span class="eyebrow">СИСТЕМА</span><strong>{{ configuration?.system.name || 'Firefly' }}</strong></div>
                <label class="check"><input type="checkbox" v-model="manual.advanced.value">Расширенный режим</label>
            </header>
            <main id="main-content" tabindex="-1">
                <div class="page-heading"><div><p class="eyebrow">FIREFLY WORKSPACE</p><h1>{{ route.meta.title }}</h1></div>
                    <button class="refresh" :disabled="loading" @click="system.refresh()">{{ loading ? 'Обновление…' : 'Обновить данные' }} <span aria-hidden="true">↻</span></button>
                </div>
                <div v-if="Object.keys(errors).length" class="notice warning" role="alert">
                    <strong>Не все данные удалось обновить</strong><p>Последние доступные сведения могут быть устаревшими.</p>
                    <ul><li v-for="(message, endpoint) in errors" :key="endpoint">{{ message }} <small>{{ endpoint }}</small></li></ul>
                </div>
                <div v-if="pending" class="notice" data-testid="pending"><strong>Есть сохранённые, но не применённые изменения</strong><p><template v-if="manual.advanced.value">Сохранённая версия: {{ stored?.revision }}. Активная: {{ active?.revision ?? 'отсутствует' }}.</template> Требуется общий review и явное подтверждение.</p></div>
                <div v-if="stateMismatch" class="notice warning">Состояния получены для другой рабочей версии. Физические значения не отображаются до согласованного обновления.</div>
                <div class="context-line"><span class="status-dot"></span>{{ health }}<span class="context-divider">/</span><span data-testid="catalog-scope">{{ active ? (errors['/api/config/active'] ? 'Последняя известная активная конфигурация' : 'Активная конфигурация') : stored ? 'Сохранённая конфигурация · не активна' : 'Загрузка конфигурации' }}</span></div>
                <div class="manual-toolbar"><button type="button" :disabled="manual.busy.value || manual.voiceBusy.value || !!manual.task.value || manual.setup.value?.active" @click="manual.prepare()">Сохранить и применить</button><span v-if="manual.dirty.value">Есть локальный план · ещё не применён</span><p v-if="manual.outcome.value" role="status">{{ manual.outcome.value }}</p><p v-if="manual.setup.value?.active" class="notice warning">Другая сессия настройки активна. Завершите её в прежнем интерфейсе.</p><p v-if="manual.issues.value.length && !manual.task.value && !manual.showReview.value" class="notice warning">{{ manual.issues.value.map(issueMessage).join(' ') }}</p></div>
                <RouterView />
                <footer class="page-footer"><span>Firefly · Локальная автоматизация</span><span>{{ updatedAt ? `Последняя попытка обновления: ${formatTime(updatedAt)}` : 'Ожидание данных' }}</span></footer>
            </main>
        </div>
    </div>
    <TaskPanel v-if="manual.task.value || manual.showReview.value" />
</template>
