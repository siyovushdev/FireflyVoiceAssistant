<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { manual } from '../stores/manual.js';
import { clone, consumers, defaultMqttMapping, newMqttConnection, projectConnectivity,
    connectionActionSummary, connectionObservationSummary, type Mutable } from '../adapters/editing.js';
import type { BindingDto, ConnectionDto, MqttObservationDto, ConfigurationDto } from '../types/wire.js';
import TaskErrors from './forms/TaskErrors.vue';
import type { Issue } from '../api/admin.js';

const emit = defineEmits<{ ready: []; back: [] }>();
const task = manual.task.value!;
const config = task.buffer;
const initialId = task.id.startsWith('@') ? task.id.slice(1) :
    config.bindings.find(b => b.target_id === task.id && b.executor === 'mqtt')?.connection_id ??
    config.observations?.find(o => o.target_id === task.id && o.executor === 'mqtt')?.connection_id ?? '';
const initial = config.connections?.find(c => c.id === initialId && c.type === 'mqtt');
const selected = ref(initial?.id ?? '');
const connection = ref<Mutable<ConnectionDto>>(clone(initial ?? newMqttConnection(config)));
const targetId = ref(task.id.startsWith('@') ? '' : task.id);
const step = ref(1), issues = ref<Issue[]>([]);
const host = ref(connection.value.network?.host ?? '127.0.0.1');
const port = ref(connection.value.network?.port ?? 1883);
const tls = ref(connection.value.mqtt?.tls ?? false);
const credentials = ref(connection.value.mqtt?.credentials_id ?? '');
const local = ref(host.value === '127.0.0.1');
const device = computed(() => config.devices.find(d => d.id === targetId.value));
const actions = computed(() => config.actions.filter(a => device.value?.action_ids.includes(a.id)));
const mappings = ref<Mutable<BindingDto>[]>([]);
let previous = config.observations?.find(o => o.target_id === targetId.value && o.executor === 'mqtt');
const observing = ref(!!previous);
const observeConnectionId = ref(previous?.connection_id ?? '');
const replaceOtherObservation = ref(false);
const otherObservation = computed(() => config.observations?.find(o =>
    o.target_id === targetId.value && o.executor !== 'mqtt'));
function emptyObservation(): Mutable<MqttObservationDto> {
    return { topic: '', format: 'text', qos: 0, field: 'power', true_payload: 'ON', false_payload: 'OFF' };
}
const observation = ref<Mutable<MqttObservationDto>>(clone(previous?.mqtt ?? emptyObservation()));
const fields = ref<[string, string][]>(Object.entries(previous?.fields ?? {}));

function chooseConnection() {
    const c = config.connections?.find(c => c.id === selected.value && c.type === 'mqtt') ?? newMqttConnection(config);
    connection.value = clone(c);
    host.value = c.network?.host ?? '127.0.0.1'; port.value = c.network?.port ?? 1883;
    tls.value = c.mqtt?.tls ?? false; credentials.value = c.mqtt?.credentials_id ?? '';
    local.value = host.value === '127.0.0.1';
}
function loadDevice() {
    mappings.value = config.bindings.filter(b => b.target_id === targetId.value && b.executor === 'mqtt').map(clone);
    previous = config.observations?.find(o => o.target_id === targetId.value && o.executor === 'mqtt');
    observing.value = !!previous;
    observeConnectionId.value = previous?.connection_id ?? '';
    replaceOtherObservation.value = false;
    observation.value = clone(previous?.mqtt ?? {
        ...emptyObservation(), field: device.value?.state?.find(s => s.type === 'boolean')?.id ?? ''
    });
    fields.value = Object.entries(previous?.fields ?? {});
}
loadDevice();
watch(targetId, loadDevice);
function toggle(actionId: string, enabled: boolean) {
    if (!enabled) { mappings.value = mappings.value.filter(m => m.action_id !== actionId); return; }
    const action = actions.value.find(a => a.id === actionId);
    if (!action || action.parameters.length || config.bindings.some(b =>
        b.target_id === targetId.value && b.action_id === actionId && b.executor !== 'mqtt')) return;
    const assignment = action.assignments?.find(a => a.field === 'power');
    const mqtt = defaultMqttMapping();
    if (typeof assignment?.value === 'boolean') mqtt.payload = assignment.value ? 'ON' : 'OFF';
    mappings.value.push({ target_id: targetId.value, action_id: actionId, connection_id: connection.value.id,
        executor: 'mqtt', mqtt });
}
function chooseLocal(value: boolean) {
    local.value = value;
    if (value) { host.value = '127.0.0.1'; port.value = 1883; tls.value = false; }
}
function validate(): boolean {
    issues.value = [];
    const issue = (message: string) => issues.value.push({ code: 'required', path: '', message });
    if (step.value === 1 && (!connection.value.name.trim() || !host.value.trim() ||
        !Number.isInteger(port.value) || port.value < 1 || port.value > 65535))
        issue('Укажите название, адрес и порт брокера.');
    if (step.value === 2) for (const m of mappings.value) {
        if (!m.mqtt?.topic.trim() || !Number.isInteger(m.mqtt.qos) || m.mqtt.qos < 0 || m.mqtt.qos > 1)
            issue('Для каждого действия укажите topic и QoS 0 или 1.');
        if (actions.value.find(a => a.id === m.action_id)?.parameters.length)
            issue('Параметризованные MQTT-действия пока не поддерживаются.');
    }
    if (step.value === 3 && observing.value) {
        if (otherObservation.value && !replaceOtherObservation.value)
            issue('Для замены чтения состояния другого протокола требуется явное подтверждение.');
        if (!observation.value.topic.trim()) issue('Укажите topic состояния.');
        if (observation.value.format === 'text') {
            if (!observation.value.field || !observation.value.true_payload || !observation.value.false_payload ||
                observation.value.true_payload === observation.value.false_payload)
                issue('Выберите boolean-свойство и значения ON/OFF.');
        } else if (!fields.value.length || fields.value.some(([key, field]) => !key.trim() || !field) ||
            new Set(fields.value.map(([key]) => key)).size !== fields.value.length)
            issue('Укажите JSON-поля верхнего уровня и свойства устройства.');
    }
    return !issues.value.length;
}
function next() { if (validate()) step.value++; }
function project(): Mutable<ConfigurationDto> {
    const c = clone(connection.value);
    c.enabled = true;
    c.network = { host: local.value ? '127.0.0.1' : host.value.trim(), port: port.value };
    c.mqtt = { tls: tls.value, ...(credentials.value.trim() ? { credentials_id: credentials.value.trim() } : {}) };
    // Preserve each action's existing broker. Only newly added mappings and
    // explicit "Use selected broker" buttons change the connection reference.
    const nextMappings = mappings.value.map(m => ({ ...clone(m), executor: 'mqtt',
        connection_id: m.connection_id || c.id }));
    let nextObservation: NonNullable<ConfigurationDto['observations']>[number] | undefined;
    if (observing.value && targetId.value) {
        const m = clone(observation.value);
        nextObservation = { target_id: targetId.value,
            connection_id: observeConnectionId.value || c.id, executor: 'mqtt',
            mqtt: m.format === 'text'
                ? { topic: m.topic, format: 'text', qos: m.qos, field: m.field,
                    true_payload: m.true_payload, false_payload: m.false_payload }
                : { topic: m.topic, format: 'json', qos: m.qos },
            fields: m.format === 'json' ? Object.fromEntries(fields.value) : {} };
    }
    return projectConnectivity(config, c, targetId.value, nextMappings, nextObservation,
        'mqtt', observing.value && replaceOtherObservation.value);
}
const reviewCandidate = computed(() => project());
const affected = computed(() => consumers(reviewCandidate.value, connection.value.id));
async function finish() {
    if (!validate()) return;
    Object.assign(config, project());
    if (await manual.stage()) { emit('ready'); await manual.prepare(); }
    else {
        const path = manual.issues.value[0]?.path ?? '';
        step.value = path.startsWith('/observations') ? 3 : path.startsWith('/bindings') ? 2 : 1;
    }
}
</script>
<template><div class="wizard" :aria-busy="manual.busy.value">
  <p class="eyebrow">MQTT · ШАГ {{ step }} ИЗ 4 · {{ device?.name ?? 'Общее подключение' }}</p>
  <h3>{{ ['', 'Подключение к брокеру', 'Команды устройства', 'Получение состояния', 'Review MQTT'][step] }}</h3>
  <div v-if="step === 1">
    <button type="button" @click="emit('back')">← Выбор протокола</button>
    <label>Подключение<select v-model="selected" @change="chooseConnection">
      <option value="">Новое MQTT-подключение</option>
      <option v-for="c in (config.connections ?? []).filter(c => c.type === 'mqtt')" :key="c.id" :value="c.id">{{ c.name }}</option>
    </select></label>
    <label>Название *<input v-model="connection.name" required /></label>
    <div class="inline-actions">
      <button type="button" :class="{ primary: local }" @click="chooseLocal(true)">Брокер Firefly</button>
      <button type="button" :class="{ primary: !local }" @click="chooseLocal(false)">Внешний брокер</button>
    </div>
    <p v-if="local" class="notice">Локальный Mosquitto должен быть установлен и включён отдельно. Apply подключения не запускает службу.</p>
    <label>Адрес брокера *<input v-model="host" :readonly="local" required /></label>
    <label>Порт *<input v-model.number="port" type="number" min="1" max="65535" required /></label>
    <label class="check"><input v-model="tls" type="checkbox" :disabled="local" />TLS (проверка сертификата)</label>
    <label>ID учётных данных (необязательно)<input v-model="credentials" placeholder="relay_user" /></label>
    <p class="muted">Только ссылка на предварительно защищённый секрет; пароль в открытом JSON не хранится.</p>
    <details><summary>Дополнительно</summary><label>Тайм-аут, мс<input v-model.number="connection.timeout_ms" type="number" min="50" max="10000" /></label></details>
  </div>
  <div v-if="step === 2">
    <label v-if="!task.id || task.id.startsWith('@')">Устройство<select v-model="targetId">
      <option value="">Пока только брокер</option><option v-for="d in config.devices" :key="d.id" :value="d.id">{{ d.name }}</option>
    </select></label>
    <p>Для каждого действия укажите topic и точный текст или JSON по документации устройства.</p>
    <article v-for="action in actions" :key="action.id" class="form-section">
      <label class="check"><input type="checkbox" :checked="mappings.some(b => b.action_id === action.id)"
        :disabled="!!action.parameters.length || config.bindings.some(b => b.target_id === targetId && b.action_id === action.id && b.executor !== 'mqtt')"
        @change="toggle(action.id, ($event.target as HTMLInputElement).checked)" />{{ action.name }}</label>
      <p v-if="action.parameters.length" class="muted">MQTT MVP поддерживает только действия без параметров.</p>
      <p v-else-if="config.bindings.some(b => b.target_id === targetId && b.action_id === action.id && b.executor !== 'mqtt')"
          class="notice">Действие уже подключено через другой протокол. Сначала явно отвяжите его там.</p>
      <template v-for="mapping in mappings.filter(m => m.action_id === action.id)" :key="mapping.action_id">
        <p v-if="mapping.connection_id && mapping.connection_id !== connection.id" class="notice">
          Действие остаётся на брокере {{ config.connections?.find(c => c.id === mapping.connection_id)?.name ?? mapping.connection_id }}.
          <button type="button" @click="mapping.connection_id = connection.id">Перенести на выбранный брокер</button>
        </p>
        <label>Command topic *<input v-model="mapping.mqtt!.topic" required placeholder="home/relay/set" /></label>
        <label>Payload (TEXT / JSON)<textarea v-model="mapping.mqtt!.payload" rows="2" /></label>
        <label>QoS<select v-model.number="mapping.mqtt!.qos"><option :value="0">0 — без подтверждения</option>
          <option :value="1">1 — возможен дубль</option></select></label>
        <label class="check"><input v-model="mapping.mqtt!.retain" type="checkbox" />Retain (может быть опасно)</label>
      </template>
    </article>
    <p class="muted">MVP — фиксированные payload, без динамических подстановок параметров действия.</p>
  </div>
  <div v-if="step === 3">
    <label>Подписка на состояние<select v-model="observing" :disabled="!targetId">
      <option :value="false">Без чтения состояния</option><option :value="true">Подписаться на topic</option>
    </select></label>
    <template v-if="observing">
      <p v-if="otherObservation" class="notice">
        Сейчас состояние читается через {{ otherObservation.executor?.toUpperCase() ?? 'Другой протокол' }}.
        <label class="check"><input v-model="replaceOtherObservation" type="checkbox" />
          Заменить существующее чтение состояния на MQTT (явное подтверждение)</label>
      </p>
      <p v-if="observeConnectionId && observeConnectionId !== connection.id" class="notice">
        Подписка остаётся на брокере {{ config.connections?.find(c => c.id === observeConnectionId)?.name ?? observeConnectionId }}.
        <button type="button" @click="observeConnectionId = connection.id">Перенести подписку на выбранный брокер</button>
      </p>
      <label>State topic *<input v-model="observation.topic" required /></label>
      <label>Формат<select v-model="observation.format"><option value="text">TEXT: ON / OFF</option>
        <option value="json">JSON: поля верхнего уровня</option></select></label>
      <template v-if="observation.format === 'text'">
        <label>Boolean-свойство<select v-model="observation.field"><option value="">Выберите</option>
          <option v-for="f in (device?.state ?? []).filter(f => f.type === 'boolean')" :key="f.id" :value="f.id">{{ f.name }}</option>
        </select></label>
        <label>Включено<input v-model="observation.true_payload" /></label>
        <label>Выключено<input v-model="observation.false_payload" /></label>
      </template>
      <template v-else>
        <div v-for="(pair, index) in fields" :key="index" class="form-grid">
          <label>JSON-поле<input v-model="pair[0]" /></label>
          <label>Свойство<select v-model="pair[1]"><option value="">Выберите</option>
            <option v-for="f in device?.state" :key="f.id" :value="f.id">{{ f.name }}</option>
          </select></label>
          <button type="button" @click="fields.splice(index, 1)">Удалить</button>
        </div>
        <button type="button" @click="fields.push(['', ''])">Добавить поле</button>
      </template>
      <label>QoS подписки<select v-model.number="observation.qos"><option :value="0">0</option><option :value="1">1</option></select></label>
      <p class="notice">Retained-сообщения не принимаются как свежее подтверждение.</p>
    </template>
  </div>
  <div v-if="step === 4" class="review-connection">
    <p><strong>{{ connection.name }}</strong> · {{ local ? 'Локальный Firefly' : host }}:{{ port }} · {{ tls ? 'TLS' : 'без TLS' }}</p>
    <p v-for="a in actions" :key="a.id">{{ a.name }}:
      {{ connectionActionSummary(reviewCandidate, targetId, a.id) }}</p>
    <p>Состояние: {{ connectionObservationSummary(reviewCandidate, targetId) }}</p>
    <p>Потребители подключения: {{ affected.join(', ') || 'нет' }}</p>
    <strong>Оборудование командами ещё не проверялось.</strong>
    <p>Откроется общий Review → Apply. Сохранение не включает локальный брокер.</p>
  </div>
  <div class="wizard-footer"><TaskErrors :issues="[...issues, ...manual.issues.value]" />
    <div class="inline-actions">
      <button type="button" @click="manual.cancel()">Отмена</button>
      <button v-if="step > 1" type="button" @click="step--; issues = []">Назад</button>
      <button v-if="step < 4" type="button" class="primary" @click="next">Далее</button>
      <button v-if="step === 4" type="button" class="primary" :disabled="manual.busy.value" @click="finish">Сохранить и применить</button>
    </div>
  </div>
</div></template>
