import { computed, ref, shallowRef } from 'vue';
import { adminRequest, issuesOf, type Issue, type CatalogReply } from '../api/admin.js';
import { readApi, freezeDeep } from '../api/client.js';
import { setupAuth, setupRequest, type Session, type CopilotReply, type Proposal } from '../api/setup.js';
import { clone, consumers } from '../adapters/editing.js';
import { collectFacts, emptyFacts, missingFact, physicalInput, connectivityInput, verifyConnectivity, verifySemantic, supportsPowerContract, setupChanges } from '../adapters/setup.js';
import type { ConfigurationDto } from '../types/wire.js';
import { manual, type TaskKind } from './manual.js';
import { system } from './system.js';

const storageKey = 'firefly-v2-setup-owner';
interface Pending { lane: 'semantic' | 'connectivity'; before: ConfigurationDto; after: ConfigurationDto; version: number; wire?: Proposal; facts?: string }
export const session = shallowRef<Session | null>(null);
export const setupBusy = ref(false), setupIssues = ref<Issue[]>([]), composer = ref(''), setupNotice = ref('');
export const zoneContext = ref(''), deviceContext = ref(''), lane = ref<'semantic' | 'connectivity'>('semantic');
export const conversation = ref<{ role: string; text: string }[]>([]);
export const proposal = shallowRef<Pending | null>(null);
export const facts = ref(emptyFacts()), sharedChoice = ref(''), probeResult = ref('');
const accepted = shallowRef<ConfigurationDto | null>(null);
const retryText = ref('');
export const owned = computed(() => !!setupAuth.token && !!session.value?.active);
export const ready = computed(() => owned.value && !proposal.value && !session.value?.message_incomplete && session.value?.status === 'editing' && !setupBusy.value && JSON.stringify(session.value?.configuration) === JSON.stringify(accepted.value));
export const proposalChanges = computed(() => proposal.value ? setupChanges(proposal.value.before, proposal.value.after) : []);
export const selectedConsumers = computed(() => {
    const config = manual.draft.value;
    if (!config) return [];
    const connections = new Set(config.bindings.filter(b => b.target_id === deviceContext.value).map(b => b.connection_id));
    for (const o of config.observations ?? []) if (o.target_id === deviceContext.value) connections.add(o.connection_id);
    return [...new Set([...connections].flatMap(id => id ? consumers(config, id) : []))];
});
function persist() {
    if (!setupAuth.token) return;
    sessionStorage.setItem(storageKey, JSON.stringify({ token: setupAuth.token, accepted: accepted.value, proposal: proposal.value, retryText: retryText.value, version: session.value?.version, facts: facts.value, device: deviceContext.value, zone: zoneContext.value, lane: lane.value, shared: sharedChoice.value }));
}
function receive(value: Session) {
    if (session.value?.version !== value.version || session.value?.active !== value.active) manual.review.value = null;
    session.value = value; manual.setup.value = value;
}
function installPlan(configuration: ConfigurationDto) { accepted.value = freezeDeep(clone(configuration)); manual.candidate.value = accepted.value; manual.review.value = null; manual.version.value++; persist(); }
async function guardVersion(version = session.value?.version) {
    let current: Session;
    try { current = await setupRequest('heartbeat', {}); }
    catch (error) {
        if (issuesOf(error).some(e => e.message.includes('setup_session_conflict'))) { setupAuth.token = ''; manual.review.value = null; sessionStorage.removeItem(storageKey); }
        throw error;
    }
    receive(current);
    if (current.version !== version) { manual.review.value = null; throw Error('План изменён другой вкладкой. Ввод сохранён; перечитайте предложение и выполните новый review.'); }
}
export async function setupRun(operation: () => Promise<void>) {
    if (setupBusy.value) return;
    setupBusy.value = true; setupIssues.value = [];
    try { await operation(); }
    catch (error) { setupIssues.value = issuesOf(error); }
    finally { setupBusy.value = false; }
}
export async function refreshSession() {
    const value = await setupRequest('status'); receive(value);
    if (setupAuth.token && value.active && !setupBusy.value && !manual.busy.value) {
        try { await guardVersion(); } catch (error) { setupIssues.value = issuesOf(error); }
    }
}
export async function restoreSession() {
    await setupRun(async () => {
        let saved: { token?: string; accepted?: ConfigurationDto; proposal?: Pending; retryText?: string; version?: number; facts?: ReturnType<typeof emptyFacts>; device?: string; zone?: string; lane?: 'semantic' | 'connectivity'; shared?: string } = {};
        try { saved = JSON.parse(sessionStorage.getItem(storageKey) ?? '{}'); } catch { /* No authority inferred from invalid storage. */ }
        const value = await setupRequest('status'); receive(value);
        if (!value.active) return;
        if (!saved.token) {
            setupNotice.value = 'Настройка открыта другим браузером. Доступен только просмотр.';
            const base = await readApi('/api/config');
            if (value.configuration) proposal.value = { lane: 'semantic', before: base.configuration, after: value.configuration, version: value.version! };
            return;
        }
        setupAuth.token = saved.token;
        try { await setupRequest('heartbeat', {}); } catch { setupAuth.token = ''; throw Error('Эта настройка принадлежит другой сессии.'); }
        const base = await readApi('/api/config'); manual.base.value = base;
        accepted.value = saved.accepted ?? base.configuration; manual.candidate.value = accepted.value;
        retryText.value = saved.retryText ?? ''; facts.value = saved.facts ?? emptyFacts(); deviceContext.value = saved.device ?? ''; zoneContext.value = saved.zone ?? ''; lane.value = saved.lane ?? 'semantic'; sharedChoice.value = saved.shared ?? '';
        if (value.configuration) {
            // Reload always re-reviews a persisted unconfirmed server result.
            proposal.value = saved.proposal?.version === value.version ? saved.proposal! :
                JSON.stringify(value.configuration) !== JSON.stringify(accepted.value) ? { lane: 'semantic', before: accepted.value, after: value.configuration, version: value.version! } : null;
        }
        if (proposal.value) setupNotice.value = 'Неподтверждённое предложение восстановлено. Проверьте и примите его заново.';
        persist();
    });
}
export async function startSetup(resume = false) {
    await setupRun(async () => {
        const before = await setupRequest('status');
        if (before.active) throw Error('Настройка уже активна. Используйте принадлежащую вам сессию.');
        const base = await readApi('/api/config');
        if (manual.base.value && manual.base.value.hash !== base.hash && manual.dirty.value) throw Error('Основа локального плана изменилась; сначала согласуйте его в Advanced.');
        setupAuth.token = '';
        const value = await setupRequest(resume ? 'resume' : 'start', {}); receive(value);
        setupAuth.token = value.session_id!; lane.value = 'semantic'; facts.value = emptyFacts(); manual.base.value = base; accepted.value = manual.candidate.value ?? base.configuration;
        proposal.value = null;
        if (resume && value.configuration && JSON.stringify(value.configuration) !== JSON.stringify(accepted.value)) proposal.value = { lane: 'semantic', before: accepted.value, after: value.configuration, version: value.version! };
        else if (!resume && JSON.stringify(accepted.value) !== JSON.stringify(value.configuration)) receive(await setupRequest('draft', { version: value.version, configuration: accepted.value }));
        accepted.value = proposal.value ? accepted.value : session.value!.configuration!; manual.candidate.value = accepted.value; persist(); setupNotice.value = 'Настройка начата. Рабочие голосовые команды приостановлены.';
    });
}
export async function manualAfterQuestion() {
    await setupRun(async () => {
        await guardVersion();
        receive(await setupRequest('draft', { version: session.value?.version, configuration: accepted.value }));
        setupNotice.value = 'Уточнение закрыто. Продолжите через ручной редактор; принятый план сохранён.'; persist();
    });
}
export async function cancelSetup() {
    await setupRun(async () => {
        receive(await setupRequest('cancel', {}));
        if (session.value?.active) { setupNotice.value = 'Отмена выполняется; дождитесь восстановления runtime.'; return; }
        setupAuth.token = ''; sessionStorage.removeItem(storageKey); proposal.value = null;
        manual.review.value = null; setupNotice.value = 'Настройка остановлена. Принятый локальный план сохранён; серверный черновик можно восстановить отдельно.';
        await system.refresh(true);
    });
}
export function context(zone = '', device = '') { zoneContext.value = zone; deviceContext.value = device; }
export function connectDevice(id: string) {
    deviceContext.value = id; facts.value = emptyFacts(); sharedChoice.value = ''; probeResult.value = '';
    if (!manual.draft.value || !supportsPowerContract(manual.draft.value, id)) { lane.value = 'semantic'; setupNotice.value = 'Сначала укажите возможности устройства вручную. Этот AI-путь подключения требует объявленных ON/OFF и boolean Питания; другие HTTP contracts доступны в ручном wizard.'; return; }
    lane.value = 'connectivity'; setupNotice.value = 'Укажите известный HTTP contract устройства. Неизвестные поля останутся пустыми.';
}
export async function sendMessage(retry = false) {
    await setupRun(async () => {
        if (!owned.value || proposal.value || manual.task.value || manual.voiceBusy.value) throw Error('Начните настройку и завершите текущий review или ручную задачу.');
        const text = retry ? retryText.value : composer.value.trim();
        if (!text) throw Error('Введите просьбу.');
        if (session.value?.message_incomplete && !retry) throw Error('Незавершённую просьбу нужно повторить полностью через «Повторить просьбу» либо отменить настройку.');
        await guardVersion(); manual.review.value = null;
        conversation.value.push({ role: 'Вы', text });
        if (lane.value === 'connectivity' || physicalInput(text)) {
            lane.value = 'connectivity';
            facts.value = collectFacts(facts.value, text, missingFact(facts.value)?.key);
            const missing = missingFact(facts.value);
            if (!deviceContext.value || missing) { setupNotice.value = !deviceContext.value ? 'Выберите устройство, которое подключаете.' : missing!.question; conversation.value.push({ role: 'Firefly', text: setupNotice.value }); composer.value = ''; return; }
            if (selectedConsumers.value.length > 1 && !sharedChoice.value) throw Error('Выберите: изменить общее подключение или создать отдельное.');
            const input = connectivityInput(accepted.value!, deviceContext.value, facts.value, sharedChoice.value);
            const response = await adminRequest<CopilotReply>('/api/admin/copilot', { text: input, configuration: accepted.value });
            const after = verifyConnectivity(accepted.value!, response.draft, deviceContext.value, facts.value, sharedChoice.value);
            proposal.value = { lane: 'connectivity', before: accepted.value!, after, wire: response.draft, version: session.value!.version!, facts: JSON.stringify(facts.value) };
        } else {
            let input = text;
            if (!retry && deviceContext.value) { const device = accepted.value?.devices.find(d => d.id === deviceContext.value); if (!device) throw Error('Контекст устройства устарел.'); input = `Устройство: ${device.name} . ${input}`; }
            if (!retry && zoneContext.value) { const zone = accepted.value?.rooms.find(z => z.id === zoneContext.value); if (!zone) throw Error('Контекст зоны устарел.'); input = `Зона: ${zone.name} . ${input}`; }
            retryText.value = input;
            // Persist checkpoint before sending: response loss/reload must not imply acceptance.
            persist();
            let value: Session;
            try { value = await setupRequest('message', { version: session.value?.version, request_id: crypto.randomUUID(), text: input, atomic_message: true }); }
            catch (error) { receive(await setupRequest('status')); persist(); throw error; }
            receive(value);
            if (value.questions?.length) { setupNotice.value = value.questions.join(' '); conversation.value.push({ role: 'Firefly', text: setupNotice.value }); composer.value = ''; persist(); return; }
            try { verifySemantic(accepted.value!, value.configuration!); }
            catch (error) { receive(await setupRequest('draft', { version: value.version, configuration: accepted.value })); throw error; }
            proposal.value = { lane: 'semantic', before: accepted.value!, after: value.configuration!, version: value.version! };
        }
        setupNotice.value = 'AI подготовил предложение. Оно ещё не принято в план.'; composer.value = ''; persist();
    });
}
async function acceptInternal() {
    const pending = proposal.value;
    if (!pending) throw Error('Нет предложения.');
    await guardVersion(pending.version);
    if (pending.lane === 'connectivity') {
        if (pending.facts !== JSON.stringify(facts.value)) throw Error('Факты изменились; требуется новое предложение.');
        verifyConnectivity(pending.before, pending.wire!, deviceContext.value, facts.value, sharedChoice.value);
        const reply = await adminRequest<CatalogReply>('/api/admin/copilot/accept', { configuration: pending.before, proposal: pending.wire });
        receive(await setupRequest('draft', { version: pending.version, configuration: reply.configuration }));
        installPlan(reply.configuration);
    } else { await adminRequest('/api/config/validate', { configuration: pending.after }); installPlan(pending.after); }
    proposal.value = null; probeResult.value = ''; persist();
    setupNotice.value = 'Принято в план. Система ещё не изменена. Подключение и оборудование командами не проверялись.';
    conversation.value.push({ role: 'Firefly', text: setupNotice.value });
}
export async function acceptProposal() { await setupRun(acceptInternal); }
export async function rejectProposal() {
    await setupRun(async () => {
        const pending = proposal.value; if (!pending) return;
        await guardVersion(pending.version);
        if (pending.lane === 'semantic') receive(await setupRequest('draft', { version: pending.version, configuration: pending.before }));
        proposal.value = null; probeResult.value = ''; persist(); setupNotice.value = 'Предложение отклонено. Предыдущий принятый план сохранён.';
    });
}
export async function editProposal(kind: TaskKind, id: string) {
    const pending = proposal.value; if (!pending) return;
    await rejectProposal(); if (proposal.value) return;
    await manual.begin(kind, id);
    if (manual.task.value) manual.task.value.buffer = clone(pending.after);
}
export async function probeProposal() {
    await setupRun(async () => {
        const pending = proposal.value; if (!pending || pending.facts !== JSON.stringify(facts.value)) throw Error('Предложение устарело.');
        const ids = new Set(pending.after.bindings.filter(b => b.target_id === deviceContext.value).map(b => b.connection_id));
        const connection = pending.after.connections?.find(c => ids.has(c.id));
        if (!connection) throw Error('Подключение отсутствует в предложении.');
        await adminRequest('/api/connections/probe', { connection }); probeResult.value = 'TCP доступен. Отправлено 0 HTTP bytes; API и оборудование командами ещё не проверялись.';
    });
}
manual.connectSetup({ owns: () => owned.value, ready: () => ready.value,
    sync: async configuration => { await guardVersion(); receive(await setupRequest('draft', { version: session.value?.version, configuration })); installPlan(session.value!.configuration!); return session.value!.configuration!; },
    apply: async () => {
        if (!ready.value) throw Error('Неподтверждённое предложение блокирует Apply.');
        await guardVersion();
        try { receive(await setupRequest('apply', { version: session.value?.version })); }
        finally { receive(await setupRequest('status')); manual.setup.value = session.value; }
        if (session.value?.status !== 'applied') throw Error('Применение не подтверждено; проверьте статус восстановления.');
        setupAuth.token = ''; sessionStorage.removeItem(storageKey); accepted.value = null;
        setupNotice.value = 'Эта часть уже применена. Для дальнейшей настройки начните новую сессию.';
        conversation.value.push({ role: 'Firefly', text: setupNotice.value });
    }
});
