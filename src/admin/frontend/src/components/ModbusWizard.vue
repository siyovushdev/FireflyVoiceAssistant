<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { manual } from '../stores/manual.js';
import { clone, consumers, connectionEndpoint, connectionActionSummary, connectionObservationSummary, type Mutable } from '../adapters/editing.js';
import { ModbusDeviceDrafts, newModbusConnection, modbusConnectionError, modbusMappingError, projectModbusWizard, modbusBaudRates } from '../adapters/modbus.js';
import type { BindingDto, ModbusMappingDto, ModbusTransport, ConnectionDto } from '../types/wire.js';
import type { Issue } from '../api/admin.js';
import TaskErrors from './forms/TaskErrors.vue';
const props = defineProps<{ transport: ModbusTransport }>();
const emit = defineEmits<{ ready: []; back: [] }>();
const task = manual.task.value!, config = task.buffer, transport = props.transport;
const label = transport === 'modbus_tcp' ? 'Modbus TCP' : 'Modbus RTU';
const initialId = task.id.startsWith('@') ? task.id.slice(1) :
    config.bindings.find(b => b.target_id === task.id && b.executor === transport)?.connection_id ??
    config.observations?.find(o => o.target_id === task.id && o.executor === transport)?.connection_id ?? '';
const initial = config.connections?.find(c => c.id === initialId && c.type === transport);
const selected = ref(initial?.id ?? '');
const connection = ref<Mutable<ConnectionDto>>(clone(initial ?? newModbusConnection(config, transport)));
const targetId = ref(task.id.startsWith('@') ? '' : task.id), step = ref(1), issues = ref<Issue[]>([]);
const drafts = new ModbusDeviceDrafts(config, transport), draft = ref(drafts.restore(targetId.value));
watch(targetId, (next, old) => { drafts.remember(old, draft.value); draft.value = drafts.restore(next); issues.value = []; }, { flush: 'sync' });
const device = computed(() => config.devices.find(d => d.id === targetId.value));
const actions = computed(() => config.actions.filter(a => device.value?.action_ids.includes(a.id)));
const otherObservation = computed(() => config.observations?.find(o => o.target_id === targetId.value && o.executor !== transport));
const edits = computed(() => drafts.changed(targetId.value, draft.value));
const editedDevices = computed(() => edits.value.map(([id]) => config.devices.find(d => d.id === id)?.name ?? id));
const previousConsumers = computed(() => consumers(config, connection.value.id));
function chooseConnection() { connection.value = clone(config.connections?.find(c => c.id === selected.value && c.type === transport) ?? newModbusConnection(config, transport)); }
function blockedAction(id: string) { return config.bindings.some(b => b.target_id === targetId.value && b.action_id === id && b.executor !== transport); }
function toggle(id: string, enabled: boolean) {
    if (!enabled) { draft.value.mappings = draft.value.mappings.filter(b => b.action_id !== id); return; }
    const action = actions.value.find(a => a.id === id);
    if (blockedAction(id) || !action) return;
    const assignment = action.assignments?.find(a => a.field === 'power');
    draft.value.mappings.push({ target_id: targetId.value, action_id: id, executor: transport, connection_id: connection.value.id,
        modbus: { function: 5, address: 0, data_type: 'boolean', value: typeof assignment?.value === 'boolean' ? assignment.value : false } });
}
function changeFunction(mapping: Mutable<ModbusMappingDto>, read = false) {
    mapping.data_type = [1, 2, 5].includes(mapping.function) ? 'boolean' : 'uint16';
    if (read) mapping.field = '';
    else { delete mapping.argument; mapping.value = mapping.data_type === 'boolean' ? false : 0; }
}
function argumentMode(mapping: Mutable<ModbusMappingDto>, value: string) {
    if (value === 'argument') { delete mapping.value; mapping.argument = ''; }
    else { delete mapping.argument; mapping.value = mapping.data_type === 'boolean' ? false : 0; }
}
function parameters(mapping: BindingDto) { return config.actions.find(a => a.id === mapping.action_id)?.parameters.filter(p => p.type === (mapping.modbus?.data_type === 'boolean' ? 'boolean' : 'integer')) ?? []; }
function setObserving(value: boolean) {
    if (value && !draft.value.observeConnectionId) draft.value.observeConnectionId = connection.value.id;
    draft.value.observing = value;
}
function fail(message: string) { issues.value = [{ code: 'modbus_incomplete', path: '', message }]; return false; }
function validateStep(): boolean {
    issues.value = [];
    const invalid = modbusConnectionError(connection.value); if (invalid) { step.value = 1; return fail(invalid); }
    if (step.value === 2) for (const mapping of draft.value.mappings) {
        const error = mapping.modbus ? modbusMappingError(mapping.modbus) : 'Укажите контракт действия.'; if (error) return fail(error);
    }
    if (step.value === 3 && draft.value.observing) {
        const error = modbusMappingError(draft.value.observation, true); if (error) return fail(error);
        if (otherObservation.value && !draft.value.replaceOtherObservation) return fail('Подтвердите замену чтения состояния другого протокола.');
    }
    return true;
}
function validateAll(): boolean {
    if (!validateStep()) return false;
    for (const [id, pending] of edits.value) {
        try { projectModbusWizard(config, connection.value, transport, [[id, pending]]); }
        catch (error) { targetId.value = id; step.value = 2; return fail(`${config.devices.find(d => d.id === id)?.name ?? id}: ${(error as Error).message}`); }
    }
    return true;
}
const preview = computed(() => {
    try { return projectModbusWizard(config, connection.value, transport, edits.value); }
    catch { return null; }
});
function next() { if (step.value === 3 ? validateAll() : validateStep()) step.value++; }
async function finish() {
    if (!validateAll() || !preview.value) return;
    Object.assign(config, preview.value);
    if (await manual.stage()) { emit('ready'); await manual.prepare(); }
    else step.value = manual.issues.value[0]?.path.startsWith('/observations') ? 3 : 1;
}
</script>
<template><div class="wizard" :aria-busy="manual.busy.value">
  <p class="eyebrow">{{ label }} · ШАГ {{ step }} ИЗ 4 · {{ device?.name ?? 'Общее подключение' }}</p>
  <h3>{{ ['', 'Подключение', 'Запись значения', 'Чтение состояния', 'Review Modbus'][step] }}</h3>
  <div v-if="step === 1">
    <button type="button" @click="emit('back')">← Выбор протокола</button>
    <label>Подключение<select v-model="selected" @change="chooseConnection"><option value="">Новое {{ label }}-подключение</option><option v-for="c in (config.connections ?? []).filter(c => c.type === transport)" :key="c.id" :value="c.id">{{ c.name }}</option></select></label>
    <label>Название *<input v-model="connection.name" required /></label><label class="check"><input v-model="connection.enabled" type="checkbox" />Подключение включено</label>
    <p v-if="previousConsumers.length" class="notice">Общее подключение используют: {{ previousConsumers.join(', ') }}. Изменение параметров затронет их все и будет показано в общем Review.</p>
    <template v-if="transport === 'modbus_tcp'"><label>IPv4 устройства *<input v-model="connection.network!.host" placeholder="192.168.1.20" required /></label><label>TCP-порт *<input v-model.number="connection.network!.port" type="number" min="1" max="65535" required /></label></template>
    <template v-else><label>Serial-порт Firefly *<input v-model="connection.serial!.port" placeholder="/dev/ttyUSB0" required /></label><label>Скорость, бод *<select v-model="connection.serial!.baud_rate"><option v-for="rate in modbusBaudRates" :key="rate" :value="rate">{{ rate }}</option></select></label><label>Чётность<select v-model="connection.serial!.parity"><option value="even">Even — чётная</option><option value="odd">Odd — нечётная</option><option value="none">None — нет</option></select></label><label>Стоповые биты<select v-model="connection.serial!.stop_bits"><option :value="1">1</option><option :value="2">2</option></select></label><p class="muted">Укажите отдельный адаптер Modbus RTU на Firefly. Настройка не изменяет права порта и не освобождает занятый адаптер.</p></template>
    <label>Unit ID *<input v-model.number="connection.modbus!.unit_id" type="number" min="1" max="247" required /></label><label>Тайм-аут, мс<input v-model.number="connection.timeout_ms" type="number" min="50" max="10000" required /></label>
    <p class="muted">Broadcast не поддерживается. Адреса в следующих шагах — с нуля, по карте регистров производителя.</p>
  </div>
  <div v-if="step === 2">
    <label v-if="!task.id || task.id.startsWith('@')">Устройство (необязательно)<select v-model="targetId"><option value="">Только общее подключение</option><option v-for="d in config.devices" :key="d.id" :value="d.id">{{ d.name }}</option></select></label>
    <p>Одна катушка (FC5) или один регистр (FC6) на действие. Значение и адрес задайте по документации устройства.</p>
    <article v-for="action in actions" :key="action.id" class="form-section mapping-editor">
      <label class="check"><input type="checkbox" :checked="draft.mappings.some(b => b.action_id === action.id)" :disabled="blockedAction(action.id)" @change="toggle(action.id, ($event.target as HTMLInputElement).checked)" />{{ action.name }}</label>
      <p v-if="blockedAction(action.id)" class="notice">Действие уже связано с другим протоколом. Сначала явно удалите прежнюю привязку.</p>
      <template v-for="mapping in draft.mappings.filter(b => b.action_id === action.id)" :key="mapping.action_id">
        <p v-if="mapping.connection_id !== connection.id" class="notice">Сохраняется подключение {{ config.connections?.find(c => c.id === mapping.connection_id)?.name ?? mapping.connection_id }}. <button type="button" @click="mapping.connection_id = connection.id">Перенести на выбранное подключение</button></p>
        <label>Функция<select v-model="mapping.modbus!.function" @change="changeFunction(mapping.modbus!)"><option :value="5">FC5 — записать катушку</option><option :value="6">FC6 — записать регистр</option></select></label>
        <label>Адрес с нуля *<input v-model.number="mapping.modbus!.address" type="number" min="0" max="65535" required /></label>
        <label v-if="mapping.modbus!.function === 6">Тип регистра<select v-model="mapping.modbus!.data_type"><option value="uint16">uint16 (0…65535)</option><option value="int16">int16 (−32768…32767)</option></select></label>
        <label>Источник значения<select :value="mapping.modbus!.argument !== undefined ? 'argument' : 'fixed'" @change="argumentMode(mapping.modbus!, ($event.target as HTMLSelectElement).value)"><option value="fixed">Фиксированное значение</option><option v-if="parameters(mapping).length" value="argument">Параметр команды</option></select></label>
        <label v-if="mapping.modbus!.argument !== undefined">Параметр<select v-model="mapping.modbus!.argument"><option value="">Выберите параметр</option><option v-for="parameter in parameters(mapping)" :key="parameter.id" :value="parameter.id">{{ parameter.name }}</option></select></label>
        <label v-else-if="mapping.modbus!.data_type === 'boolean'">Значение<select v-model="mapping.modbus!.value"><option :value="false">Выключено / false</option><option :value="true">Включено / true</option></select></label>
        <label v-else>Значение<input v-model.number="mapping.modbus!.value" type="number" :min="mapping.modbus!.data_type === 'int16' ? -32768 : 0" :max="mapping.modbus!.data_type === 'int16' ? 32767 : 65535" required /></label>
      </template>
    </article>
  </div>
  <div v-if="step === 3">
    <label>Чтение состояния<select :value="draft.observing" :disabled="!targetId" @change="setObserving(($event.target as HTMLSelectElement).value === 'true')"><option :value="false">Не предоставляется</option><option :value="true">Отдельное чтение Modbus</option></select></label>
    <template v-if="draft.observing">
      <p v-if="otherObservation" class="notice">Сейчас настроено чтение {{ otherObservation.executor }}. <label class="check"><input v-model="draft.replaceOtherObservation" type="checkbox" />Явно заменить прежнее чтение на {{ label }}</label></p>
      <p v-if="draft.observeConnectionId !== connection.id" class="notice">Чтение сохраняет подключение {{ config.connections?.find(c => c.id === draft.observeConnectionId)?.name ?? draft.observeConnectionId }}. <button type="button" @click="draft.observeConnectionId = connection.id">Перенести чтение на выбранное подключение</button></p>
      <label>Функция<select v-model="draft.observation.function" @change="changeFunction(draft.observation, true)"><option :value="1">FC1 — катушка</option><option :value="2">FC2 — дискретный вход</option><option :value="3">FC3 — holding register</option><option :value="4">FC4 — input register</option></select></label>
      <label>Адрес с нуля *<input v-model.number="draft.observation.address" type="number" min="0" max="65535" required /></label>
      <label v-if="draft.observation.function >= 3">Тип регистра<select v-model="draft.observation.data_type" @change="draft.observation.field = ''"><option value="uint16">uint16 (0…65535)</option><option value="int16">int16 (−32768…32767)</option></select></label>
      <label>Свойство состояния<select v-model="draft.observation.field"><option value="">Выберите свойство</option><option v-for="field in device?.state?.filter(f => f.type === (draft.observation.data_type === 'boolean' ? 'boolean' : 'integer'))" :key="field.id" :value="field.id">{{ field.name }}</option></select></label>
      <p class="muted">Читается одно значение без масштабирования. Ответ на запись сам по себе не подтверждает физическое состояние.</p>
    </template>
  </div>
  <div v-if="step === 4 && preview" class="review-connection">
    <p><strong>{{ connection.name }}</strong> · {{ connectionEndpoint(connection) }} · timeout {{ connection.timeout_ms }} мс</p>
    <p>Устройства подключения: {{ consumers(preview, connection.id).join(', ') || 'нет' }}</p>
    <p v-if="editedDevices.length">Изменены настройки устройств: {{ editedDevices.join(', ') }}</p>
    <section v-for="[id] in edits" :key="id"><h4>{{ config.devices.find(d => d.id === id)?.name }}</h4><p v-for="action in config.actions.filter(a => config.devices.find(d => d.id === id)?.action_ids.includes(a.id))" :key="action.id">{{ action.name }}: {{ connectionActionSummary(preview, id, action.id) }}</p><p>Чтение: {{ connectionObservationSummary(preview, id) }}</p></section>
    <p>Связь и оборудование ещё не проверялись. Команды и чтение не отправлялись. Далее откроется общий Review перед сохранением и Apply.</p>
  </div>
  <div class="wizard-footer"><TaskErrors :issues="[...issues, ...manual.issues.value]" /><div class="inline-actions"><button type="button" @click="manual.cancel()">Отмена</button><button v-if="step > 1" type="button" @click="step--; issues = []">Назад</button><button v-if="step < 4" type="button" class="primary" @click="next">Далее</button><button v-else type="button" class="primary" :disabled="manual.busy.value" @click="finish">К общему Review</button></div></div>
</div></template>
