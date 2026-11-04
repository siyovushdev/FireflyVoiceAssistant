<script setup lang="ts">
import ContextualAssist from './ContextualAssist.vue';
import { computed } from 'vue';
import { manual } from '../stores/manual.js';
import { system } from '../stores/system.js';
import { allocate, newField, initial, type Action } from '../adapters/editing.js';
import FieldSchema from './forms/FieldSchema.vue';
import LineListInput from './forms/LineListInput.vue';
import ScalarInput from './forms/ScalarInput.vue';
const task = computed(() => manual.task.value!);
const config = computed(() => task.value.buffer);
const zone = computed(() => config.value.rooms.find(z => z.id === task.value.id));
const device = computed(() => config.value.devices.find(d => d.id === task.value.id));
const actions = computed(() => config.value.actions.filter(a => device.value?.action_ids.includes(a.id)));
const path = computed(() => task.value.kind === 'zone' ? '/rooms/' + config.value.rooms.findIndex(z => z.id === task.value.id) : '/devices/' + config.value.devices.findIndex(d => d.id === task.value.id));
function addField() { const fields = device.value!.state ??= []; let n = 1; while (fields.some(f => f.id === `value_${n}`)) n++; const field = newField(`value_${n}`); field.initial = initial(field); fields.push(field); }
function addAction() { const id = allocate(config.value, 'action'); const a: Action = { id, name: 'Новое действие', operation: 'mock.assign.v1', enabled: true, description: '', speech_aliases: [], parameters: [], assignments: [] }; config.value.actions.push(a); device.value!.action_ids.push(id); }
function removeAction(action: Action) { device.value!.action_ids = device.value!.action_ids.filter(id => id !== action.id); config.value.bindings = config.value.bindings.filter(b => b.target_id !== device.value!.id || b.action_id !== action.id); if (!config.value.devices.some(d => d.action_ids.includes(action.id))) config.value.actions = config.value.actions.filter(a => a.id !== action.id); }
function remove() {
    if (zone.value) config.value.rooms = config.value.rooms.filter(z => z.id !== zone.value!.id);
    else if (device.value) { const id = device.value.id; config.value.devices = config.value.devices.filter(d => d.id !== id); config.value.bindings = config.value.bindings.filter(b => b.target_id !== id); config.value.observations = config.value.observations?.filter(o => o.target_id !== id); }
}
const removed = computed(() => task.value.kind === 'zone' ? !zone.value : task.value.kind === 'device' ? !device.value : false);
function setResponses(value: string) {
    if (value !== 'off' && value !== 'brief' && value !== 'detailed') return;
    config.value.system.voice_dialogue = { responses: value, questions_enabled: config.value.system.voice_dialogue?.questions_enabled ?? false };
}
function setQuestions(value: boolean) {
    config.value.system.voice_dialogue = { responses: config.value.system.voice_dialogue?.responses ?? 'off', questions_enabled: value };
}
const inlineCommands = computed(() => config.value.system.voice_input?.inline_commands ?? system.status.value?.voice_input?.inline_commands ?? false);
function setInlineCommands(value: boolean) {
    config.value.system.voice_input = { inline_commands: value };
}
</script>
<template>
    <template v-if="task.kind === 'system'"><label>Название системы<input v-model="config.system.name" required data-path="/system/name" autofocus></label>
        <h3>Ввод голосовых команд</h3>
        <label><input type="checkbox" :checked="inlineCommands" data-testid="inline-commands" @change="setInlineCommands(($event.target as HTMLInputElement).checked)">Разрешить слитные команды</label>
        <p v-if="inlineCommands">Можно сказать «Амина, включи свет» одной фразой. Если сказать только «Амина», ассистент подаст бип и будет ждать команду. В шумном помещении чужая речь может попасть в продолжение команды.</p>
        <p v-else>Сначала скажите «Амина», дождитесь бипа и произнесите команду. Этот режим удобнее в шумном помещении.</p>
        <p>Режим изменится после «Сохранить и применить», без перезапуска.</p>
        <p v-if="system.status.value?.voice_input?.confirmation_enabled">Фильтр ложных пробуждений включён и работает в обоих режимах.</p>
        <h3>Голос Амины · F2</h3>
        <label>Голосовые ответы<select :value="config.system.voice_dialogue?.responses ?? 'off'" data-testid="voice-responses" @change="setResponses(($event.target as HTMLSelectElement).value)"><option value="off">Выключено</option><option value="brief">Кратко</option><option value="detailed">Подробно</option></select></label>
        <label><input type="checkbox" :checked="config.system.voice_dialogue?.questions_enabled ?? false" data-testid="voice-questions" @change="setQuestions(($event.target as HTMLInputElement).checked)">Вопросы о работе ассистента и устройств</label>
        <p>Например: «Что сейчас включено?», «Что ты сделала последним?», «А на кухне?». После подтверждённого выполнения Амина молчит. Если подтверждения нет или произошла ошибка — отвечает коротко. На вопросы отвечает по данным устройства.</p>
        <p>Изменения входят в локальный план. После Review → Apply действуют без перезапуска. В режиме «Выключено» звук не воспроизводится. Общение на посторонние темы не поддерживается.</p>
    </template>
    <template v-if="zone"><label>Название зоны<input v-model="zone.name" required :data-path="path + '/name'" autofocus></label><ContextualAssist :entity="zone" kind="location" /><label>Родительская зона<select v-model="zone.metadata!.parent_id"><option :value="null">Без родительской зоны</option><option v-for="parent in config.rooms.filter(z => z.id !== zone!.id)" :key="parent.id" :value="parent.id">{{ parent.name }}</option></select></label><label>Альтернативные названия зоны<LineListInput :model-value="(zone.metadata!.aliases as string[] | undefined) ?? []" @update:model-value="zone.metadata!.aliases = $event" /></label><label>Описание зоны<textarea v-model="zone.metadata!.description"></textarea></label><label class="check"><input v-model="zone.metadata!.container" type="checkbox">Зона-группа (контейнер)</label><p class="field-example">* Обязательно для зоны, используемой в командах. Примеры ниже не заполняют поля.</p><label>Как назвать зону в команде{{ !zone.metadata?.container ? ' *' : '' }}<LineListInput v-model="zone.speech!.mentions" :aria-required="!zone.metadata?.container" :data-path="path + '/speech/mentions'" aria-describedby="zone-mentions-example" /></label><small id="zone-mentions-example" class="field-example">Пример: цех</small><label>Как указать местоположение{{ !zone.metadata?.container ? ' *' : '' }}<LineListInput v-model="zone.speech!.locations" :aria-required="!zone.metadata?.container" :data-path="path + '/speech/locations'" aria-describedby="zone-locations-example" /></label><small id="zone-locations-example" class="field-example">Пример: в цехе</small></template>
    <template v-if="device"><label>Название устройства<input v-model="device.name" required :data-path="path + '/name'" autofocus></label><ContextualAssist :entity="device" kind="device" /><label>Зона<select v-model="device.room_id"><option :value="null">Без зоны</option><option v-for="z in config.rooms" :key="z.id" :value="z.id">{{ z.name }}</option></select></label><label>Как обращаться к устройству, по одной форме в строке<LineListInput v-model="device.spoken_aliases" aria-describedby="device-alias-example" :data-path="path + '/spoken_aliases'" /></label><small id="device-alias-example" class="field-example">Проверьте название и форму после «включи / выключи»: розетка, розетку; вентилятор. Без нужной речевой формы команда будет отклонена.</small>
        <label>Описание устройства<textarea v-model="device.metadata!.description"></textarea></label>
        <p v-if="task.isNew" class="notice">Начальный набор: только Включить / Выключить и Питание. Возможности можно изменить ниже.</p>
        <template v-if="device.kind === 'generic'"><h3>Свойства состояния</h3><article v-for="(field, index) in device.state" :key="index" class="form-section"><FieldSchema :field="field" state :path="path + '/state/' + index" /><button type="button" class="danger" @click="device.state!.splice(index, 1)">Удалить свойство</button></article><button type="button" @click="addField">Добавить свойство</button>
        <h3>Действия</h3><article v-for="action in actions" :key="action.id" class="form-section action-editor"><label>Название действия<input v-model="action.name" required :data-path="'/actions/' + config.actions.indexOf(action) + '/name'"></label><p v-if="config.devices.filter(d => d.action_ids.includes(action.id)).length > 1" class="notice">Общее действие: {{ config.devices.filter(d => d.action_ids.includes(action.id)).map(d => d.name).join(', ') }}</p><label>Описание<input v-model="action.description"></label><label>Фразы действия<LineListInput v-model="action.speech_aliases" /></label><ContextualAssist :entity="action" kind="action" /><h4>Параметры команды</h4><div v-for="(parameter, i) in action.parameters" :key="i" class="form-section"><FieldSchema :field="parameter" :path="'/actions/' + config.actions.indexOf(action) + '/parameters/' + i" /><button type="button" @click="action.parameters.splice(i, 1)">Удалить параметр</button></div><button type="button" @click="action.parameters.push(newField('argument_' + (action.parameters.length + 1)))">Добавить параметр</button>
        <h4>Результат действия</h4><div v-for="(assignment, i) in action.assignments" :key="i" class="form-section"><label>Свойство<select v-model="assignment.field"><option v-for="field in device.state" :key="field.id" :value="field.id">{{ field.name }}</option></select></label><label>Источник значения<select :value="assignment.argument === undefined ? 'fixed' : 'argument'" @change="($event.target as HTMLSelectElement).value === 'argument' ? (assignment.argument = action.parameters[0]?.id ?? '', delete assignment.value) : (delete assignment.argument, assignment.value = initial(device.state!.find(f => f.id === assignment.field)!))"><option value="fixed">Фиксированное значение</option><option value="argument">Параметр команды</option></select></label><label v-if="assignment.argument !== undefined">Параметр<select v-model="assignment.argument"><option v-for="parameter in action.parameters" :key="parameter.id" :value="parameter.id">{{ parameter.name }}</option></select></label><ScalarInput v-else-if="device.state?.find(f => f.id === assignment.field)" :field="device.state.find(f => f.id === assignment.field)!" v-model="assignment.value" label="После действия" /><button type="button" @click="action.assignments!.splice(i, 1)">Удалить результат</button></div><button type="button" :disabled="!device.state?.length" @click="(action.assignments ??= []).push({ field: device.state![0].id, value: initial(device.state![0]) })">Добавить результат</button><button type="button" class="danger" @click="removeAction(action)">Удалить действие у устройства</button></article><button type="button" @click="addAction">Добавить действие</button></template>
        <p v-else class="notice">Встроенная возможность сохраняется по существующему контракту. Её operation и параметры не преобразуются в generic автоматически.</p>
    </template>
    <p v-if="removed" class="notice">Удаление добавлено в локальную задачу. Зависимости проверит backend; связанные устройства и чужие изменения не удаляются автоматически.</p>
    <button v-if="!task.isNew && !removed && task.kind !== 'system'" type="button" class="danger" @click="remove">Удалить {{ task.kind === 'zone' ? 'зону' : 'устройство' }}</button>
    <p v-if="manual.advanced.value">Stable ID: <code>{{ task.id }}</code></p>
</template>
