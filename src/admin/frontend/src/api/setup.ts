import { reactive } from 'vue';
import { adminRequest, type AdminReply, type SetupStatus } from './admin.js';
import type { ConfigurationDto, WireObject } from '../types/wire.js';
export interface Session extends SetupStatus {
    readonly version?: number;
    readonly session_id?: string;
    readonly configuration?: ConfigurationDto;
    readonly questions?: readonly string[];
    readonly language_warning?: string;
    readonly message_incomplete?: boolean;
    readonly limits?: { readonly message_codepoints?: number; readonly message_bytes?: number };
    readonly proposal_scope?: 'configuration';
    readonly progress?: { readonly phase?: string; readonly step?: number; readonly total_steps?: number };
}
export interface Proposal extends WireObject { readonly configuration: ConfigurationDto; readonly operations: readonly WireObject[]; readonly rejected?: boolean }
export interface CopilotReply extends AdminReply { readonly draft: Proposal }
// Token is tab-scoped, never a URL, log or model input.
export const setupAuth = reactive({ token: '' });
export function setupRequest<T = Session>(route: string, body?: Record<string, unknown>): Promise<T> {
    return adminRequest<T>('/api/setup/' + route, body === undefined ? undefined : { ...body, ...(setupAuth.token ? { session_id: setupAuth.token } : {}) });
}
