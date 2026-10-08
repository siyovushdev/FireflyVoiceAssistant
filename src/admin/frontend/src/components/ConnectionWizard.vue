<script setup lang="ts">
import { foreground } from '../stores/foreground.js';
import { computed, ref, watch } from 'vue';
import { manual } from '../stores/manual.js';
import { clone, consumers, projectConnectivity, newHttpConnection, defaultHttpMapping, type Mutable } from '../adapters/editing.js';
import type { BindingDto, ConnectionDto, HttpDto } from '../types/wire.js';
import { adminRequest, issuesOf, type Issue } from '../api/admin.js';
import HttpMappingEditor from './HttpMappingEditor.vue';
import TaskErrors from './forms/TaskErrors.vue';
const emit = defineEmits<{ ready: [] }>();
const task = manual.task.value!;
const config = task.buffer;
const step = ref(1), targetId = ref(task.id.startsWith('@') ? '' : task.id);
const device = computed(() => config.devices.find(d => d.id === targetId.value));
const actions = computed(() => config.actions.filter(a => device.value?.action_ids.includes(a.id)));
const existing = task.id.startsWith('@') ? task.id.slice(1) : config.bindings.find(b => b.target_id === task.id)?.connection_id ?? config.observations?.find(o => o.target_id === task.id)?.connection_id ?? (config.bindings.some(b => b.target_id === task.id && b.http?.url) ? '__legacy__' : '');
const selected = ref(existing);
const legacy = computed(() => selected.value === '__legacy__');
function fresh(): Mutable<ConnectionDto> { return newHttpConnection(config); }
const connection = ref<Mutable<ConnectionDto>>(clone(config.connections?.find(c => c.id === existing) ?? fresh()));
const issues = ref<Issue[]>([]), probe = ref(''), probeSnapshot = ref(''), probing = ref(false);
const fingerprint = computed(() => JSON.stringify(connection.value));
const probeStale = computed(() => !!probe.value && probeSnapshot.value !== fingerprint.value);
const mappings = ref<Mutable<BindingDto>[]>([]);
function loadMappings() { mappings.value = config.bindings.filter(b => b.target_id === targetId.value).map(clone); }
const defaultHttp = defaultHttpMapping;
function toggle(actionId: string, enabled: boolean) {
    if (!enabled) { mappings.value = mappings.value.filter(b => b.action_id !== actionId); return; }
    const assignment = actions.value.find(a => a.id === actionId)?.assignments?.find(a => a.field === 'power');
    const http = defaultHttp(); if (legacy.value) { delete http.path; http.url = ''; } if (typeof assignment?.value === 'boolean') http.body = { on: assignment.value };
    mappings.value.push({ target_id: targetId.value, action_id: actionId, ...(legacy.value ? {} : { connection_id: connection.value.id }), executor: 'http', http });
}
let oldObserve = config.observations?.find(o => o.target_id === targetId.value);
const observing = ref(!!oldObserve);
const observeConnectionId = ref(oldObserve?.connection_id ?? '');
const observeHttp = ref<Mutable<HttpDto>>(clone(oldObserve?.http ?? { ...defaultHttp(), method: 'GET' }));
const fields = ref<[string, string][]>(Object.entries(oldObserve?.fields ?? {}));
function loadDevice() {
    loadMappings();
    oldObserve = config.observations?.find(o => o.target_id === targetId.value);
    observing.value = !!oldObserve;
    observeConnectionId.value = oldObserve?.connection_id ?? '';
    observeHttp.value = clone(oldObserve?.http ?? { ...defaultHttp(), method: 'GET' });
    fields.value = Object.entries(oldObserve?.fields ?? {});
}
loadDevice();
watch(targetId, loadDevice);
const affected = computed(() => consumers(project(), connection.value.id));
function selectConnection() { connection.value = clone(config.connections?.find(c => c.id === selected.value) ?? fresh()); }
async function testConnection() {
    if (probing.value) return;
    probing.value = true; issues.value = [];
    const snapshot = fingerprint.value;
    const progress = foreground.begin('probe');
    try { await adminRequest('/api/connections/probe', { connection: JSON.parse(snapshot) }); probe.value = 'TCP-соединение установлено. HTTP-команды и чтение состояния не отправлялись.'; }
    catch (error) { issues.value = issuesOf(error); probe.value = 'Проверка неуспешна. Доступность HTTP API и состояние неизвестны.'; }
    finally { probeSnapshot.value = snapshot; probing.value = false; progress.finish(); }
}
function validateStep(): boolean {
    issues.value = [];
    if (step.value === 2 && !legacy.value && (!connection.value.name.trim() || !connection.value.http?.base_url)) issues.value.push({ code: 'required', path: !connection.value.name.trim() ? '/connection/name' : '/connection/http/base_url', message: 'Заполните название и endpoint подключения.' });
    if (step.value === 3) for (const mapping of mappings.value) {
        const http = mapping.http!;
        if (!http.path && !http.url) issues.value.push({ code: 'required', path: '', message: 'Для каждого подключаемого действия укажите путь.' });
        if (http.method === 'GET' && (Object.keys(http.body ?? {}).length || http.argument)) issues.value.push({ code: 'get_body', path: '', message: 'GET не может иметь тело или параметр. Удалите их явно перед выбором GET.' });
    }
    if (step.value === 4 && observing.value && (!observeHttp.value.path || !fields.value.length || fields.value.some(([key, value]) => !key.trim() || !value) || new Set(fields.value.map(([key]) => key)).size !== fields.value.length)) issues.value.push({ code: 'observe_fields', path: '', message: 'Укажите GET path и уникальные top-level поля ответа с существующими свойствами.' });
    return !issues.value.length;
}
function next() { if (validateStep()) step.value++; }
function project() {
    const pendingMappings = mappings.value.map(mapping => {
        const next = clone(mapping);
        if (next.http?.url === undefined && !next.connection_id && !legacy.value) next.connection_id = connection.value.id;
        return next;
    });
    const observation = observing.value ? { ...(oldObserve ?? {}), target_id: targetId.value,
        connection_id: observeConnectionId.value || connection.value.id, executor: 'http',
        http: clone(observeHttp.value), fields: Object.fromEntries(fields.value) } : undefined;
    return projectConnectivity(config, legacy.value ? null : connection.value, targetId.value, pendingMappings, observation);
}
function assemble() {
    if (!validateStep()) return false;
    Object.assign(config, project());
    return true;
}
async function removeConnection() {
    if (!confirm('Удалить подключение? Если оно используется устройствами, backend отклонит удаление.')) return;
    config.connections = (config.connections ?? []).filter(c => c.id !== connection.value.id);
    if (await manual.stage()) await manual.prepare();
}
async function finish() { if (!assemble()) return; if (await manual.stage()) { emit('ready'); await manual.prepare(); }
    else {
        const path = manual.issues.value[0]?.path ?? '';
        if (path.startsWith('/connections')) { step.value = 2; manual.issues.value = manual.issues.value.map(e => ({ ...e, path: e.path.replace(/^\/connections\/\d+/, '/connection') })); }
        else if (path.startsWith('/bindings')) step.value = 3;
        else if (path.startsWith('/observations')) step.value = 4;
    } }
defineExpose({ step, finish });
</script>
<template><div class="wizard" :aria-busy="probing || manual.busy.value"><p class="eyebrow">ШАГ {{ step }} ИЗ 5 · {{ device?.name ?? 'Общее подключение' }}</p><h3>{{ ['', 'Способ подключения', 'Endpoint', 'Действия устройства', 'Чтение состояния', 'Review подключения'][step] }}</h3>
    <div v-if="step === 1"><p>Рабочий транспорт: HTTP / REST</p><div class="inline-actions"><button type="button" class="primary" @click="next">HTTP / REST →</button><button v-for="name in ['MQTT', 'TCP', 'UDP', 'Serial / RS-485', 'CAN']" :key="name" type="button" disabled>{{ name }} · запланировано</button></div></div>
    <div v-if="step === 2"><label>Подключение<select v-model="selected" @change="selectConnection"><option value="">Новое HTTP-подключение</option><option v-if="existing === '__legacy__'" value="__legacy__">Сохранённые отдельные URL</option><option v-for="c in config.connections" :key="c.id" :value="c.id" :disabled="c.type !== 'http'">{{ c.name }}</option></select></label><p v-if="legacy" class="notice">Legacy URL остаются самостоятельными адресами. Новое Connection не создаётся; редактируйте контракты действий на следующем шаге.</p><template v-else><label>Название подключения *<input v-model="connection.name" required data-path="/connection/name"></label><label>Endpoint *<input v-model="connection.http!.base_url" required aria-label="Endpoint *" data-path="/connection/http/base_url"><small class="field-example">Примеры: http://192.168.68.120 — порт 80; http://192.168.68.120:8765 — другой порт. Укажите адрес и порт своего API.</small></label><small>Только http://numeric-IPv4[:port]. Без DNS, credentials и HTTPS.</small><details :open="manual.advanced.value"><summary>Дополнительно</summary><label>Timeout, мс<input v-model.number="connection.timeout_ms" type="number" min="50" max="10000" required data-path="/connection/timeout_ms"></label><label>Base path<input v-model="connection.http!.base_path"></label></details><button v-if="selected && !legacy" type="button" class="danger" @click="removeConnection">Удалить подключение</button><p>Устройства общего подключения: {{ affected.join(', ') || 'пока нет' }}</p><button type="button" :disabled="probing" @click="testConnection">Проверить доступность</button><p v-if="probe" class="notice" aria-live="polite">{{ probeStale ? 'Результат проверки устарел: endpoint или параметры изменены. ' : '' }}{{ probe }}</p></template></div>
    <div v-if="step === 3"><label v-if="!task.id || task.id.startsWith('@')">Устройство (необязательно)<select v-model="targetId"><option value="">Только общее подключение</option><option v-for="d in config.devices" :key="d.id" :value="d.id">{{ d.name }}</option></select></label><article v-for="action in actions" :key="action.id" class="form-section mapping-editor"><label class="check"><input type="checkbox" :checked="mappings.some(b => b.action_id === action.id)" @change="toggle(action.id, ($event.target as HTMLInputElement).checked)">{{ action.name }}</label><p v-if="!mappings.some(b => b.action_id === action.id)" class="muted">Не подключать сейчас</p><template v-for="mapping in mappings.filter(b => b.action_id === action.id)" :key="mapping.action_id"><div v-if="mapping.http?.url" class="notice"><p>Сохранённый legacy URL: {{ mapping.http.url }}. Он останется отдельным адресом.</p><button v-if="!legacy" type="button" @click="delete mapping.http!.url; mapping.http!.path = ''; mapping.connection_id = connection.id">Перевести на выбранное Connection и указать path</button></div><p v-if="mapping.connection_id && mapping.connection_id !== connection.id" class="notice">Действие сохраняет отдельное подключение: {{ config.connections?.find(c => c.id === mapping.connection_id)?.name }}. <button v-if="!legacy" type="button" @click="mapping.connection_id = connection.id">Использовать выбранное подключение для действия</button></p><HttpMappingEditor :http="mapping.http!" :action="action" /></template></article></div>
    <div v-if="step === 4"><label>Чтение состояния<select v-model="observing" :disabled="!targetId || legacy"><option :value="false">Не предоставляется</option><option :value="true">Есть независимый GET</option></select></label><template v-if="observing"><p v-if="observeConnectionId && observeConnectionId !== connection.id" class="notice">Чтение сохраняет отдельное подключение: {{ config.connections?.find(c => c.id === observeConnectionId)?.name }}. <button v-if="!legacy" type="button" @click="observeConnectionId = connection.id">Использовать выбранное подключение для чтения</button></p><HttpMappingEditor :http="observeHttp" observe /><div v-for="(pair, index) in fields" :key="index" class="form-grid"><label>Top-level поле ответа *<input v-model="pair[0]" required><small class="field-example">Пример: on — укажите реальное поле ответа.</small></label><label>Свойство состояния<select v-model="pair[1]"><option value="">Выберите свойство</option><option v-for="property in device?.state" :key="property.id" :value="property.id">{{ property.name }} · {{ property.type }}</option></select></label><button type="button" @click="fields.splice(index, 1)">Удалить поле Observe</button></div><button type="button" @click="fields.push(['', device?.state?.[0]?.id ?? ''])">Добавить поле Observe</button><p>Тип значения GET должен точно совпадать со свойством. Nested JSONPath и преобразование строк/чисел не поддерживаются; проверка ответа выполняется backend при явном чтении.</p></template></div>
    <div v-if="step === 5" class="review-connection"><p><strong>{{ legacy ? 'Сохранённые отдельные URL' : connection.name }}</strong> · {{ connection.http?.base_url }}{{ connection.http?.base_path }}</p><p v-for="action in actions" :key="action.id">{{ action.name }}: {{ mappings.find(b => b.action_id === action.id)?.http?.method ?? 'не подключено' }} {{ mappings.find(b => b.action_id === action.id)?.http?.path }}</p><p>Observe: {{ observing ? 'GET ' + observeHttp.path : 'не предоставляется' }}</p><p>Будут использовать подключение: {{ affected.join(', ') || 'нет' }}</p><p>TCP: {{ probe && !probeStale ? probe : 'не проверен или результат устарел' }}</p><strong>Оборудование командами ещё не проверялось.</strong><p>Далее откроется общий final review всех изменений.</p></div>
    <div class="wizard-footer"><TaskErrors :issues="[...issues, ...manual.issues.value]" /><div class="inline-actions"><button type="button" @click="manual.cancel()">Отмена</button><button v-if="step > 1" type="button" @click="step--; issues = []">Назад</button><button v-if="step < 5 && step > 1" type="button" class="primary" @click="next">Далее</button><button v-if="step === 5" type="button" class="primary" :disabled="manual.busy.value" @click="finish">Сохранить и применить</button></div></div>
</div></template>
