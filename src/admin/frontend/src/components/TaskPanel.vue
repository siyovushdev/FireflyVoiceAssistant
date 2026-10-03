<script setup lang="ts">
import { useRouter } from 'vue-router';
import { onMounted, ref, nextTick } from 'vue';
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
function targetFor(path: string) {
    const parts = path.split('/'); const index = Number(parts[2]);
    const config = manual.draft.value;
    if (!config) return null;
    if (parts[1] === 'rooms' && config.rooms[index]) return { kind: 'zone' as const, id: config.rooms[index].id };
    if (parts[1] === 'devices' && config.devices[index]) return { kind: 'device' as const, id: config.devices[index].id };
    if (parts[1] === 'connections' && config.connections?.[index]) return { kind: 'connection' as const, id: '@' + config.connections[index].id };
    if (parts[1] === 'actions' && config.actions[index]) {
        const device = config.devices.find(d => d.action_ids.includes(config.actions[index].id));
        if (device) return { kind: 'device' as const, id: device.id };
    }
    return null;
}
function canLocate(path: string) { return !manual.task.value && (path === '/import' || !!targetFor(path)); }
async function locate(path: string) {
    if (!canLocate(path)) return;
    if (path === '/import') { manual.showReview.value = false; await router.push('/advanced'); return; }
    const target = targetFor(path);
    if (!target) return;
    await manual.begin(target.kind, target.id);
    await nextTick();
    const controls = [...(dialog.value?.querySelectorAll<HTMLElement>('[data-path]') ?? [])];
    const control = controls.find(node => node.dataset.path === path) ?? controls.find(node => node.dataset.path?.startsWith(path + '/'));
    control?.focus(); control?.scrollIntoView({ block: 'center' });
}
</script>
<template><dialog ref="dialog" class="task-dialog" @cancel.prevent="close"><header class="task-header"><div><p class="eyebrow">ЛОКАЛЬНЫЙ ПЛАН</p><h2>{{ manual.task.value ? ({ system: 'Система', zone: 'Зона', device: 'Устройство', connection: 'Подключение' })[manual.task.value.kind] : 'Проверка всех изменений' }}</h2></div><button type="button" :disabled="manual.busy.value" @click="close">Закрыть</button></header>
    <form v-if="manual.task.value" @submit.prevent="manual.task.value.kind !== 'connection' && manual.stage()"><fieldset :disabled="manual.busy.value"><ConnectionWizard v-if="manual.task.value.kind === 'connection'" :key="manual.task.value.id" /><template v-else><div class="task-content"><EntityEditor /></div><footer class="wizard-footer"><TaskErrors :issues="manual.issues.value" :can-locate="canLocate" @locate="locate" /><div class="inline-actions"><button type="button" @click="manual.cancel()">Отмена</button><button type="submit" class="primary">{{ manual.busy.value ? 'Проверка…' : 'Добавить в план' }}</button></div></footer></template></fieldset></form>
    <template v-else><div class="task-content"><p>Будут опубликованы все перечисленные изменения, включая ранее принятые задачи. До подтверждения сохранённая и активная конфигурации не меняются.</p><p v-if="manual.busy.value" role="status">Проверка…</p><template v-if="manual.review.value"><h3>Изменения относительно сохранённой конфигурации</h3><article v-for="(change, i) in manual.changes.value" :key="i" class="form-section review-change"><h4>{{ change.title }}</h4><p><strong>Было:</strong> {{ explain(change).before }}</p><p><strong>Будет:</strong> {{ explain(change).after }}</p><p v-if="change.consumers.length" class="notice">Затронуты устройства общего объекта: {{ change.consumers.join(', ') }}</p><details v-if="manual.advanced.value"><summary>Точные данные</summary><pre>{{ JSON.stringify(change, null, 2) }}</pre></details></article><p v-if="!manual.changes.value.length">Сохранённая конфигурация уже совпадает с планом.</p><template v-if="manual.review.value.active?.hash !== manual.review.value.stored.hash"><h3>Изменения относительно активной конфигурации</h3><article v-for="(change, i) in manual.activeChanges.value" :key="i" class="form-section"><h4>{{ change.title }}</h4><p>Было: {{ explain(change).before }}</p><p>Будет: {{ explain(change).after }}</p><p v-if="change.consumers.length">Затронуты: {{ change.consumers.join(', ') }}</p></article></template><p v-if="manual.review.value.activeDiff.state_reconciliation" class="notice">После изменения подключения состояние может стать неизвестным. Прочитайте его заново с устройства.</p><details v-if="manual.advanced.value"><summary>Raw diff / admission</summary><pre>{{ JSON.stringify(manual.review.value, null, 2) }}</pre></details></template><p v-if="manual.outcome.value" class="notice" role="status">{{ manual.outcome.value }}</p></div><footer class="wizard-footer"><TaskErrors :issues="manual.issues.value" :can-locate="canLocate" @locate="locate" /><div class="inline-actions"><button type="button" :disabled="manual.busy.value" @click="manual.showReview.value = false">Назад к плану</button><button type="button" :disabled="manual.busy.value" @click="manual.prepare()">Проверить заново</button><button type="button" class="primary" :disabled="!manual.canConfirm.value" @click="manual.confirm()">Подтвердить применение</button></div></footer></template>
</dialog></template>
