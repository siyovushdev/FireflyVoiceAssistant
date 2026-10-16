import type { ActionDto, ConfigurationDto, DeviceDto, FieldDto, Scalar, ConnectionDto, BindingDto, HttpDto, MqttBindingDto } from '../types/wire.js';
export type Mutable<T> = T extends object ? { -readonly [K in keyof T]: Mutable<T[K]> } : T;
export type Draft = Mutable<ConfigurationDto>;
export type Field = Mutable<FieldDto>;
export type Device = Mutable<DeviceDto>;
export type Action = Mutable<ActionDto>;
export function clone<T>(value: T): Mutable<T> { return JSON.parse(JSON.stringify(value)) as Mutable<T>; }
export const lines = (value: string): string[] => [...new Set(value.split('\n').map(s => s.trim()).filter(Boolean))];
export const rawLines = (value: string): string[] => value.split('\n');
export function allocate(config: ConfigurationDto, prefix: string): string {
    const ids = new Set([config.system.id, ...config.rooms.map(x => x.id), ...config.devices.map(x => x.id), ...config.actions.map(x => x.id), ...(config.connections ?? []).map(x => x.id), ...(config.retired_ids ?? [])]);
    let n = 1; while (ids.has(`${prefix}_${n}`)) n++;
    return `${prefix}_${n}`;
}
export function newField(id = 'value', type = 'integer'): Field {
    const field: Field = { id, name: 'Значение', type, required: true, description: '', unit: '' };
    changeType(field, type); return field;
}
export function initial(field: FieldDto): Scalar { return field.type === 'boolean' ? false : field.type === 'string' ? '' : field.type === 'enum' ? field.values?.[0] ?? '' : field.minimum ?? 0; }
export function changeType(field: Field, type: string) {
    for (const key of ['minimum', 'maximum', 'min_length', 'max_length', 'values']) delete field[key];
    field.type = type;
    if (type === 'integer' || type === 'number') { field.minimum = 0; field.maximum = 100; }
    if (type === 'string') { field.min_length = 0; field.max_length = 128; }
    if (type === 'enum') field.values = ['normal', 'eco'];
    if ('initial' in field) field.initial = initial(field);
}
export function powerOnly(config: Draft, device: Device) {
    const field = newField('power', 'boolean'); field.name = 'Питание'; field.initial = false;
    device.state = [field]; device.action_ids = [];
    for (const on of [true, false]) {
        const id = allocate(config, 'action');
        const action: Action = { id, name: on ? 'Включить' : 'Выключить', enabled: true, operation: 'mock.assign.v1', description: 'Питание устройства',
            parameters: [], speech_aliases: on ? ['включи', 'включить', 'включите'] : ['выключи', 'выключить', 'выключите'], assignments: [{ field: 'power', value: on }] };
        config.actions.push(action); device.action_ids.push(id);
    }
}
export function consumers(config: ConfigurationDto, connectionId: string): string[] {
    const ids = new Set([...config.bindings.filter(b => b.connection_id === connectionId).map(b => b.target_id), ...(config.observations ?? []).filter(o => o.connection_id === connectionId).map(o => o.target_id)]);
    return [...ids].map(id => config.devices.find(d => d.id === id)?.name ?? id);
}
// The manual wizard and explicit-facts setup use the same construction defaults.
export function newHttpConnection(config: ConfigurationDto): Mutable<ConnectionDto> {
    return { id: allocate(config, 'connection_http'), name: '', type: 'http', enabled: true, timeout_ms: 2000, http: { base_url: '', base_path: '' } };
}
export function defaultHttpMapping(): Mutable<HttpDto> {
    return { method: 'POST', path: '', body: {}, timeout_ms: 0, content_type: 'application/json', expected_status: 200 };
}
export function newMqttConnection(config: ConfigurationDto): Mutable<ConnectionDto> {
    return { id: allocate(config, 'connection_mqtt'), name: '', type: 'mqtt', enabled: true,
        timeout_ms: 2000, network: { host: '127.0.0.1', port: 1883 }, mqtt: { tls: false } };
}
export function defaultMqttMapping(): Mutable<MqttBindingDto> {
    return { topic: '', payload: '', qos: 0, retain: false };
}
export function projectConnectivity(config: ConfigurationDto, connection: ConnectionDto | null, targetId: string,
    mappings: readonly BindingDto[], observation?: NonNullable<ConfigurationDto['observations']>[number],
    transport?: 'http' | 'mqtt', replaceOtherObservation = false): Draft {
    const next = clone(config);
    if (connection) {
        const index = (next.connections ??= []).findIndex(c => c.id === connection.id);
        if (index < 0) next.connections.push(clone(connection)); else next.connections[index] = clone(connection);
    }
    if (targetId) {
        // A device can have HTTP and MQTT actions simultaneously. Never remove
        // the other transport just because this editor didn't render its fields.
        next.bindings = [...next.bindings.filter(b =>
            b.target_id !== targetId || (transport !== undefined && b.executor !== transport)), ...clone(mappings)];
        // This MVP supports one observation per target. Replacing a readback
        // of another transport must be a separate, explicitly approved choice.
        next.observations = (next.observations ?? []).filter(o => o.target_id !== targetId ||
            (transport !== undefined && o.executor !== transport && !replaceOtherObservation));
        if (observation) next.observations.push(clone(observation));
    }
    return next;
}
function connectivitySummary(value: Record<string, unknown>, config: ConfigurationDto): string | null {
    if (value.type === 'mqtt' && value.network && typeof value.network === 'object') {
        const network = value.network as { host?: string; port?: number };
        const mqtt = value.mqtt as { tls?: boolean; credentials_id?: string } | undefined;
        return String(value.name ?? 'MQTT') + ' · MQTT ' + String(network.host) + ':' + String(network.port) +
            (mqtt?.tls ? ' · TLS' : '') + (mqtt?.credentials_id ? ' · credentials' : '') +
            (value.enabled === false ? ' · отключено' : '');
    }
    if (value.executor === 'mqtt' && value.mqtt && typeof value.mqtt === 'object') {
        const mqtt = value.mqtt as { topic?: string; payload?: string; format?: string };
        const device = config.devices.find(d => d.id === value.target_id);
        const action = config.actions.find(a => a.id === value.action_id);
        return [device?.name, action?.name, 'MQTT ' + (mqtt.topic ?? ''),
            mqtt.payload === undefined ? 'State: ' + (mqtt.format ?? '') : 'Payload: ' + mqtt.payload,
            value.fields ? 'Поля: ' + JSON.stringify(value.fields) : ''].filter(Boolean).join(' · ');
    }
    if (!value.http || typeof value.http !== 'object') return null;
    const http = value.http as Record<string, unknown>;
    if ('base_url' in http) return `${value.name} · ${http.base_url}${http.base_path ?? ''}${value.enabled === false ? ' · отключено' : ''}`;
    const device = config.devices.find(d => d.id === value.target_id);
    const connection = config.connections?.find(c => c.id === value.connection_id);
    const action = config.actions.find(a => a.id === value.action_id);
    const endpoint = http.url ?? `${connection?.http?.base_url ?? ''}${connection?.http?.base_path ?? ''}${http.path ?? ''}`;
    const parts = [device?.name, action?.name, `${http.method} ${endpoint}`].filter(Boolean);
    if (http.body && typeof http.body === 'object' && Object.keys(http.body).length)
        parts.push('Значения: ' + Object.entries(http.body).map(([key, v]) => `${key} = ${typeof v === 'boolean' ? v ? 'Да' : 'Нет' : JSON.stringify(v)}`).join(', '));
    if (http.argument) parts.push(`Параметр «${action?.parameters.find(p => p.id === http.argument)?.name ?? http.argument}» → поле ${http.argument}`);
    if (value.fields && typeof value.fields === 'object') parts.push('Чтение: ' + Object.entries(value.fields)
        .map(([key, field]) => `${key} → ${device?.state?.find(p => p.id === field)?.name ?? field}`).join(', '));
    return parts.join(' · ');
}
export interface Change { title: string; before: unknown; after: unknown; beforeText: string; afterText: string; consumers: string[] }
export function describeChange(value: unknown, config: ConfigurationDto, key = ''): string {
    if (value === undefined) return 'Отсутствует';
    if (key === 'retired_ids' && Array.isArray(value)) return `Архив: ${value.length} записей`;
    if (key === 'room_id') return config.rooms.find(z => z.id === value)?.name ?? 'Без зоны';
    if (key === 'parent_id') return config.rooms.find(z => z.id === value)?.name ?? 'Без родительской зоны';
    if (key === 'connection_id') return config.connections?.find(c => c.id === value)?.name ?? String(value);
    if (key === 'target_id') return config.devices.find(d => d.id === value)?.name ?? String(value);
    if (key === 'action_id') return config.actions.find(a => a.id === value)?.name ?? String(value);
    if (key === 'action_ids' && Array.isArray(value)) return value.map(id => config.actions.find(a => a.id === id)?.name ?? id).join(', ');
    if (key === 'field') return config.devices.flatMap(d => d.state ?? []).find(f => f.id === value)?.name ?? String(value);
    if (key === 'argument') return config.actions.flatMap(a => a.parameters).find(p => p.id === value)?.name ?? String(value);
    if (key === 'body' || key === 'headers') return JSON.stringify(value);
    if (key === 'type') return ({ boolean: 'Да / Нет', integer: 'Целое число', number: 'Число', enum: 'Выбор из списка', string: 'Текст', location: 'Зона', equipment: 'Оборудование' } as Record<string, string>)[String(value)] ?? String(value);
    if (value === null) return 'Не задано';
    if (typeof value === 'boolean') return value ? 'Да' : 'Нет';
    if (typeof value !== 'object') return String(value);
    if (Array.isArray(value)) return value.map(v => describeChange(v, config)).join('; ');
    const connectivity = connectivitySummary(value as Record<string, unknown>, config);
    if (connectivity !== null) return connectivity;
    const labels: Record<string, string> = { name: 'Название', description: 'Описание', speech: 'Речь', mentions: 'Обращения', locations: 'Местоположение в команде', metadata: 'Сведения', parent_id: 'Родительская зона', container: 'Зона-группа', room_id: 'Зона', action_ids: 'Действия', spoken_aliases: 'Обращения', speech_aliases: 'Фразы действия', state: 'Свойства', assignments: 'Результаты', parameters: 'Параметры', field: 'Свойство', value: 'Значение', argument: 'Параметр', type: 'Тип', required: 'Обязательно', unit: 'Единица', initial: 'Начальное виртуальное значение', minimum: 'Минимум', maximum: 'Максимум', min_length: 'Минимальная длина', max_length: 'Максимальная длина', values: 'Варианты', connection_id: 'Подключение', target_id: 'Устройство', action_id: 'Действие', http: 'HTTP', method: 'Метод', path: 'Путь', url: 'URL', base_url: 'Endpoint', base_path: 'Префикс', body: 'Точный JSON', fields: 'Поля чтения', timeout_ms: 'Timeout, мс', expected_status: 'Ожидаемый status', content_type: 'Content-Type', headers: 'Headers', enabled: 'Активно' };
    return Object.entries(value).filter(([k, v]) => !['id', 'operation', 'origin', 'confirmed', 'kind', 'executor', 'timeout_ms', 'content_type', 'headers', 'expected_status'].includes(k) && v !== '' && !(Array.isArray(v) && !v.length)).map(([k, v]) => `${labels[k] ?? k}: ${describeChange(v, config, k)}`).join('; ');
}
export function readableDiff(before: ConfigurationDto, after: ConfigurationDto): Change[] {
    const changes: Change[] = [];
    const add = (title: string, a: unknown, b: unknown, affected: string[] = []) => {
        if (JSON.stringify(a) === JSON.stringify(b)) return;
        const beforeText = describeChange(a, before), afterText = describeChange(b, after);
        changes.push({ title, before: a, after: b, beforeText, afterText: afterText === beforeText
            ? afterText + ' · Изменены технические параметры; подробности в расширенном режиме.' : afterText, consumers: affected });
    };
    add('Система', before.system, after.system);
    for (const [key, label] of [['rooms', 'Зона'], ['devices', 'Устройство'], ['actions', 'Действие'], ['connections', 'Подключение']] as const) {
        const previous = before[key] ?? [], next = after[key] ?? [];
        const ids = new Set([...previous, ...next].map(x => x.id));
        for (const id of ids) {
            const a = previous.find(x => x.id === id), b = next.find(x => x.id === id);
            const affected = key === 'connections' ? [...new Set([...consumers(before, id), ...consumers(after, id)])] : key === 'actions' ? [...new Set([...before.devices, ...after.devices].filter(d => d.action_ids.includes(id)).map(d => d.name))] : [];
            add(`${label}: ${b?.name ?? a?.name} · ${!a ? 'добавить' : !b ? 'удалить' : 'изменить'}`, a, b, affected);
        }
    }
    for (const key of ['bindings', 'observations'] as const) {
        const previous = before[key] ?? [], next = after[key] ?? [];
        const keyOf = (x: (typeof previous)[number]) => x.target_id + ('action_id' in x ? ':' + x.action_id : '');
        for (const id of new Set([...previous, ...next].map(keyOf))) {
            const a = previous.find(x => keyOf(x) === id), b = next.find(x => keyOf(x) === id);
            const target = (b ?? a)!.target_id;
            add(`${key === 'bindings' ? 'Настройка действия' : 'Чтение состояния'}: ${[...after.devices, ...before.devices].find(d => d.id === target)?.name ?? target}`, a, b);
        }
    }
    for (const key of new Set([...Object.keys(before), ...Object.keys(after)])) {
        if (!['system', 'rooms', 'devices', 'actions', 'connections', 'bindings', 'observations', 'revision'].includes(key)) add(`Конфигурация: ${key}`, before[key], after[key]);
    }
    return changes;
}
