import { computed, ref, shallowRef, watch } from 'vue';
import { adminRequest, AdminError, identity, issuesOf, type AdminReply, type CatalogReply, type Issue, type SetupStatus } from '../api/admin.js';
import { readApi, freezeDeep } from '../api/client.js';
import { allocate, clone, powerOnly, readableDiff, type Draft } from '../adapters/editing.js';
import type { CatalogDto, ConfigurationDto, Identity } from '../types/wire.js';
import { system } from './system.js';
import { zoneSpeechIssues } from '../adapters/manualValidation.js';

export type TaskKind = 'system' | 'zone' | 'device' | 'connection';
export interface TaskBuffer { kind: TaskKind; id: string; buffer: Draft; isNew: boolean; version: number }
interface Review { candidate: ConfigurationDto; stored: CatalogDto; active: CatalogDto | null; version: number; storedDiff: AdminReply; activeDiff: AdminReply; validation: AdminReply }
export interface SetupBridge {
    owns(): boolean;
    ready(): boolean;
    sync(configuration: ConfigurationDto): Promise<ConfigurationDto>;
    apply(): Promise<void>;
}
export function createManualStore(api = adminRequest, read = readApi, refresh = () => system.refresh(true)) {
    let bridge: SetupBridge | undefined;
    const connectSetup = (value: SetupBridge) => { bridge = value; };
    const base = shallowRef<CatalogDto | null>(null);
    const candidate = shallowRef<ConfigurationDto | null>(null);
    const task = ref<TaskBuffer | null>(null);
    const review = shallowRef<Review | null>(null);
    const voiceBusy = ref(false), importBuffer = ref('');
    const version = ref(0), busy = ref(false), advanced = ref(false), showReview = ref(false);
    const issues = ref<Issue[]>([]), outcome = ref('');
    const setup = shallowRef<SetupStatus | null>(null);
    const draft = computed(() => candidate.value ?? base.value?.configuration ?? system.stored.value?.configuration ?? system.configuration.value);
    const dirty = computed(() => !!importBuffer.value.trim() || !!task.value || !!candidate.value && JSON.stringify(candidate.value) !== JSON.stringify(base.value?.configuration));
    const canConfirm = computed(() => !!review.value && review.value.version === version.value && !task.value && !busy.value && !voiceBusy.value && (!setup.value?.active || !!bridge?.owns() && bridge.ready()) && (review.value.validation.apply_ready === true || !!setup.value?.active && !!bridge?.owns() && review.value.validation.valid === true));
    // Any local edit invalidates review, even if the user later restores a value.
    watch(task, () => { version.value++; review.value = null; }, { deep: true, flush: 'sync' });
    watch(importBuffer, () => { version.value++; review.value = null; }, { flush: 'sync' });
    async function refreshSetupStatus() { setup.value = await api<SetupStatus>('/api/setup/status'); }
    async function checkSetup() {
        await refreshSetupStatus();
        if (setup.value?.active && (!bridge?.owns() || !bridge.ready())) throw new AdminError([{ code: 'setup_in_progress', path: '', message: 'Завершите review AI-предложения или вернитесь к принадлежащей вам настройке. Чужая сессия доступна только для просмотра.' }]);
    }
    async function ensure() {
        await checkSetup();
        if (!base.value) { base.value = await read('/api/config'); candidate.value = base.value.configuration; }
    }
    async function run(operation: () => Promise<void>) {
        if (busy.value) return;
        busy.value = true; issues.value = [];
        try { await operation(); }
        catch (error) { issues.value = issuesOf(error); }
        finally { busy.value = false; }
    }
    async function begin(kind: TaskKind, id = '') {
        await run(async () => {
            if (task.value) throw Error('Завершите или отмените текущую задачу.');
            await ensure(); outcome.value = ''; showReview.value = false;
            const buffer = clone(candidate.value!); let isNew = !id;
            if (kind === 'system') { id = buffer.system.id; isNew = false; }
            if (kind === 'zone' && !id) {
                id = allocate(buffer, 'zone'); buffer.rooms.push({ id, name: '', speech: { mentions: [], locations: [] }, metadata: { type: 'location', parent_id: null, container: false, description: '' } });
            }
            if (kind === 'device' && !id) {
                id = allocate(buffer, 'device');
                const device = { id, name: '', kind: 'generic', room_id: null, enabled: true, action_ids: [] as string[], spoken_aliases: [] as string[], state: [] };
                buffer.devices.push(device); powerOnly(buffer, device);
            }
            if (kind === 'zone') {
                const zone = buffer.rooms.find(z => z.id === id);
                if (zone) { zone.metadata ??= {}; zone.speech ??= { mentions: [], locations: [] }; }
            }
            if (kind === 'device') { const device = buffer.devices.find(d => d.id === id); if (device) device.metadata ??= {}; }
            task.value = { kind, id, buffer, isNew, version: version.value };
        });
    }
    function cancel() { if (busy.value) return; task.value = null; issues.value = []; showReview.value = false; }
    async function stage() {
        let success = false;
        await run(async () => {
            if (!task.value) return;
            let next = clone(task.value.buffer);
            if (task.value.kind === 'zone') {
                const incomplete = zoneSpeechIssues(next, task.value.id);
                if (incomplete.length) throw new AdminError(incomplete);
            }
            await checkSetup(); await api('/api/config/validate', { configuration: next });
            if (setup.value?.active) next = clone(await bridge!.sync(next));
            candidate.value = freezeDeep(next); version.value++; task.value = null; review.value = null;
            outcome.value = 'Изменения добавлены в план. Система ещё не изменена.'; success = true;
        });
        return success;
    }
    async function replaceCandidate(configuration: ConfigurationDto) {
        await run(async () => {
            if (task.value) throw Error('Сначала завершите текущую задачу.');
            await ensure(); await api('/api/config/validate', { configuration });
            if (setup.value?.active) configuration = await bridge!.sync(configuration);
            candidate.value = freezeDeep(clone(configuration)); importBuffer.value = ''; version.value++; review.value = null;
            outcome.value = 'Кандидат принят в план. Требуется общий review и подтверждение.';
        });
    }
    async function prepare() {
        showReview.value = true; outcome.value = '';
        await run(async () => {
            if (task.value) throw Error('Завершите локальную задачу перед общим review.');
            if (voiceBusy.value) throw Error('Завершите запись и распознавание перед review.');
            if (importBuffer.value.trim()) throw new AdminError([{ code: 'import_pending', path: '/import', message: 'Непроверенный импорт блокирует общее применение. Исправьте документ или явно отбросьте импорт в Advanced.' }]);
            await ensure(); review.value = null;
            const stored = await read('/api/config');
            if (stored.hash !== base.value!.hash) throw Error('Сохранённая конфигурация изменилась в другой вкладке. План сохранён локально; экспортируйте его и перечитайте основу в Advanced.');
            const active = (await read('/api/config/active')).active;
            const next = freezeDeep(clone(candidate.value!)), reviewedVersion = version.value;
            const validation = await api('/api/config/validate', { configuration: next });
            const storedDiff = await api('/api/config/diff', { configuration: next, base: 'stored' });
            const activeDiff = await api('/api/config/diff', { configuration: next, base: 'active' });
            if (version.value !== reviewedVersion || task.value) throw Error('План изменился во время проверки; повторите review.');
            review.value = { candidate: next, stored, active, version: reviewedVersion, storedDiff, activeDiff, validation };
            if (!validation.apply_ready && !setup.value?.active) issues.value = [{ code: 'not_apply_ready', path: '', message: 'Конфигурация валидна, но runtime preflight недоступен. Применение пока не готово.' }];
        });
    }
    const equalIdentity = (a: Identity | null, b: Identity | null) => a?.hash === b?.hash && a?.revision === b?.revision;
    async function confirm() {
        if (!canConfirm.value) { issues.value = [{ code: 'review_required', path: '', message: 'Требуется новый review перед применением.' }]; return; }
        const reviewed = review.value!;
        await run(async () => {
            let saved: CatalogDto | null = null;
            try {
                await checkSetup();
                const stored = await read('/api/config'), active = (await read('/api/config/active')).active;
                if (!equalIdentity(stored, reviewed.stored) || !equalIdentity(active, reviewed.active) || active?.generation_id !== reviewed.active?.generation_id || version.value !== reviewed.version || task.value) throw Error('Основа или план изменились после review. Требуется новая проверка.');
                if (setup.value?.active) {
                    await bridge!.apply();
                    saved = await read('/api/config');
                    base.value = saved; candidate.value = saved.configuration;
                } else {
                saved = await api<CatalogReply>('/api/config', { configuration: reviewed.candidate, expected: identity(reviewed.stored) }, 'PUT');
                base.value = saved; candidate.value = saved.configuration;
                await api('/api/config/apply', { expected: identity(saved) });
                }
                const activated = (await read('/api/config/active')).active;
                if (!equalIdentity(activated, saved)) throw Error('Активная версия не совпала с сохранённой.');
                outcome.value = 'Изменения применены. Активная версия проверена.';
            } catch (error) {
                outcome.value = saved ? 'Версия сохранена, но не активирована' : 'Применение не подтверждено';
                // A lost response is not a reason to repeat Save, Apply or execution.
                try {
                    const stored = await read('/api/config'), active = (await read('/api/config/active')).active;
                    base.value = stored;
                    if (saved && equalIdentity(active, saved)) outcome.value = 'Активная версия подтверждена после повторного чтения.';
                    else if (stored.hash !== reviewed.stored.hash) outcome.value = 'Версия сохранена, но не активирована';
                    if (!equalIdentity(active, stored)) outcome.value += '. Сохранённая и активная версии различаются.';
                } catch { outcome.value += '. Проверить состояние не удалось; повторите только чтение статуса.'; }
                throw error;
            } finally { review.value = null; version.value++; await refresh(); }
        });
    }
    async function reset() { await run(async () => { await checkSetup(); base.value = await read('/api/config'); candidate.value = base.value.configuration; task.value = null; review.value = null; version.value++; importBuffer.value = ''; outcome.value = ''; }); }
    const changes = computed(() => review.value ? readableDiff(review.value.stored.configuration, review.value.candidate) : []);
    const activeChanges = computed(() => review.value?.active ? readableDiff(review.value.active.configuration, review.value.candidate) : []);
    return { base, candidate, task, review, version, busy, voiceBusy, importBuffer, advanced, showReview, issues, outcome, setup, draft, dirty, canConfirm, changes, activeChanges,
        connectSetup, begin, cancel, stage, prepare, confirm, replaceCandidate, reset, run, checkSetup, refreshSetupStatus };
}
export const manual = createManualStore();
