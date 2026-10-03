<script setup lang="ts">
import { nextTick, watch, ref } from 'vue';
import type { Issue } from '../../api/admin.js';
import { issueMessage } from '../../adapters/manualValidation.js';
import { manual } from '../../stores/manual.js';
const props = defineProps<{ issues: readonly Issue[]; canLocate?: (path: string) => boolean }>();
const summary = ref<HTMLElement>();
const host = ref<HTMLElement>();
const targets = ref<Record<string, HTMLElement>>({});
function focus(control: HTMLElement) {
    for (let parent = control.parentElement; parent; parent = parent.parentElement)
        if (parent instanceof HTMLDetailsElement) parent.open = true;
    control.focus(); control.scrollIntoView({ block: 'center' });
}
function locate(path: string) {
    const control = targets.value[path];
    if (control?.isConnected) focus(control);
    else if (props.canLocate?.(path)) emit('locate', path);
}
watch(() => [props.issues, manual.advanced.value] as const, async ([value]) => {
    await nextTick();
    const root = host.value?.closest('form, section, dialog') ?? host.value;
    if (!root) return;
    root.querySelectorAll('[data-inline-error]').forEach(node => node.remove());
    root.querySelectorAll('[aria-invalid="true"]').forEach(node => node.removeAttribute('aria-invalid'));
    targets.value = {};
    if (!value.length) return;
    let first: HTMLElement | undefined;
    for (const issue of value) {
        const controls = [...root.querySelectorAll<HTMLElement>('[data-path]')];
        const control = controls.find(node => issue.path && node.dataset.path === issue.path)
            ?? controls.filter(node => node.dataset.path && issue.path.startsWith(node.dataset.path + '/')).sort((a, b) => b.dataset.path!.length - a.dataset.path!.length)[0]
            ?? controls.find(node => issue.path && node.dataset.path?.startsWith(issue.path + '/'));
        if (control) {
            targets.value[issue.path] = control;
            control.setAttribute('aria-invalid', 'true');
            const message = document.createElement('small'); message.dataset.inlineError = 'true'; message.className = 'field-error'; message.textContent = issueMessage(issue);
            control.after(message); first ??= control;
        }
    }
    if (first) focus(first); else if (summary.value) focus(summary.value);
}, { immediate: true });
const emit = defineEmits<{ locate: [path: string] }>();
</script>
<template><div ref="host"><div v-if="issues.length" ref="summary" class="notice warning task-errors" tabindex="-1" role="alert"><strong>Задача требует внимания</strong><ul><li v-for="(issue, i) in issues" :key="i">{{ issueMessage(issue) }} <button v-if="targets[issue.path] || canLocate?.(issue.path)" type="button" class="text-button" @click="locate(issue.path)">Перейти к полю или сущности</button><details v-if="manual.advanced.value"><summary>Технические сведения</summary><pre>{{ JSON.stringify(issue, null, 2) }}</pre></details></li></ul><p>Введённые данные сохранены в этой вкладке.</p></div></div></template>
