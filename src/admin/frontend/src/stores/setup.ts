import { foreground, type OperationKind } from './foreground.js';
import { computed, ref, shallowRef } from 'vue';
import { adminRequest, AdminError, issuesOf, type Issue } from '../api/admin.js';
import { readApi, freezeDeep } from '../api/client.js';
import { setupAuth, setupRequest, type Session } from '../api/setup.js';
import { clone, consumers } from '../adapters/editing.js';
import { collectFacts, emptyFacts, missingFact, buildConnectivity, verifyConnectivity, targetConnectionIds, verifySemantic, supportsPowerContract, setupChanges } from '../adapters/setup.js';
import { promptLimits as readPromptLimits, measurePrompt, planningLabel, containsPrivateCredentials } from '../adapters/setupPrompt.js';
import type { ConfigurationDto } from '../types/wire.js';
import { manual, type TaskKind } from './manual.js';
import { system } from './system.js';

const storageKey = 'firefly-v2-setup-owner';
interface Pending { lane: 'semantic' | 'connectivity'; before: ConfigurationDto; after: ConfigurationDto; version: number; source?: 'explicit'; facts?: string; scope?: 'configuration' }
export const session = shallowRef<Session | null>(null);
export const messagePending = ref(false), cancelBusy = ref(false), cancelRequested = ref(false);
export const setupBusy = ref(false), setupIssues = ref<Issue[]>([]), composer = ref(''), setupNotice = ref('');
export const sendFeedback = ref(''), sendFailed = ref(false);
export const zoneContext = ref(''), deviceContext = ref(''), lane = ref<'semantic' | 'connectivity'>('semantic');
export const conversation = ref<{ role: string; text: string }[]>([]);
export const proposal = shallowRef<Pending | null>(null);
export const facts = ref(emptyFacts()), sharedChoice = ref(''), probeResult = ref('');
const accepted = shallowRef<ConfigurationDto | null>(null);
const retryText = ref(''), sourceText = ref('');
const initialPlanPending = ref(false), initialPlanSyncing = ref(false);
export const entering = computed(() => session.value?.active && session.value.status === 'entering');
export const transitioning = computed(() => !!session.value?.active && (initialPlanPending.value || initialPlanSyncing.value || ['entering', 'cancel_pending', 'restoring', 'restoring_for_admission', 'planning'].includes(session.value.status) || cancelRequested.value));
const connectivityContext = () => JSON.stringify({ target: deviceContext.value, facts: facts.value, shared: sharedChoice.value });
export const owned = computed(() => !!setupAuth.token && !!session.value?.active);
export const ready = computed(() => owned.value && !transitioning.value && !proposal.value && !session.value?.message_incomplete && session.value?.status === 'editing' && !setupBusy.value && !cancelBusy.value && JSON.stringify(session.value?.configuration) === JSON.stringify(accepted.value));
export const proposalChanges = computed(() => proposal.value ? setupChanges(proposal.value.before, proposal.value.after) : []);
export const limits = computed(() => readPromptLimits(session.value?.limits));
function contextualInput(text: string) {
    if (deviceContext.value) {
        const device = accepted.value?.devices.find(d => d.id === deviceContext.value);
        if (!device) throw Error('Контекст устройства устарел.');
        text = `Устройство: ${device.name} . ${text}`;
    }
    if (zoneContext.value) {
        const zone = accepted.value?.rooms.find(z => z.id === zoneContext.value);
        if (!zone) throw Error('Контекст зоны устарел.');
        text = `Зона: ${zone.name} . ${text}`;
    }
    return text;
}
export const promptSize = computed(() => {
    let text = composer.value;
    try { if (lane.value === 'semantic') text = contextualInput(text); } catch { /* Sending reports stale context. */ }
    return measurePrompt(text, limits.value);
});
export const progressLabel = computed(() => planningLabel(session.value?.progress));
export const canCancel = computed(() => owned.value && !cancelBusy.value && !cancelRequested.value && !manual.busy.value
    && (!setupBusy.value || messagePending.value || !!entering.value || session.value?.status === 'planning'));
function addConversation(role: string, text: string) {
    conversation.value.push({ role, text });
    // Keep an in-memory window; never persist an accumulating prompt transcript.
    while (conversation.value.length > 12 || conversation.value.length > 1 && conversation.value.reduce((n, m) => n + m.text.length, 0) > 48000) conversation.value.shift();
}

export const selectedConsumers = computed(() => {
    const config = manual.draft.value;
    if (!config) return [];
    const connections = targetConnectionIds(config, deviceContext.value);
    return [...new Set(connections.flatMap(id => id ? consumers(config, id) : []))];
});
function persist() {
    if (!setupAuth.token) return;
    sessionStorage.setItem(storageKey, JSON.stringify({ token: setupAuth.token, initialPlanPending: initialPlanPending.value, accepted: accepted.value, proposal: proposal.value, retryText: retryText.value, sourceText: sourceText.value, version: session.value?.version, facts: facts.value, device: deviceContext.value, zone: zoneContext.value, lane: lane.value, shared: sharedChoice.value }));
}
function receive(value: Session) {
    if (value.active === session.value?.active && typeof value.version === 'number' && typeof session.value.version === 'number' && value.version < session.value.version) return;
    if (session.value?.version !== value.version || session.value?.active !== value.active) manual.review.value = null;
    session.value = value; manual.setup.value = value;
    if (!value.active && setupAuth.token) {
        setupAuth.token = ''; sessionStorage.removeItem(storageKey);
        initialPlanPending.value = false; proposal.value = null; cancelRequested.value = false;
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
    if (setupBusy.value || cancelBusy.value) return;
    setupBusy.value = true; setupIssues.value = [];
    try { if (kind) await foreground.run(kind, operation); else await operation(); }
    catch (error) { setupIssues.value = issuesOf(error); }
    finally { setupBusy.value = false; }
}
let statusRefresh: Promise<void> | null = null;
export function pollStatus(): Promise<void> {
    if (statusRefresh) return statusRefresh;
    const owner = setupAuth.token;
    statusRefresh = setupRequest('status').then(value => {
        if (setupAuth.token === owner) receive(value);
    }).finally(() => { statusRefresh = null; });
    return statusRefresh;
}
export async function refreshSession() {
    await pollStatus();
    if (setupAuth.token && session.value?.active && !setupBusy.value && !manual.busy.value && session.value.status !== 'planning' && !cancelRequested.value) {
        try {
            await guardVersion(undefined, true); await finishInitialPlan();
            if (!manual.task.value && !proposal.value && !initialPlanPending.value && accepted.value && session.value?.status === 'editing' && !session.value.message_incomplete && session.value.configuration && JSON.stringify(session.value.configuration) !== JSON.stringify(accepted.value)) {
                await setupRun(async () => {
                    const value = session.value!;
                    if (value.proposal_scope === 'configuration') await adminRequest('/api/config/validate', { configuration: value.configuration });
                    else verifySemantic(accepted.value!, value.configuration!);
                    await guardVersion(value.version);
                    proposal.value = { lane: 'semantic', before: accepted.value!, after: value.configuration!, version: value.version!, scope: value.proposal_scope };
                    setupNotice.value = 'Обработка завершена. Проверьте восстановленное предложение и примите его в план.'; persist();
                }, 'draft');
            }
        }
        catch (error) { const issues = issuesOf(error); if (!issues.some(e => e.message.includes('setup_busy'))) setupIssues.value = issues; }
    }
}
export async function restoreSession(foregroundRequest = false) {
    await setupRun(async () => {
        setupNotice.value = '';
        let saved: { token?: string; initialPlanPending?: boolean; accepted?: ConfigurationDto; proposal?: Pending; retryText?: string; sourceText?: string; version?: number; facts?: ReturnType<typeof emptyFacts>; device?: string; zone?: string; lane?: 'semantic' | 'connectivity'; shared?: string } = {};
        try { saved = JSON.parse(sessionStorage.getItem(storageKey) ?? '{}'); } catch { /* No authority inferred from invalid storage. */ }
        const value = await setupRequest('status'); receive(value);
        if (!value.active) return;
        if (!saved.token) {
            setupAuth.token = '';
            setupNotice.value = 'Настройка открыта другим браузером. Доступен только просмотр.';
            const base = await readApi('/api/config');
            if (value.configuration && !value.message_incomplete && value.status === 'editing') proposal.value = { lane: 'semantic', before: base.configuration, after: value.configuration, version: value.version!, scope: value.proposal_scope };
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
        retryText.value = saved.retryText && !containsPrivateCredentials(saved.retryText) ? saved.retryText : ''; sourceText.value = saved.sourceText && !containsPrivateCredentials(saved.sourceText) ? saved.sourceText : '';
        if (!composer.value && sourceText.value) composer.value = sourceText.value;
        if (!conversation.value.length && sourceText.value) addConversation('Вы', sourceText.value);
        facts.value = saved.facts ?? emptyFacts(); deviceContext.value = saved.device ?? ''; zoneContext.value = saved.zone ?? ''; lane.value = saved.lane ?? 'semantic'; sharedChoice.value = saved.shared ?? '';
        if (initialPlanPending.value || value.message_incomplete || value.status === 'planning') proposal.value = null;
        else if (value.configuration) {
            // Reload always re-reviews a persisted unconfirmed server result.
            proposal.value = saved.proposal?.version === value.version ? saved.proposal! :
                JSON.stringify(value.configuration) !== JSON.stringify(accepted.value) ? { lane: 'semantic', before: accepted.value, after: value.configuration, version: value.version!, scope: value.proposal_scope } : null;
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
        cancelRequested.value = false; sourceText.value = ''; retryText.value = ''; conversation.value = [];
        lane.value = 'semantic'; facts.value = emptyFacts(); manual.base.value = base; accepted.value = manual.candidate.value ?? base.configuration;
        proposal.value = null; initialPlanPending.value = !resume && JSON.stringify(accepted.value) !== JSON.stringify(value.configuration);
        if (resume && value.configuration && JSON.stringify(value.configuration) !== JSON.stringify(accepted.value)) proposal.value = { lane: 'semantic', before: accepted.value, after: value.configuration, version: value.version!, scope: value.proposal_scope };
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
    if (!canCancel.value) return;
    cancelBusy.value = true; cancelRequested.value = true; setupIssues.value = [];
    try {
        await foreground.run('cancelSetup', async () => {
            const value = await setupRequest('cancel', {});
            receive(value.status === 'cancel_pending' ? { ...session.value!, ...value } : value);
            manual.review.value = null; proposal.value = null;
            if (session.value?.active) {
                setupNotice.value = 'Остановка запрошена. Дождитесь завершения текущего шага и восстановления ассистента.';
                return;
            }
            setupNotice.value = 'Настройка остановлена. Принятый локальный план сохранён; серверный черновик можно восстановить отдельно.';
            if (!messagePending.value) await system.refresh(true);
        });
    } catch (error) { cancelRequested.value = false; setupIssues.value = issuesOf(error); }
    finally { cancelBusy.value = false; }
}
export async function discardMessage() {
    await setupRun(async () => {
        if (!owned.value || !session.value?.message_incomplete || transitioning.value) throw Error('Дождитесь завершения обработки запроса.');
        await guardVersion();
        const value = await setupRequest('discard_message', { version: session.value?.version });
        receive(value);
        if (value.message_incomplete || JSON.stringify(value.configuration) !== JSON.stringify(accepted.value)) throw Error('Исходный план не подтверждён. Перечитайте серверный черновик.');
        proposal.value = null; manual.review.value = null;
        composer.value = sourceText.value || retryText.value;
        // A legacy retry may contain prefixed context; do not add it a second time.
        if (!sourceText.value) { zoneContext.value = ''; deviceContext.value = ''; }
        retryText.value = ''; persist();
        setupNotice.value = 'Незавершённый запрос отменён, принятый план сохранён. Исправьте текст и отправьте заново.';
    }, 'reject');
}
export function context(zone = '', device = '') { zoneContext.value = zone; deviceContext.value = device; }
export function connectDevice(id: string) {
    deviceContext.value = id; facts.value = emptyFacts(); sharedChoice.value = ''; probeResult.value = '';
    if (!manual.draft.value || !supportsPowerContract(manual.draft.value, id)) { lane.value = 'connectivity'; setupNotice.value = 'Сначала укажите возможности устройства вручную. Этот путь подключения требует объявленных ON/OFF и boolean Питания; другие HTTP contracts доступны в ручном wizard.'; return; }
    lane.value = 'connectivity'; setupNotice.value = 'Укажите известный HTTP contract устройства. Неизвестные поля останутся пустыми.';
}
export async function sendMessage(retry = false) {
    if (setupBusy.value || cancelBusy.value) return;
    sendFeedback.value = 'Проверяем запрос…'; sendFailed.value = false;
    let submitted = false;
    await setupRun(async () => {
        if (!owned.value || transitioning.value || proposal.value || manual.task.value || manual.voiceBusy.value) throw Error('Начните настройку и завершите текущий review или ручную задачу.');
        const text = retry ? retryText.value : composer.value;
        if (!text.trim()) throw Error('Введите просьбу.');
        if (containsPrivateCredentials(text)) throw new AdminError([{ code: 'admin_private_credentials', path: '/text', message: 'Возможно, в тексте указаны данные доступа. Задайте их отдельно в настройках доступа; здесь используйте credentials_id. Текст не отправлен AI и не сохранён для повтора.' }]);
        if (session.value?.message_incomplete && !retry) throw Error('Незавершённую просьбу нужно повторить полностью через «Повторить просьбу» либо отменить настройку.');
        const input = lane.value === 'semantic' && !retry ? contextualInput(text) : text;
        const size = measurePrompt(input, limits.value);
        if (size.exceeds) throw Error(`Запрос слишком длинный: ${size.codepoints} символов, ${size.bytes} байт. Допустимо ${limits.value.message_codepoints} символов и ${limits.value.message_bytes} байт. Текст сохранён — сократите его или отправьте несколькими просьбами.`);
        await guardVersion(); manual.review.value = null;
        addConversation('Вы', retry ? sourceText.value || text : text);
        if (lane.value === 'connectivity') {
            lane.value = 'connectivity';
            facts.value = collectFacts(facts.value, text, missingFact(facts.value)?.key);
            const missing = missingFact(facts.value);
            if (!deviceContext.value || missing) { setupNotice.value = !deviceContext.value ? 'Выберите устройство, которое подключаете.' : missing!.question; addConversation('Firefly', setupNotice.value); composer.value = ''; persist(); return; }
            const before = accepted.value!, version = session.value!.version!, fingerprint = connectivityContext();
            if (JSON.stringify(session.value!.configuration) !== JSON.stringify(before)) throw Error('Принятый план изменился. Перечитайте серверный черновик.');
            const after = buildConnectivity(before, deviceContext.value, facts.value, sharedChoice.value);
            await adminRequest('/api/config/validate', { configuration: after });
            await guardVersion(version);
            if (fingerprint !== connectivityContext()) throw Error('Факты или устройство изменились. Подготовьте предложение заново.');
            proposal.value = { lane: 'connectivity', source: 'explicit', before, after: freezeDeep(after), version, facts: fingerprint };
        } else {
            retryText.value = input;
            if (!retry) sourceText.value = text;
            // Persist checkpoint before sending: response loss/reload must not imply acceptance.
            persist();
            let value: Session;
            messagePending.value = true;
            sendFeedback.value = 'Запрос отправляется и обрабатывается AI. Дождитесь предложения или уточнения.';
            submitted = true;
            try { value = await setupRequest('message', { version: session.value?.version, request_id: crypto.randomUUID(), text: input, atomic_message: true }); }
            catch (error) { receive(await setupRequest('status')); persist(); throw error; }
            finally { messagePending.value = false; }
            if (cancelRequested.value || !setupAuth.token) { receive(await setupRequest('status')); return; }
            receive(value);
            if (value.questions?.length) { setupNotice.value = value.questions.join(' '); addConversation('Firefly', setupNotice.value); composer.value = ''; persist(); return; }
            try {
                if (value.proposal_scope === 'configuration') {
                    // Only the server's explicitly grounded full-configuration
                    // pipeline can propose connectivity. Revalidate the exact
                    // returned candidate before displaying it for review.
                    await adminRequest('/api/config/validate', { configuration: value.configuration });
                } else verifySemantic(accepted.value!, value.configuration!);
            } catch (error) { receive(await setupRequest('draft', { version: value.version, configuration: accepted.value })); throw error; }
            proposal.value = { lane: 'semantic', before: accepted.value!, after: value.configuration!, version: value.version!, scope: value.proposal_scope };
        }
        setupNotice.value = lane.value === 'connectivity' ? 'Подключение собрано из явных фактов. Проверьте предложение: оно ещё не принято в план.' : 'AI подготовил предложение. Оно ещё не принято в план.'; composer.value = ''; persist();
    });
    if (setupIssues.value.length) {
        sendFailed.value = true;
        sendFeedback.value = submitted ? 'Готовое предложение не получено. Причина указана ниже; автоматического повтора нет.' : 'Запрос не отправлен. Исправьте ошибку, указанную ниже.';
    } else if (cancelRequested.value || !setupAuth.token) {
        sendFeedback.value = 'Обработка остановлена. Изменения не применены.';
    } else {
        sendFeedback.value = setupNotice.value;
    }
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
    addConversation('Firefly', setupNotice.value);
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
        addConversation('Firefly', setupNotice.value);
    }
});
