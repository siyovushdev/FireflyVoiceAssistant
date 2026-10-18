<script setup lang="ts">
import { computed, onActivated, onDeactivated, ref, watch } from 'vue';
import { manual } from '../stores/manual.js';
import { clone, consumers, defaultMqttMapping, newMqttConnection, projectMqttWizard,
    connectionActionSummary, connectionObservationSummary, type Mutable } from '../adapters/editing.js';
import type { BindingDto, ConnectionDto, MqttObservationDto, ConfigurationDto } from '../types/wire.js';
import { MqttDeviceDrafts, initialMqttDeviceDraft, observationFromMqttDraft,
    pinMqttObservationConnection, type MqttDeviceDraft } from '../adapters/mqttDeviceDrafts.js';
import TaskErrors from './forms/TaskErrors.vue';
import { adminRequest, issuesOf, type Issue } from '../api/admin.js';

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
// Inspect the configured local service using the existing read-only Admin
// status route. This never enables a broker, contacts a relay or publishes.
interface LocalBrokerStatus { ok: true; installed: boolean; enabled: boolean; running: boolean }
const brokerStatus = ref<LocalBrokerStatus | null>(null);
const brokerStatusBusy = ref(false);
const brokerStatusError = ref('');
let brokerStatusGeneration = 0;
async function inspectLocalBroker() {
    if (!local.value) return;
    const generation = ++brokerStatusGeneration;
    brokerStatus.value = null;
    brokerStatusError.value = '';
    brokerStatusBusy.value = true;
    try {
        const status = await adminRequest<LocalBrokerStatus>('/api/mqtt/local-broker');
        if (generation === brokerStatusGeneration && local.value) brokerStatus.value = status;
    } catch (error) {
        if (generation === brokerStatusGeneration && local.value)
            brokerStatusError.value = issuesOf(error).map(issue => issue.message).join(' ');
    } finally {
        if (generation === brokerStatusGeneration) brokerStatusBusy.value = false;
    }
}
function clearLocalBrokerEvidence() {
    ++brokerStatusGeneration;
    brokerStatus.value = null;
    brokerStatusError.value = '';
    brokerStatusBusy.value = false;
}
// KeepAlive preserves all form fields across protocol navigation, but never
// reuses old local-broker status evidence after the editor is reactivated.
onActivated(() => {
    clearLocalBrokerEvidence();
    if (local.value) void inspectLocalBroker();
});
onDeactivated(clearLocalBrokerEvidence);
const device = computed(() => config.devices.find(d => d.id === targetId.value));
const actions = computed(() => config.actions.filter(a => device.value?.action_ids.includes(a.id)));
const mappings = ref<Mutable<BindingDto>[]>([]);
const observing = ref(false);
const observeConnectionId = ref('');
const replaceOtherObservation = ref(false);
const otherObservation = computed(() => config.observations?.find(o =>
    o.target_id === targetId.value && o.executor !== 'mqtt'));
const observation = ref<Mutable<MqttObservationDto>>(
    clone(initialMqttDeviceDraft(config, targetId.value).observation));
const fields = ref<[string, string][]>([]);
const deviceDrafts = new MqttDeviceDrafts(config);
function currentDeviceDraft(): MqttDeviceDraft {
    return {
        mappings: clone(mappings.value),
        observing: observing.value,
        observeConnectionId: observeConnectionId.value,
        replaceOtherObservation: replaceOtherObservation.value,
        observation: clone(observation.value),
        fields: clone(fields.value)
    };
}

function chooseConnection() {
    const c = config.connections?.find(c => c.id === selected.value && c.type === 'mqtt') ?? newMqttConnection(config);
    connection.value = clone(c);
    host.value = c.network?.host ?? '127.0.0.1'; port.value = c.network?.port ?? 1883;
    tls.value = c.mqtt?.tls ?? false; credentials.value = c.mqtt?.credentials_id ?? '';
    local.value = host.value === '127.0.0.1';
    clearLocalBrokerEvidence();
    if (local.value) void inspectLocalBroker();
}
function loadDevice(id: string) {
    const draft = deviceDrafts.restore(id);
    mappings.value = clone(draft.mappings);
    observing.value = draft.observing;
    observeConnectionId.value = draft.observeConnectionId;
    replaceOtherObservation.value = draft.replaceOtherObservation;
    observation.value = clone(draft.observation);
    fields.value = clone(draft.fields);
}
loadDevice(targetId.value);
watch(targetId, (next, previous) => {
    // Store the outgoing device's *unsaved* editor state before loading the
    // incoming one. Switching A -> B -> A restores all command/observe edits.
    deviceDrafts.remember(previous, currentDeviceDraft());
    loadDevice(next);
}, { flush: 'sync' });
function setObserving(value: boolean) {
    // This is called only for a USER change of the observation toggle, never
    // while restoring an existing device draft. The selected broker belongs
    // to the new subscription from this moment onward.
    if (value)
        observeConnectionId.value = pinMqttObservationConnection(
            observeConnectionId.value, connection.value.id);
    observing.value = value;
}
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
    if (local.value === value) return;
    local.value = value;
    clearLocalBrokerEvidence();
    if (value) {
        host.value = '127.0.0.1'; port.value = 1883; tls.value = false;
        void inspectLocalBroker();
    } else if (host.value === '127.0.0.1') {
        // "External broker" cannot silently keep the old local address.
        host.value = '';
    }
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
        if (!observeConnectionId.value)
            issue('Для подписки на состояние требуется закреплённый MQTT-брокер.');
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
    // First change only the selected Connection. Then project every edited
    // device independently; the editor MUST NOT lose A's work merely because
    // B is the currently visible device when the final Review is opened.
    let candidate = projectMqttWizard({
        config, connection: connection.value, local: local.value, host: host.value,
        port: port.value, tls: tls.value, credentialsId: credentials.value,
        targetId: '', mappings: [], replaceOtherObservation: false
    });
    for (const [id, draft] of deviceDrafts.changed(targetId.value, currentDeviceDraft())) {
        candidate = projectMqttWizard({
            config: candidate, connection: connection.value,
            local: local.value, host: host.value, port: port.value,
            tls: tls.value, credentialsId: credentials.value,
            targetId: id, mappings: draft.mappings,
            observation: observationFromMqttDraft(id, draft),
            replaceOtherObservation: draft.observing && draft.replaceOtherObservation
        });
    }
    return candidate;
}
function validateEditedDevices(): boolean {
    // All edited devices are applied together. Recheck hidden drafts too:
    // an incomplete command/topic from A must not silently escape just
    // because B is selected when the user opens final Review.
    for (const [id, draft] of deviceDrafts.changed(targetId.value, currentDeviceDraft())) {
        let editorStep = 0;
        let message = '';
        if (draft.mappings.some(m => !m.mqtt?.topic?.trim() ||
            !Number.isInteger(m.mqtt.qos) || m.mqtt.qos < 0 || m.mqtt.qos > 1 ||
            config.actions.find(a => a.id === m.action_id)?.parameters.length)) {
            editorStep = 2;
            message = 'Исправьте topic/QoS или неподдерживаемое действие.';
        } else if (draft.observing) {
            if (!draft.observeConnectionId) {
                editorStep = 3;
                message = 'Новая подписка не закреплена за MQTT-брокером.';
            }
            const conflict = config.observations?.some(o =>
                o.target_id === id && o.executor !== 'mqtt');
            if (!editorStep && conflict && !draft.replaceOtherObservation) {
                editorStep = 3;
                message = 'Подтвердите замену существующего чтения состояния.';
            } else if (!editorStep && (!draft.observation.topic?.trim() ||
                (draft.observation.format === 'text'
                    ? !draft.observation.field || !draft.observation.true_payload ||
                      !draft.observation.false_payload ||
                      draft.observation.true_payload === draft.observation.false_payload
                    : !draft.fields.length ||
                      draft.fields.some(([key, field]) => !key.trim() || !field) ||
                      new Set(draft.fields.map(([key]) => key)).size !== draft.fields.length))) {
                editorStep = 3;
                message = 'Завершите настройку MQTT-подписки и полей состояния.';
            }
        }
        if (editorStep) {
            // The sync watcher stores the current device and restores the
            // offending device, so the operator sees the unsaved data.
            targetId.value = id;
            step.value = editorStep;
            issues.value = [{
                code: 'mqtt_device_draft_incomplete', path: '',
                message: (config.devices.find(d => d.id === id)?.name ?? id) + ': ' + message
            }];
            return false;
        }
    }
    return true;
}
const reviewCandidate = computed(() => project());
const affected = computed(() => consumers(reviewCandidate.value, connection.value.id));
const previouslyAffected = computed(() => consumers(config, connection.value.id));
const editedDevices = computed(() => deviceDrafts.changed(targetId.value, currentDeviceDraft())
    .map(([id]) => config.devices.find(d => d.id === id)?.name ?? id));
async function finish() {
    if (!validate() || !validateEditedDevices()) return;
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
    <p v-if="selected && previouslyAffected.length > 1" class="notice">
      Это общее подключение уже используют устройства: {{ previouslyAffected.join(', ') }}.
      Изменение адреса, порта, TLS или credentials для него затронет их все.
      Все изменения будут показаны в общем Review до одного финального Apply.
    </p>
    <div class="inline-actions">
      <button type="button" :class="{ primary: local }" @click="chooseLocal(true)">Брокер Firefly</button>
      <button type="button" :class="{ primary: !local }" @click="chooseLocal(false)">Внешний брокер</button>
    </div>
    <p v-if="local" class="notice">Локальный Mosquitto включается отдельно в разделе «Подключения». Создание и Apply этого подключения не запускают службу и не включают реле.</p>
    <template v-if="local">
      <p v-if="brokerStatus" :class="brokerStatus.installed && brokerStatus.running ? 'muted' : 'notice warning'" role="status">
        Локальный брокер: {{ brokerStatus.installed ? 'установлен' : 'не установлен' }},
        {{ brokerStatus.enabled ? 'автозапуск разрешён' : 'автозапуск выключен' }},
        {{ brokerStatus.running ? 'служба работает' : 'служба не работает' }}.
        {{ brokerStatus.installed && brokerStatus.running ? 'Это статус службы, не проверка учётных данных или подключения устройства.' : 'Реле через локальный брокер сейчас не подтверждено как доступное.' }}
      </p>
      <p v-else-if="brokerStatusError" class="notice warning" role="alert">Статус локального Mosquitto неизвестен: {{ brokerStatusError }}. Не считать соединение проверенным.</p>
      <p v-else class="muted">Статус локального Mosquitto ещё не подтверждён.</p>
      <button type="button" :disabled="brokerStatusBusy" @click="inspectLocalBroker">{{ brokerStatusBusy ? 'Проверяем статус…' : 'Проверить статус брокера (без команд устройству)' }}</button>
    </template>
    <label>Адрес брокера *<input v-model="host" :readonly="local" required /></label>
    <label>Порт *<input v-model.number="port" type="number" min="1" max="65535" required /></label>
    <label class="check"><input v-model="tls" type="checkbox" :disabled="local" />TLS (проверка сертификата)</label>
    <label>ID учётных данных (необязательно)<input v-model="credentials" placeholder="ID заранее созданного секрета" /></label>
    <p class="muted">Указывайте ID, созданный в разделе «Подключения → Учётные данные MQTT». Пароль не хранится в конфигурации и не включается в экспорт. Локальный Mosquitto требует авторизацию, поэтому без подходящего ID подключение Firefly не будет подтверждено.</p>
    <p v-if="!local && !tls" class="notice warning">Без TLS учётные данные могут передаваться по сети открытым текстом. Используйте только доверенную локальную сеть, не подключайте неизвестный WAN-брокер.</p>
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
        <label>Command topic *<input v-model="mapping.mqtt!.topic" required placeholder="Пример: device/relay/set" /></label>
        <p class="muted">Адрес topic в поле — только пример. Введите фактический topic из документации вашего устройства.</p>
        <label>Payload (TEXT / JSON)<textarea v-model="mapping.mqtt!.payload" rows="2" /></label>
        <p class="muted">Автоматически подставленное ON/OFF — лишь предположение по действию включения/выключения, не подтверждённый контракт устройства. Сверьте payload с его документацией.</p>
        <label>QoS<select v-model.number="mapping.mqtt!.qos"><option :value="0">0 — без подтверждения</option>
          <option :value="1">1 — возможен дубль</option></select></label>
        <label class="check"><input v-model="mapping.mqtt!.retain" type="checkbox" />Retain (может быть опасно)</label>
        <p v-if="mapping.mqtt!.retain" class="notice warning">
          Broker сохранит эту команду и может передать её устройству при повторной подписке. Для команд включения/выключения обычно оставляют Retain выключенным.
        </p>
      </template>
    </article>
    <p class="muted">MVP — фиксированные payload, без динамических подстановок параметров действия.</p>
  </div>
  <div v-if="step === 3">
    <label>Подписка на состояние<select v-model="observing" :disabled="!targetId"
       @change="setObserving(($event.target as HTMLSelectElement).value === 'true')">
      <option :value="false">Без чтения состояния</option><option :value="true">Подписаться на topic</option>
    </select></label>
    <template v-if="observing">
      <p v-if="otherObservation" class="notice">
        Сейчас состояние читается через {{ otherObservation.executor?.toUpperCase() ?? 'Другой протокол' }}.
        <label class="check"><input v-model="replaceOtherObservation" type="checkbox" />
          Заменить существующее чтение состояния на MQTT (явное подтверждение)</label>
      </p>
      <p v-if="observeConnectionId && observeConnectionId === connection.id" class="muted">
        Подписка закреплена за брокером {{ connection.name }}. Последующее редактирование других устройств не меняет её брокер.
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
    <p v-if="editedDevices.length" class="notice">
      Несохранённые изменения устройств: {{ editedDevices.join(', ') }}.
      В общий Review попадут изменения каждого перечисленного устройства, даже если сейчас открыто другое.
    </p>
    <p v-if="local" class="notice">Локальный Mosquitto:
      {{ brokerStatus ? (brokerStatus.installed && brokerStatus.running ? 'служба работает, авторизация и реле не проверены' : 'не готова к подключению устройств') : 'статус неизвестен, не проверен' }}.
    </p>
    <p v-else class="muted">Внешний брокер: доступность и авторизация здесь не проверялись.</p>
    <strong>Команды оборудованию не отправлялись. MQTT-статус не означает подтверждение устройства.</strong>
    <p>Следующий экран — общий Review всех изменений. Только отдельное подтверждение выполняет Save → Apply; настройка подключения никогда не включает локальный брокер.</p>
  </div>
  <div class="wizard-footer"><TaskErrors :issues="[...issues, ...manual.issues.value]" />
    <div class="inline-actions">
      <button type="button" @click="manual.cancel()">Отмена</button>
      <button v-if="step > 1" type="button" @click="step--; issues = []">Назад</button>
      <button v-if="step < 4" type="button" class="primary" @click="next">Далее</button>
      <button v-if="step === 4" type="button" class="primary" :disabled="manual.busy.value" @click="finish">В общий Review (без Apply)</button>
    </div>
  </div>
</div></template>
