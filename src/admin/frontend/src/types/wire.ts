// Wire names stay compatible with the C++ API. Unknown fields are retained,
// read responses remain immutable; editing uses a separate complete clone.
export interface WireObject { readonly [key: string]: unknown }
export type Scalar = string | number | boolean;
export interface Identity extends WireObject { readonly revision: number; readonly hash: string }
export interface ZoneDto extends WireObject {
    readonly id: string; readonly name: string;
    readonly speech?: { readonly mentions: readonly string[]; readonly locations: readonly string[] };
    readonly metadata?: WireObject & { readonly parent_id?: string | null; readonly container?: boolean; readonly description?: string };
}
export interface FieldDto extends WireObject {
    readonly id: string; readonly name: string; readonly type: string; readonly unit?: string;
    readonly required?: boolean; readonly description?: string; readonly initial?: Scalar;
    readonly minimum?: number; readonly maximum?: number; readonly min_length?: number; readonly max_length?: number; readonly values?: readonly string[];
}
export interface DeviceDto extends WireObject {
    readonly id: string; readonly name: string; readonly kind: string;
    readonly room_id: string | null; readonly enabled: boolean;
    readonly spoken_aliases?: readonly string[]; readonly action_ids: readonly string[]; readonly state?: readonly FieldDto[];
    readonly metadata?: WireObject & { readonly description?: string; readonly type?: string };
}
export interface ActionDto extends WireObject {
    readonly id: string; readonly name: string; readonly enabled: boolean;
    readonly operation?: string; readonly speech_aliases?: readonly string[];
    readonly assignments?: readonly (WireObject & { readonly field: string; readonly argument?: string; readonly value?: Scalar })[];
    readonly description?: string; readonly parameters: readonly FieldDto[];
}
export interface HttpDto extends WireObject { readonly method: string; readonly path?: string; readonly url?: string; readonly body?: Readonly<Record<string, Scalar>>; readonly argument?: string; readonly timeout_ms?: number; readonly expected_status?: number; readonly content_type?: string; readonly headers?: Readonly<Record<string, string>> }
export interface MqttBindingDto extends WireObject {
    readonly topic: string; readonly payload: string; readonly qos: number; readonly retain: boolean;
}
export interface MqttObservationDto extends WireObject {
    readonly topic: string; readonly format: 'text' | 'json'; readonly qos: number;
    readonly field?: string; readonly true_payload?: string; readonly false_payload?: string;
}
export interface BindingDto extends WireObject {
    readonly target_id: string; readonly action_id: string; readonly connection_id?: string;
    readonly executor?: string; readonly http?: HttpDto; readonly mqtt?: MqttBindingDto;
}
export interface ConnectionDto extends WireObject {
    readonly id: string; readonly name: string; readonly enabled: boolean; readonly type: string;
    readonly timeout_ms?: number; readonly http?: WireObject & { readonly base_url: string; readonly base_path?: string };
    readonly network?: WireObject & { readonly host: string; readonly port: number };
    readonly mqtt?: WireObject & { readonly tls: boolean; readonly credentials_id?: string };
}
export interface ConfigurationDto extends WireObject {
    readonly schema_version: number; readonly revision: number;
    readonly system: WireObject & { readonly id: string; readonly name: string;
        readonly voice_dialogue?: { readonly responses: 'off' | 'brief' | 'detailed'; readonly questions_enabled: boolean };
        readonly voice_input?: { readonly inline_commands: boolean } };
    readonly rooms: readonly ZoneDto[]; readonly devices: readonly DeviceDto[]; readonly actions: readonly ActionDto[];
    readonly bindings: readonly BindingDto[]; readonly connections?: readonly ConnectionDto[];
    readonly observations?: readonly (WireObject & { readonly target_id: string; readonly connection_id: string; readonly executor?: string; readonly http?: HttpDto; readonly mqtt?: MqttObservationDto; readonly fields?: Readonly<Record<string, string>> })[];
    readonly retired_ids?: readonly string[];
}
export interface CatalogDto extends Identity { readonly configuration: ConfigurationDto; readonly generation_id?: number }
export interface StatusDto extends WireObject {
    readonly ok: true; readonly runtime_ready: boolean; readonly runtime_busy: boolean;
    readonly needs_apply: boolean; readonly active: Identity | null; readonly stored?: Identity;
    readonly health?: WireObject & { readonly overall: string; readonly stopping?: boolean };
    readonly speech?: WireObject & { readonly state: string; readonly error: string; readonly voice: string };
    readonly voice_input?: { readonly inline_commands: boolean; readonly mode: 'auto' | 'after-beep'; readonly confirmation_enabled?: boolean };
    readonly voice?: string; readonly stt_ready?: boolean; readonly runtime_error?: string;
}
export interface PropertyDto extends WireObject {
    readonly value: Scalar | null; readonly knowledge: string; readonly stale: boolean;
    readonly updated_at_ms: number; readonly observed_at_ms: number | null;
    readonly last_observed_value?: Scalar | null; readonly observation_age_ms?: number | null;
}
export interface DeviceStateDto extends WireObject {
    readonly targetId: string; readonly properties: Readonly<Record<string, PropertyDto>>;
    readonly connectivity: string; readonly last_success_at_ms: number | null; readonly last_failure_at_ms: number | null;
    readonly availability?: 'online' | 'offline' | 'unknown'; readonly availability_reason?: string;
    readonly last_observation_at_ms?: number | null; readonly observation_age_ms?: number | null;
}
export interface StatesDto extends WireObject {
    readonly ok: true; readonly active: CatalogDto | null; readonly devices: readonly DeviceStateDto[];
}
export interface ExecutionDto extends WireObject {
    readonly sequence: number; readonly generation_id: number; readonly timestamp_ms: number; readonly source: string;
    readonly execution: WireObject & { readonly targetId: string; readonly actionId: string; readonly success: boolean; readonly status: string; readonly error: string };
}
export interface ExecutionsDto extends WireObject {
    readonly ok: true; readonly active: CatalogDto | null; readonly executions: readonly ExecutionDto[];
}
export interface ApiResponses {
    '/api/status': StatusDto;
    '/api/config': CatalogDto;
    '/api/config/active': WireObject & { readonly ok: true; readonly active: CatalogDto | null };
    '/api/devices/state': StatesDto;
    '/api/executions': ExecutionsDto;
}
export type ReadEndpoint = keyof ApiResponses;
