<script setup lang="ts">
import { computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { system } from '../stores/system.js';
import { manual } from '../stores/manual.js';
import { adminRequest, issuesOf, type AdminReply, type Issue } from '../api/admin.js';
import { commandIntent } from '../adapters/commands.js';
import { initial } from '../adapters/editing.js';
import type { ActionDto, Scalar } from '../types/wire.js';
import ScalarInput from './forms/ScalarInput.vue';
import TaskErrors from './forms/TaskErrors.vue';
const props = defineProps<{ id: string }>();
const router = useRouter();
const device = computed(() => system.active.value?.configuration.devices.find(d => d.id === props.id));
const actions = computed(() => system.active.value?.configuration.actions.filter(a => device.value?.action_ids.includes(a.id)) ?? []);
const argumentsByAction = ref<Record<string, Record<string, Scalar>>>({});
function args(action: ActionDto) { return argumentsByAction.value[action.id] ??= Object.fromEntries(action.parameters.map(p => [p.id, initial(p)])); }
function intent(action: ActionDto) { return device.value && system.active.value ? commandIntent(system.active.value.configuration, device.value, action, args(action)) : null; }
const busy = ref(false), message = ref(''), issues = ref<Issue[]>([]), details = ref<AdminReply | null>(null);
async function act(action: ActionDto) {
    const command = intent(action), active = system.active.value;
    if (!command || !active || busy.value) return;
    busy.value = true; issues.value = []; message.value = '';
    try {
        const reply = await adminRequest<AdminReply & { execution?: { success: boolean; error: string } }>('/api/devices/command', { ...command, expected_hash: active.hash });
        details.value = reply;
        const noTransport = ['binding_missing', 'binding_ambiguous', 'connection_disabled', 'unsupported_executor'].includes(reply.execution?.error ?? '');
        message.value = noTransport ? 'Действие не отправлено: проверьте настройку подключения. Физическое состояние не изменяется в интерфейсе.' : reply.execution?.success ? `${action.name}: команда принята. Ожидаемый результат не подтверждён чтением состояния.` : 'Результат неизвестен. Запрос завершился ошибкой; оборудование могло выполнить действие. Автоматического повтора нет.';
        if (!reply.execution?.success) issues.value = [{ code: reply.execution?.error ?? 'execution_unknown', path: '', message: reply.execution?.error ?? 'Исполнение не подтверждено' }];
    } catch (error) { issues.value = issuesOf(error); message.value = 'Исполнение не подтверждено. При отказе guard HTTP-запрос не отправляется; при потере ответа результат неизвестен.'; }
    finally { await system.refresh(true); busy.value = false; }
}
async function poll() {
    if (!system.active.value || busy.value) return;
    busy.value = true; issues.value = [];
    try { await adminRequest('/api/devices/poll', { target_id: props.id, expected_hash: system.active.value.hash }); message.value = 'Независимое чтение завершено. Физическое состояние показано по ответу устройства.'; }
    catch (error) { issues.value = issuesOf(error); message.value = 'Чтение не подтвердило состояние. Последние свидетельства и их время сохранены.'; }
    finally { await system.refresh(true); busy.value = false; }
}
</script>
<template><section class="panel operational"><h2>Действия</h2><p v-if="manual.dirty.value" class="notice">Управление использует активную конфигурацию. Локальный план ещё не действует.</p><article v-for="action in actions" :key="action.id" class="form-section"><strong>{{ action.name }}</strong><div class="form-grid"><ScalarInput v-for="parameter in action.parameters" :key="parameter.id" :field="parameter" v-model="args(action)[parameter.id]" :label="parameter.name" /></div><template v-if="intent(action)"><p class="command-text">Будет проверена команда: «{{ intent(action)!.text }}»</p><button type="button" :disabled="busy || manual.busy.value || manual.setup.value?.active" @click="act(action)">{{ action.name }}</button></template><template v-else><p>Для этой возможности нет однозначной команды кнопки. Введите намерение вручную в Playground.</p><button @click="router.push('/advanced/playground')">Открыть Playground</button></template></article><button v-if="system.active.value?.configuration.observations?.some(o => o.target_id === id)" :disabled="busy || manual.setup.value?.active" @click="poll">Прочитать состояние</button><p v-if="message" class="notice" role="status">{{ message }}</p><TaskErrors :issues="issues" /><details v-if="manual.advanced.value && details"><summary>Command admission / transport result</summary><pre>{{ JSON.stringify(details, null, 2) }}</pre></details></section></template>
