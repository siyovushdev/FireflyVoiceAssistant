<script setup lang="ts">
import { useRouter } from 'vue-router';
import { onMounted, ref } from 'vue';
import { manual } from '../stores/manual.js';
import EntityEditor from './EntityEditor.vue';
import ConnectionWizard from './ConnectionWizard.vue';
import TaskErrors from './forms/TaskErrors.vue';
import type { Change } from '../adapters/editing.js';
const dialog = ref<HTMLDialogElement>();
const router = useRouter();
onMounted(() => dialog.value?.showModal());
function close() { if (!manual.busy.value) { if (manual.task.value && !confirm('Отбросить изменения этой задачи? Ранее принятый план сохранится.')) return; manual.cancel(); manual.showReview.value = false; } }
function explain(change: Change) { return { before: change.beforeText, after: change.afterText }; }
async function locate(path: string) {
    if (path === '/import' && !manual.task.value) { manual.showReview.value = false; await router.push('/advanced'); return; }
    const parts = path.split('/'); const list = parts[1], index = Number(parts[2]);
    const config = manual.task.value?.buffer ?? manual.draft.value;
    if (!config) return;
    let kind: 'zone' | 'device' | 'connection' | null = null, id = '', name = '';
    if (list === 'rooms' && config.rooms[index]) { kind = 'zone'; id = config.rooms[index].id; name = config.rooms[index].name; }
    if (list === 'devices' && config.devices[index]) { kind = 'device'; id = config.devices[index].id; name = config.devices[index].name; }
    if (list === 'connections' && config.connections?.[index]) { kind = 'connection'; id = '@' + config.connections[index].id; name = config.connections[index].name; }
    if (list === 'actions' && config.actions[index]) {
        const action = config.actions[index]; const consumer = config.devices.find(d => d.action_ids.includes(action.id));
        if (consumer) { kind = 'device'; id = consumer.id; name = consumer.name; }
    }
    if (kind && id !== manual.task.value?.id) {
        if (manual.task.value) manual.issues.value = [...manual.issues.value, { code: 'other_task', path: '', message: `Общее применение блокирует другая сущность: «${name}». Завершите или отмените текущую задачу, затем откройте её редактор. Чужие данные не изменены.` }];
        else await manual.begin(kind, id);
    }
}
</script>
<template><dialog ref="dialog" class="task-dialog" @cancel.prevent="close"><header class="task-header"><div><p class="eyebrow">ЛОКАЛЬНЫЙ ПЛАН</p><h2>{{ manual.task.value ? ({ system: 'Система', zone: 'Зона', device: 'Устройство', connection: 'Подключение' })[manual.task.value.kind] : 'Проверка всех изменений' }}</h2></div><button type="button" :disabled="manual.busy.value" @click="close">Закрыть</button></header>
    <form v-if="manual.task.value" @submit.prevent="manual.task.value.kind !== 'connection' && manual.stage()"><fieldset :disabled="manual.busy.value"><ConnectionWizard v-if="manual.task.value.kind === 'connection'" :key="manual.task.value.id" /><template v-else><div class="task-content"><EntityEditor /></div><footer class="wizard-footer"><TaskErrors :issues="manual.issues.value" @locate="locate" /><div class="inline-actions"><button type="button" @click="manual.cancel()">Отмена</button><button type="submit" class="primary">{{ manual.busy.value ? 'Проверка…' : 'Добавить в план' }}</button></div></footer></template></fieldset></form>
    <template v-else><div class="task-content"><p>Будут опубликованы все перечисленные изменения, включая ранее принятые задачи. До подтверждения сохранённая и активная конфигурации не меняются.</p><p v-if="manual.busy.value" role="status">Проверка…</p><template v-if="manual.review.value"><h3>Изменения относительно сохранённой конфигурации</h3><article v-for="(change, i) in manual.changes.value" :key="i" class="form-section review-change"><h4>{{ change.title }}</h4><p><strong>Было:</strong> {{ explain(change).before }}</p><p><strong>Будет:</strong> {{ explain(change).after }}</p><p v-if="change.consumers.length" class="notice">Затронуты устройства общего объекта: {{ change.consumers.join(', ') }}</p><details v-if="manual.advanced.value"><summary>Точные данные</summary><pre>{{ JSON.stringify(change, null, 2) }}</pre></details></article><p v-if="!manual.changes.value.length">Сохранённая конфигурация уже совпадает с планом.</p><template v-if="manual.review.value.active?.hash !== manual.review.value.stored.hash"><h3>Изменения относительно активной конфигурации</h3><article v-for="(change, i) in manual.activeChanges.value" :key="i" class="form-section"><h4>{{ change.title }}</h4><p>Было: {{ explain(change).before }}</p><p>Будет: {{ explain(change).after }}</p><p v-if="change.consumers.length">Затронуты: {{ change.consumers.join(', ') }}</p></article></template><p v-if="manual.review.value.activeDiff.state_reconciliation" class="notice">Backend пересчитает совместимость свидетельств состояния. Изменённые mappings могут вернуть свойства в Unknown.</p><details v-if="manual.advanced.value"><summary>Raw diff / admission</summary><pre>{{ JSON.stringify(manual.review.value, null, 2) }}</pre></details></template><p v-if="manual.outcome.value" class="notice" role="status">{{ manual.outcome.value }}</p></div><footer class="wizard-footer"><TaskErrors :issues="manual.issues.value" @locate="locate" /><div class="inline-actions"><button type="button" :disabled="manual.busy.value" @click="manual.showReview.value = false">Назад к плану</button><button type="button" :disabled="manual.busy.value" @click="manual.prepare()">Проверить заново</button><button type="button" class="primary" :disabled="!manual.canConfirm.value" @click="manual.confirm()">Подтвердить применение</button></div></footer></template>
</dialog></template>
