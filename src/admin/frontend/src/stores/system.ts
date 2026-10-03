import { computed, readonly, shallowRef } from 'vue';
import { readApi } from '../api/client.js';
import { devices, sameGeneration, zones } from '../adapters/catalog.js';
import type { ApiResponses, ReadEndpoint } from '../types/wire.js';

const endpoints: readonly ReadEndpoint[] = ['/api/status', '/api/config', '/api/config/active', '/api/devices/state', '/api/executions'];
type Snapshot = { readonly [P in ReadEndpoint]?: ApiResponses[P] };
export function createSystemStore(reader: typeof readApi = readApi) {
    const snapshot = shallowRef<Snapshot>({});
    const errors = shallowRef<Readonly<Record<string, string>>>({});
    const loading = shallowRef(false);
    const updatedAt = shallowRef<number | null>(null);
    const status = computed(() => snapshot.value['/api/status'] ?? null);
    const stored = computed(() => snapshot.value['/api/config'] ?? null);
    const active = computed(() => snapshot.value['/api/config/active']?.active ?? null);
    const states = computed(() => snapshot.value['/api/devices/state'] ?? null);
    const configuration = computed(() => active.value?.configuration ?? stored.value?.configuration ?? null);
    const deviceViews = computed(() => configuration.value ? devices(configuration.value, active.value, errors.value['/api/devices/state'] || errors.value['/api/config/active'] ? null : states.value) : []);
    const zoneViews = computed(() => configuration.value ? zones(configuration.value) : []);
    const stateMismatch = computed(() => !!states.value && !sameGeneration(active.value, states.value.active));
    const pending = computed(() => !!stored.value && stored.value.hash !== active.value?.hash);
    const executions = computed(() => snapshot.value['/api/executions']?.executions ?? []);
    const health = computed(() => {
        if (errors.value['/api/status'] || !status.value) return 'Статус недоступен';
        if (status.value.health?.stopping) return 'Остановка';
        if (status.value.runtime_busy) return 'Обрабатывает запрос';
        switch (status.value.health?.overall) {
            case 'ready': return 'Система готова';
            case 'starting': return 'Запуск системы';
            case 'failed': return 'Сбой запуска';
            case 'degraded': return 'Ограниченный режим';
            default: return status.value.runtime_ready ? 'Система готова' : 'Ограниченный режим';
        }
    });
    let pendingRefresh: Promise<void> | null = null;
    async function refresh(afterMutation = false) {
        if (pendingRefresh) { await pendingRefresh; if (!afterMutation) return; }
        const request = loadSnapshot(); pendingRefresh = request;
        try { await request; } finally { if (pendingRefresh === request) pendingRefresh = null; }
    }
    async function loadSnapshot() {
        loading.value = true;
        const next: Record<string, unknown> = { ...snapshot.value };
        const failures: Record<string, string> = {};
        // Legacy IPC clients serialize state/history reads through runtimeOperation.
        // Concurrent GETs can produce runtime_busy even without a command. Keep
        // the five reads sequential; a failed endpoint doesn't block the rest.
        for (const path of endpoints) {
            try { next[path] = await reader(path); }
            catch (error) { failures[path] = error instanceof Error ? error.message : 'Нет связи с Firefly'; }
        }
        snapshot.value = next as Snapshot;
        errors.value = failures;
        updatedAt.value = Date.now();
        loading.value = false;
    }
    return { snapshot: readonly(snapshot), errors: readonly(errors), loading: readonly(loading), updatedAt: readonly(updatedAt),
        status, stored, active, states, configuration, devices: deviceViews, zones: zoneViews, stateMismatch, pending, executions, health, refresh };
}
export const system = createSystemStore();
