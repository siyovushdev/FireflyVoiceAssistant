<script setup lang="ts">
import { ref } from 'vue';
import { adminRequest } from '../api/admin.js';
import { applyLanguage } from '../adapters/setup.js';
import { clone } from '../adapters/editing.js';
import { manual } from '../stores/manual.js';
import { setupBusy, owned, transitioning, startSetup } from '../stores/setup.js';
import type { WireObject } from '../types/wire.js';
const props = defineProps<{ entity: WireObject; kind: 'location' | 'device' | 'action' }>();
const checkpoint = ref<WireObject | null>(null), proposed = ref(false);
async function suggest(schema = false) {
    if (!owned.value || transitioning.value) return;
    await manual.run(async () => {
        const task = manual.task.value;
        if (!task || !String(props.entity.name ?? '').trim()) throw Error('Сначала укажите название.');
        const fingerprint = JSON.stringify(task.buffer), original = clone(props.entity);
        let suggestion: WireObject;
        if (schema && props.kind === 'action') {
            const configuration = clone(task.buffer);
            if (!manual.draft.value?.actions.some(a => a.id === props.entity.id)) {
                configuration.actions = configuration.actions.filter(a => a.id !== props.entity.id);
                for (const device of configuration.devices) device.action_ids = device.action_ids.filter(id => id !== props.entity.id);
            }
            const result = await adminRequest<{ suggestion: WireObject }>('/api/admin/action-draft', { configuration, target: task.id, name: props.entity.name });
            suggestion = result.suggestion;
        } else if (props.kind === 'action') {
            const result = await adminRequest<{ suggestion: WireObject }>('/api/admin/language-profile', { configuration: task.buffer, entity: 'action', id: props.entity.id });
            suggestion = result.suggestion;
        } else {
            const result = await adminRequest<{ suggestion: WireObject }>('/api/admin/draft', { entity: props.kind, name: props.entity.name });
            suggestion = result.suggestion;
        }
        if (manual.task.value !== task || fingerprint !== JSON.stringify(task.buffer)) throw Error('Форма изменилась. AI-предложение устарело и не вставлено.');
        checkpoint.value = original;
        Object.assign(props.entity, schema ? { ...original, description: suggestion.description, parameters: suggestion.parameters, assignments: suggestion.assignments, speech_aliases: suggestion.speech_aliases } : applyLanguage(original, suggestion, props.kind));
        proposed.value = true;
    }, 'ai');
}
function discard() { if (checkpoint.value) Object.assign(props.entity, clone(checkpoint.value)); checkpoint.value = null; proposed.value = false; }
</script>
<template><section class="form-section ai-assist"><p v-if="!owned">Для локального AI нужна явная сессия настройки. <button type="button" :disabled="setupBusy" @click="startSetup()">Начать настройку</button>. Ручные поля работают без AI.</p><button type="button" :disabled="!owned || transitioning || manual.busy.value || proposed" @click="suggest()">✨ Заполнить с AI</button><button v-if="kind === 'action'" type="button" :disabled="!owned || transitioning || manual.busy.value || proposed" @click="suggest(true)">Предложить схему действия с AI</button><template v-if="proposed"><p class="notice">✨ Предложено AI — проверьте перед добавлением. Значения можно редактировать; возможности устройства сохранены.</p><button type="button" @click="proposed = false; checkpoint = null">Принять AI-поля</button><button type="button" @click="discard">Отказаться от AI-полей</button></template></section></template>
