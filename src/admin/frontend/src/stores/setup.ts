import { foreground, type OperationKind } from './foreground.js';
import { computed, ref, shallowRef } from 'vue';
import { adminRequest, issuesOf, type Issue } from '../api/admin.js';
import { readApi, freezeDeep } from '../api/client.js';
import { setupAuth, setupRequest, type Session } from '../api/setup.js';
import { clone, consumers } from '../adapters/editing.js';
import { collectFacts, emptyFacts, missingFact, physicalInput, buildConnectivity, verifyConnectivity, targetConnectionIds, verifySemantic, supportsPowerContract, setupChanges } from '../adapters/setup.js';
import type { ConfigurationDto } from '../types/wire.js';
import { manual, type TaskKind } from './manual.js';
import { system } from './system.js';

const storageKey = 'firefly-v2-setup-owner';
interface Pending { lane: 'semantic' | 'connectivity'; before: ConfigurationDto; after: ConfigurationDto; version: number; source?: 'explicit'; facts?: string }
export const session = shallowRef<Session | null>(null);
export const setupBusy = ref(false), setupIssues = ref<Issue[]>([]), composer = ref(''), setupNotice = ref('');
export const zoneContext = ref(''), deviceContext = ref(''), lane = ref<'semantic' | 'connectivity'>('semantic');
export const conversation = ref<{ role: string; text: string }[]>([]);
export const proposal = shallowRef<Pending | null>(null);
export const facts = ref(emptyFacts()), sharedChoice = ref(''), probeResult = ref('');
const accepted = shallowRef<ConfigurationDto | null>(null);
const retryText = ref('');
const initialPlanPending = ref(false), initialPlanSyncing = ref(false);
export const entering = computed(() => session.value?.active && session.value.status === 'entering');
export const transitioning = computed(() => !!session.value?.active && (initialPlanPending.value || initialPlanSyncing.value || ['entering', 'cancel_pending', 'restoring', 'restoring_for_admission'].includes(session.value.status)));
const connectivityContext = () => JSON.stringify({ target: deviceContext.value, facts: facts.value, shared: sharedChoice.value });
export const owned = computed(() => !!setupAuth.token && !!session.value?.active);
export const ready = computed(() => owned.value && !transitioning.value && !proposal.value && !session.value?.message_incomplete && session.value?.status === 'editing' && !setupBusy.value && JSON.stringify(session.value?.configuration) === JSON.stringify(accepted.value));
export const proposalChanges = computed(() => proposal.value ? setupChanges(proposal.value.before, proposal.value.after) : []);
export const selectedConsumers = computed(() => {
    const config = manual.draft.value;
    if (!config) return [];
    const connections = targetConnectionIds(config, deviceContext.value);
    return [...new Set(connections.flatMap(id => id ? consumers(config, id) : []))];
});
function persist() {
    if (!setupAuth.token) return;
    sessionStorage.setItem(storageKey, JSON.stringify({ token: setupAuth.token, initialPlanPending: initialPlanPending.value, accepted: accepted.value, proposal: proposal.value, retryText: retryText.value, version: session.value?.version, facts: facts.value, device: deviceContext.value, zone: zoneContext.value, lane: lane.value, shared: sharedChoice.value }));
}
function receive(value: Session) {
    if (session.value?.version !== value.version || session.value?.active !== value.active) manual.review.value = null;
    session.value = value; manual.setup.value = value;
    if (!value.active && setupAuth.token) {
        setupAuth.token = ''; sessionStorage.removeItem(storageKey);
        initialPlanPending.value = false; proposal.value = null;
    }
}
function installPlan(configuration: ConfigurationDto) { accepted.value = freezeDeep(clone(configuration)); manual.candidate.value = accepted.value; manual.review.value = null; manual.version.value++; persist(); }
async function guardVersion(version = session.value?.version, allowTransition = false) {
    let current: Session;
    try { current = await setupRequest('heartbeat', {}); }
    catch (error) {
        if (issuesOf(error).some(e => e.message.includes('setup_session_conflict'))) { setupAuth.token = ''; manual.review.value = null; sessionStorage.removeItem(storageKey); }
        throw error;
    }
    receive(current);
    if (!allowTransition && transitioning.value) throw Error('Подготовка или восстановление AI ещё не завершены.');
    if (current.version !== version) { manual.review.value = null; throw Error('План изменён другой вкладкой. Ввод сохранён; перечитайте предложение и выполните новый review.'); }
}
export async function setupRun(operation: () => Promise<void>, kind: OperationKind | null = 'ai') {
    if (setupBusy.value) return;
    setupBusy.value = true; setupIssues.value = [];
    try { if (kind) await foreground.run(kind, operation); else await operation(); }
    catch (error) { setupIssues.value = issuesOf(error); }
    finally { setupBusy.value = false; }
}
export async function refreshSession() {
    const value = await setupRequest('status'); receive(value);
    if (setupAuth.token && value.active && !setupBusy.value && !manual.busy.value) {
        try { await guardVersion(undefined, true); await finishInitialPlan(); }
        catch (error) { const issues = issuesOf(error); if (!issues.some(e => e.message.includes('setup_busy'))) setupIssues.value = issues; }
    }
}
export async function restoreSession(foregroundRequest = false) {
    await setupRun(async () => {
        setupNotice.value = '';
        let saved: { token?: string; initialPlanPending?: boolean; accepted?: ConfigurationDto; proposal?: Pending; retryText?: string; version?: number; facts?: ReturnType<typeof emptyFacts>; device?: string; zone?: string; lane?: 'semantic' | 'connectivity'; shared?: string } = {};
        try { saved = JSON.parse(sessionStorage.getItem(storageKey) ?? '{}'); } catch { /* No authority inferred from invalid storage. */ }
        const value = await setupRequest('status'); receive(value);
        if (!value.active) return;
        if (!saved.token) {
            setupAuth.token = '';
            setupNotice.value = 'Настройка открыта другим браузером. Доступен только просмотр.';
            const base = await readApi('/api/config');
            if (value.configuration) proposal.value = { lane: 'semantic', before: base.configuration, after: value.configuration, version: value.version! };
            return;
        }
        setupAuth.token = saved.token;
        try { receive(await setupRequest('heartbeat', {})); }
        catch (error) {
            const issues = issuesOf(error);
            if (issues.some(e => e.message.includes('setup_session_conflict'))) {
                setupAuth.token = ''; sessionStorage.removeItem(storageKey);
                throw Error('Эта настройка принадлежит другой сессии.');
            }
            // Backend checks ownership before reporting busy. A short entry
            // completion/checkpoint lock cannot turn this owner into another tab.
            if (!issues.some(e => e.message.includes('setup_busy'))) throw error;
        }
        const base = await readApi('/api/config'); manual.base.value = base;
        initialPlanPending.value = saved.initialPlanPending ?? false;
        accepted.value = saved.accepted ?? base.configuration; manual.candidate.value = accepted.value;
        retryText.value = saved.retryText ?? ''; facts.value = saved.facts ?? emptyFacts(); deviceContext.value = saved.device ?? ''; zoneContext.value = saved.zone ?? ''; lane.value = saved.lane ?? 'semantic'; sharedChoice.value = saved.shared ?? '';
        if (initialPlanPending.value) proposal.value = null;
        else if (value.configuration) {
            // Reload always re-reviews a persisted unconfirmed server result.
            proposal.value = saved.proposal?.version === value.version ? saved.proposal! :
                JSON.stringify(value.configuration) !== JSON.stringify(accepted.value) ? { lane: 'semantic', before: accepted.value, after: value.configuration, version: value.version! } : null;
        }
        if (proposal.value?.lane === 'connectivity' && proposal.value.source !== 'explicit') {
            proposal.value = null; setupNotice.value = 'Прежнее AI-предложение подключения устарело. Подготовьте предложение из явных фактов заново.';
        } else if (proposal.value) setupNotice.value = 'Неподтверждённое предложение восстановлено. Проверьте и примите его заново.';
        persist(); await finishInitialPlan();
    }, foregroundRequest ? 'draft' : null);
}
async function finishInitialPlan() {
    if (!initialPlanPending.value || session.value?.status !== 'editing') return;
    // This is the manual plan explicitly carried into Start. Consume the flag
    // before the CAS request so failure/response loss cannot trigger an auto retry.
    initialPlanSyncing.value = true; initialPlanPending.value = false; persist();
    try {
        receive(await setupRequest('draft', { version: session.value.version, configuration: accepted.value }));
        installPlan(session.value!.configuration!);
    } finally { initialPlanSyncing.value = false; }
}
export async function startSetup(resume = false) {
    await setupRun(async () => {
        const before = await setupRequest('status');
        if (before.active) throw Error('Настройка уже активна. Используйте принадлежащую вам сессию.');
        const base = await readApi('/api/config');
        if (manual.base.value && manual.base.value.hash !== base.hash && manual.dirty.value) throw Error('Основа локального плана изменилась; сначала согласуйте его в Advanced.');
        setupAuth.token = '';
        const value = await setupRequest(resume ? 'resume' : 'start', { async_enter: true }); receive(value);
        setupAuth.token = value.session_id!;
        // Persist identity before any follow-up request or model-ready handling.
        sessionStorage.setItem(storageKey, JSON.stringify({ token: setupAuth.token }));
        lane.value = 'semantic'; facts.value = emptyFacts(); manual.base.value = base; accepted.value = manual.candidate.value ?? base.configuration;
        proposal.value = null; initialPlanPending.value = !resume && JSON.stringify(accepted.value) !== JSON.stringify(value.configuration);
        if (resume && value.configuration && JSON.stringify(value.configuration) !== JSON.stringify(accepted.value)) proposal.value = { lane: 'semantic', before: accepted.value, after: value.configuration, version: value.version! };
        accepted.value = proposal.value || initialPlanPending.value ? accepted.value : session.value!.configuration!; manual.candidate.value = accepted.value; persist(); setupNotice.value = ''; await finishInitialPlan();
    }, 'setup');
}
export async function manualAfterQuestion() {
    await setupRun(async () => {
        await guardVersion();
        receive(await setupRequest('draft', { version: session.value?.version, configuration: accepted.value }));
        setupNotice.value = 'Уточнение закрыто. Продолжите через ручной редактор; принятый план сохранён.'; persist();
    }, 'reject');
}
export async function cancelSetup() {
    await setupRun(async () => {
        receive(await setupRequest('cancel', {}));
        if (session.value?.active) { setupNotice.value = 'Отмена выполняется; дождитесь восстановления runtime.'; return; }
        setupAuth.token = ''; sessionStorage.removeItem(storageKey); proposal.value = null;
        manual.review.value = null; setupNotice.value = 'Настройка остановлена. Принятый локальный план сохранён; серверный черновик можно восстановить отдельно.';
        await system.refresh(true);
    }, 'cancelSetup');
}
export function context(zone = '', device = '') { zoneContext.value = zone; deviceContext.value = device; }
export function connectDevice(id: string) {
    deviceContext.value = id; facts.value = emptyFacts(); sharedChoice.value = ''; probeResult.value = '';
    if (!manual.draft.value || !supportsPowerContract(manual.draft.value, id)) { lane.value = 'connectivity'; setupNotice.value = 'Сначала укажите возможности устройства вручную. Этот путь подключения требует объявленных ON/OFF и boolean Питания; другие HTTP contracts доступны в ручном wizard.'; return; }
    lane.value = 'connectivity'; setupNotice.value = 'Укажите известный HTTP contract устройства. Неизвестные поля останутся пустыми.';
}
export async function sendMessage(retry = false) {
    await setupRun(async () => {
        if (!owned.value || transitioning.value || proposal.value || manual.task.value || manual.voiceBusy.value) throw Error('Начните настройку и завершите текущий review или ручную задачу.');
        const text = retry ? retryText.value : composer.value.trim();
        if (!text) throw Error('Введите просьбу.');
        if (session.value?.message_incomplete && !retry) throw Error('Незавершённую просьбу нужно повторить полностью через «Повторить просьбу» либо отменить настройку.');
        await guardVersion(); manual.review.value = null;
        conversation.value.push({ role: 'Вы', text });
        if (lane.value === 'connectivity' || physicalInput(text)) {
            lane.value = 'connectivity';
            facts.value = collectFacts(facts.value, text, missingFact(facts.value)?.key);
            const missing = missingFact(facts.value);
            if (!deviceContext.value || missing) { setupNotice.value = !deviceContext.value ? 'Выберите устройство, которое подключаете.' : missing!.question; conversation.value.push({ role: 'Firefly', text: setupNotice.value }); composer.value = ''; persist(); return; }
            const before = accepted.value!, version = session.value!.version!, fingerprint = connectivityContext();
            if (JSON.stringify(session.value!.configuration) !== JSON.stringify(before)) throw Error('Принятый план изменился. Перечитайте серверный черновик.');
            const after = buildConnectivity(before, deviceContext.value, facts.value, sharedChoice.value);
            await adminRequest('/api/config/validate', { configuration: after });
            await guardVersion(version);
            if (fingerprint !== connectivityContext()) throw Error('Факты или устройство изменились. Подготовьте предложение заново.');
            proposal.value = { lane: 'connectivity', source: 'explicit', before, after: freezeDeep(after), version, facts: fingerprint };
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
        setupNotice.value = lane.value === 'connectivity' ? 'Подключение собрано из явных фактов. Проверьте предложение: оно ещё не принято в план.' : 'AI подготовил предложение. Оно ещё не принято в план.'; composer.value = ''; persist();
    });
}
function verifyPendingConnectivity(pending: Pending) {
    if (pending.source !== 'explicit' || pending.facts !== connectivityContext()
        || JSON.stringify(pending.before) !== JSON.stringify(accepted.value)
        || JSON.stringify(pending.before) !== JSON.stringify(session.value?.configuration)) throw Error('Факты или принятый план изменились; требуется новое предложение.');
    verifyConnectivity(pending.before, pending.after, deviceContext.value, facts.value, sharedChoice.value);
}
async function acceptInternal() {
    const pending = proposal.value;
    if (!pending) throw Error('Нет предложения.');
    await guardVersion(pending.version);
    if (pending.lane === 'connectivity') {
        verifyPendingConnectivity(pending);
        await adminRequest('/api/config/validate', { configuration: pending.after });
        await guardVersion(pending.version);
        verifyPendingConnectivity(pending);
        receive(await setupRequest('draft', { version: pending.version, configuration: pending.after }));
        installPlan(session.value!.configuration!);
    } else { await adminRequest('/api/config/validate', { configuration: pending.after }); installPlan(pending.after); }
    proposal.value = null; probeResult.value = ''; persist();
    setupNotice.value = 'Принято в план. Система ещё не изменена. Подключение и оборудование командами не проверялись.';
    conversation.value.push({ role: 'Firefly', text: setupNotice.value });
}
export async function acceptProposal() { await setupRun(acceptInternal, 'accept'); }
export async function rejectProposal() {
    await setupRun(async () => {
        const pending = proposal.value; if (!pending) return;
        await guardVersion(pending.version);
        if (pending.lane === 'semantic') receive(await setupRequest('draft', { version: pending.version, configuration: pending.before }));
        proposal.value = null; probeResult.value = ''; persist(); setupNotice.value = 'Предложение отклонено. Предыдущий принятый план сохранён.';
    }, 'reject');
}
export async function editProposal(kind: TaskKind, id: string) {
    const pending = proposal.value; if (!pending) return;
    await rejectProposal(); if (proposal.value) return;
    await manual.begin(kind, id, pending.after);
}
export async function manualConnectivity() {
    if (proposal.value) { await editProposal('connection', deviceContext.value); return; }
    await manual.begin('connection', deviceContext.value);
}
export async function probeProposal() {
    await setupRun(async () => {
        const pending = proposal.value; if (!pending || pending.lane !== 'connectivity') throw Error('Предложение устарело.');
        await guardVersion(pending.version); verifyPendingConnectivity(pending);
        const ids = new Set(pending.after.bindings.filter(b => b.target_id === deviceContext.value).map(b => b.connection_id));
        const connection = pending.after.connections?.find(c => ids.has(c.id));
        if (!connection) throw Error('Подключение отсутствует в предложении.');
        await adminRequest('/api/connections/probe', { connection }); probeResult.value = 'TCP доступен. Отправлено 0 HTTP bytes; API и оборудование командами ещё не проверялись.';
    }, 'probe');
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
