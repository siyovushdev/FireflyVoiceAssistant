import { decodeResponse } from './client.js';
import { setupAuth } from './setup.js';
import { queued } from './transport.js';
import type { CatalogDto, ConfigurationDto, Identity, WireObject } from '../types/wire.js';
export interface Issue { code: string; path: string; message: string }
export class AdminError extends Error {
    constructor(readonly issues: Issue[], readonly uncertain = false) { super(issues.map(e => e.message).join('\n')); }
}
export interface AdminReply extends WireObject {
    readonly ok: boolean; readonly errors?: readonly Issue[]; readonly apply_ready?: boolean;
    readonly changes?: readonly WireObject[]; readonly configuration?: ConfigurationDto;
}
export type CatalogReply = CatalogDto & AdminReply;
export interface SetupStatus extends AdminReply { readonly active: boolean; readonly status: string }
const reads = ['/api/setup/status', '/api/config/revisions', '/api/config/export', '/api/dashboard', '/api/mqtt/credentials', '/api/mqtt/local-broker'];
const posts = ['/api/mqtt/local-broker', '/api/mqtt/credentials', '/api/setup/start', '/api/setup/resume', '/api/setup/heartbeat', '/api/setup/message', '/api/setup/draft', '/api/setup/apply', '/api/setup/cancel', '/api/admin/copilot', '/api/admin/copilot/accept', '/api/admin/language-profile', '/api/admin/action-draft', '/api/admin/draft', '/api/config/validate', '/api/config/diff', '/api/config/apply', '/api/config/import', '/api/config/restore-candidate', '/api/config/reconcile', '/api/config/migrate', '/api/connections/probe', '/api/devices/command', '/api/devices/poll', '/api/test-command'];
export async function adminRequest<T = AdminReply>(path: string, body?: unknown, method = body === undefined ? 'GET' : 'POST'): Promise<T> {
    if (!(method === 'GET' && reads.includes(path) || method === 'POST' && posts.includes(path) || (method === 'PUT' && (path === '/api/config' || path === '/api/mqtt/credentials') || method === 'DELETE' && path === '/api/mqtt/credentials'))) throw Error('Недопустимая операция Admin');
    if (setupAuth.token && body && typeof body === 'object' && !path.startsWith('/api/setup/')) body = { ...body, session_id: setupAuth.token };
    return queued(async () => {
        let response: Response;
        try { response = await fetch(path, { method, body: body === undefined ? undefined : JSON.stringify(body),
            headers: { 'Content-Type': 'application/json', 'X-Firefly-Admin': '1' }, credentials: 'same-origin', redirect: 'error', cache: 'no-store', signal: AbortSignal.timeout(path === '/api/setup/message' ? 660000 : 120000) }); }
        catch { throw new AdminError([{ code: 'network_unknown', path: '', message: 'Ответ не получен. Результат операции неизвестен; автоматического повтора нет.' }], method !== 'GET'); }
        let reply: AdminReply;
        try { reply = await response.json() as AdminReply; }
        catch { throw new AdminError([{ code: 'invalid_response', path: '', message: 'Ответ не удалось прочитать; проверьте сохранённую и активную версии.' }], method !== 'GET'); }
        if (!reply || typeof reply !== 'object' || Array.isArray(reply)) throw new AdminError([{ code: 'invalid_response', path: '', message: 'Неподдерживаемый ответ сервера; результат операции нужно проверить.' }], method !== 'GET');
        if (!response.ok || reply.ok === false) throw new AdminError(reply.errors?.length ? [...reply.errors] : [{ code: `http_${response.status}`, path: '', message: `Операция отклонена (HTTP ${response.status})` }]);
        if (path !== '/api/config/export' && reply.ok !== true) throw new AdminError([{ code: 'invalid_response', path: '', message: 'Сервер не подтвердил результат операции.' }], method !== 'GET');
        if (path === '/api/config' && method === 'PUT') {
            try { decodeResponse('/api/config', reply); }
            catch { throw new AdminError([{ code: 'invalid_save_response', path: '', message: 'Ответ Save неполный. Перечитайте stored/active; повтор Save не выполняется.' }], true); }
        }
        return reply as T;
    });
}
export const identity = (catalog: Identity): { revision: number; hash: string } => ({ revision: catalog.revision, hash: catalog.hash });
export function issuesOf(error: unknown): Issue[] { return error instanceof AdminError ? error.issues : [{ code: 'local', path: '', message: error instanceof Error ? error.message : String(error) }]; }
