import type { ApiResponses, ReadEndpoint, WireObject } from '../types/wire.js';

export class ApiError extends Error {
    constructor(message: string, readonly code: string) { super(message); }
}
function invalid(): never { throw new ApiError('Ответ Firefly имеет неподдерживаемый формат.', 'invalid_response'); }
function object(value: unknown): asserts value is WireObject {
    if (!value || typeof value !== 'object' || Array.isArray(value)) invalid();
}
function string(value: unknown): void { if (typeof value !== 'string') invalid(); }
function number(value: unknown): void { if (typeof value !== 'number' || !Number.isFinite(value)) invalid(); }
function boolean(value: unknown): void { if (typeof value !== 'boolean') invalid(); }
function list(value: unknown, check: (item: unknown) => void): void {
    if (!Array.isArray(value)) invalid();
    value.forEach(check);
}
function nullableString(value: unknown): void { if (value !== null) string(value); }
function nullableNumber(value: unknown): void { if (value !== null) number(value); }
function identity(value: unknown): void { object(value); number(value.revision); string(value.hash); }
function entity(value: unknown): void { object(value); string(value.id); string(value.name); }
function field(value: unknown): void { object(value); entity(value); string(value.type); if (value.unit !== undefined) string(value.unit); }
function metadata(value: unknown): void {
    if (value === undefined) return;
    object(value);
    if (value.description !== undefined) string(value.description);
    if (value.type !== undefined) string(value.type);
    if (value.parent_id !== undefined) nullableString(value.parent_id);
    if (value.container !== undefined) boolean(value.container);
}
function catalog(value: unknown): void {
    object(value); identity(value);
    if (value.generation_id !== undefined) number(value.generation_id);
    const config = value.configuration; object(config);
    number(config.schema_version); number(config.revision); entity(config.system);
    list(config.rooms, item => { object(item); entity(item); metadata(item.metadata); });
    list(config.devices, item => {
        object(item); entity(item); string(item.kind); nullableString(item.room_id); boolean(item.enabled);
        list(item.action_ids, string); if (item.state !== undefined) list(item.state, field); metadata(item.metadata);
    });
    list(config.actions, item => {
        object(item); entity(item); boolean(item.enabled); list(item.parameters, field);
        if (item.description !== undefined) string(item.description);
    });
    list(config.bindings, item => { object(item); string(item.target_id); string(item.action_id); if (item.connection_id !== undefined) string(item.connection_id); });
    if (config.connections !== undefined) list(config.connections, item => { object(item); entity(item); boolean(item.enabled); string(item.type); });
    if (config.observations !== undefined) list(config.observations, item => { object(item); string(item.target_id); string(item.connection_id); });
}
function active(value: unknown): void { if (value !== null) catalog(value); }
export function decodeResponse<P extends ReadEndpoint>(path: P, value: unknown): ApiResponses[P] {
    object(value);
    if (value.ok !== true) throw new ApiError('Firefly не смог предоставить данные. Попробуйте обновить страницу.', 'api_rejected');
    if (path === '/api/config') catalog(value);
    if (path === '/api/config/active') active(value.active);
    if (path === '/api/status') {
        boolean(value.runtime_ready); boolean(value.runtime_busy); boolean(value.needs_apply);
        if (value.active !== null) identity(value.active);
        if (value.stored !== undefined) identity(value.stored);
        if (value.health !== undefined) {
            object(value.health); string(value.health.overall);
            if (value.health.stopping !== undefined) boolean(value.health.stopping);
        }
        if (value.runtime_error !== undefined) string(value.runtime_error);
        if (value.stt_ready !== undefined) boolean(value.stt_ready);
        if (value.voice !== undefined) string(value.voice);
    }
    if (path === '/api/devices/state') {
        active(value.active);
        list(value.devices, item => {
            object(item); string(item.targetId); string(item.connectivity);
            nullableNumber(item.last_success_at_ms); nullableNumber(item.last_failure_at_ms); object(item.properties);
            Object.values(item.properties).forEach(property => {
                object(property); string(property.knowledge); boolean(property.stale);
                number(property.updated_at_ms); nullableNumber(property.observed_at_ms);
                if (property.value !== null && !['string', 'boolean', 'number'].includes(typeof property.value)) invalid();
                if (typeof property.value === 'number') number(property.value);
            });
        });
    }
    if (path === '/api/executions') {
        active(value.active);
        list(value.executions, item => {
            object(item); number(item.sequence); number(item.generation_id); number(item.timestamp_ms); string(item.source);
            object(item.execution); string(item.execution.targetId); string(item.execution.actionId);
            boolean(item.execution.success); string(item.execution.status); string(item.execution.error);
        });
    }
    // Validation never reconstructs/strips the wire document; rare fields survive.
    return freezeDeep(value) as unknown as ApiResponses[P];
}
export function freezeDeep<T>(value: T): T {
    if (value && typeof value === 'object' && !Object.isFrozen(value)) {
        Object.values(value).forEach(freezeDeep);
        Object.freeze(value);
    }
    return value;
}
export async function readApi<P extends ReadEndpoint>(path: P): Promise<ApiResponses[P]> {
    // Deliberately no generic method/body API in B1.
    const allowed: readonly string[] = ['/api/status', '/api/config', '/api/config/active', '/api/devices/state', '/api/executions'];
    if (!allowed.includes(path)) throw new ApiError('Этот запрос недоступен в режиме просмотра.', 'not_read_only');
    const response = await fetch(path, { method: 'GET', cache: 'no-store', credentials: 'same-origin', redirect: 'error', signal: AbortSignal.timeout(10000) });
    if (!response.ok) throw new ApiError(`Данные Firefly недоступны (HTTP ${response.status}).`, `http_${response.status}`);
    return decodeResponse(path, await response.json());
}
