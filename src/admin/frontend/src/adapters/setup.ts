import { clone, readableDiff } from './editing.js';
import type { ConfigurationDto, WireObject } from '../types/wire.js';
import type { Proposal } from '../api/setup.js';
export interface HttpFact { method: string; path: string; body: Record<string, unknown> }
export interface ConnectionFacts { endpoint: string; on: HttpFact | null; off: HttpFact | null; observe: string; field: string; noObserve: boolean }
export const emptyFacts = (): ConnectionFacts => ({ endpoint: '', on: null, off: null, observe: '', field: '', noObserve: false });
// Conservative extraction of explicit user facts. No URL/body defaults or coercion.
export function collectFacts(previous: ConnectionFacts, text: string, asking = ''): ConnectionFacts {
    const next = clone(previous);
    const endpoint = text.match(/http:\/\/(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?/);
    if (endpoint) next.endpoint = endpoint[0];
    for (const match of text.matchAll(/(POST|PUT|PATCH|GET)\s+(\/[^\s,;{}]+)\s*(\{[^{}]*\})?/g)) {
        const path = match[2].replace(/[.;]$/, '');
        const prefix = text.slice(Math.max(0, match.index! - 35), match.index).toLowerCase();
        if (match[1] === 'GET' && (/состоя|чтени|observe/.test(prefix) || asking === 'observe')) { next.observe = path; next.noObserve = false; }
        else {
            const body = match[3] ? JSON.parse(match[3]) : {};
            if (Object.values(body).some(v => v !== null && typeof v === 'object')) throw Error('Нужен JSON с простыми полями верхнего уровня.');
            const fact = { method: match[1], path, body };
            if (/выключ|\bOFF\b/i.test(prefix) || asking === 'off') next.off = fact;
            else if (/включ|\bON\b/i.test(prefix) || asking === 'on') next.on = fact;
        }
    }
    const field = text.match(/(?:поле|field)\s+([a-zA-Z_][\w]*)/i);
    if (field) next.field = field[1];
    if (/без чтения|чтение не предоставляется|нет observe/i.test(text)) { next.noObserve = true; next.observe = ''; next.field = ''; }
    return next;
}
export function missingFact(f: ConnectionFacts): { key: string; question: string } | null {
    if (!f.endpoint) return { key: 'endpoint', question: 'Какой адрес и протокол использовать? Укажите http://числовой IPv4 и порт, если он отличается от 80.' };
    if (!f.on) return { key: 'on', question: 'Как выполняется включение? Укажите HTTP method, path и точный JSON body.' };
    if (!f.off) return { key: 'off', question: 'Как выполняется выключение? Укажите HTTP method, path и точный JSON body.' };
    if (!f.noObserve && (!f.observe || !f.field)) return { key: 'observe', question: 'Как прочитать состояние: GET path и поле ответа? Либо явно укажите «без чтения».' };
    return null;
}
export const physicalInput = (text: string) => /(?:\d{1,3}\.){3}\d{1,3}|https?:|\b(?:POST|GET|PUT|PATCH|MQTT)\b|\{\s*"|подключ|контроллеру/i.test(text);
export function supportsPowerContract(config: ConfigurationDto, target: string): boolean {
    const device = config.devices.find(d => d.id === target);
    return !!device?.state?.some(f => f.id === 'power' && f.type === 'boolean') && [true, false].every(value => config.actions.some(a => device.action_ids.includes(a.id) && a.assignments?.some(x => x.field === 'power' && x.value === value)));
}
export function connectivityInput(config: ConfigurationDto, target: string, facts: ConnectionFacts, shared: string): string {
    if (missingFact(facts)) throw Error('Сначала уточните недостающие сведения.');
    const device = config.devices.find(d => d.id === target);
    if (!device) throw Error('Выберите устройство.');
    if (!supportsPowerContract(config, target)) throw Error('У устройства нет объявленных ON/OFF возможностей. Укажите возможности вручную; AI не будет их угадывать.');
    return `Настрой только HTTP подключение для ${device.name} (${target}). ${shared === 'all' ? 'Изменить общее подключение для всех показанных устройств.' : 'Создать отдельное подключение только для выбранного устройства.'}\nЯвные факты пользователя: ${JSON.stringify(facts)}. Не меняй семантику. ON/OFF — существующие действия питания, Observe относится к существующему boolean свойству power. Никаких других сетевых фактов.`;
}
const equal = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
export function verifyConnectivity(base: ConfigurationDto, proposal: Proposal, target: string, facts: ConnectionFacts, shared: string): ConfigurationDto {
    if (proposal.rejected || !proposal.configuration || !Array.isArray(proposal.operations) || !proposal.operations.length) throw Error('AI не подготовил проверяемое предложение. План этого запроса не принят.');
    if (proposal.operations.some(op => !['set_http_connection', 'set_connection_binding', 'set_http_observe'].includes(String(op.op)))) throw Error('Смешанное или неизвестное AI-предложение отклонено целиком.');
    const next = proposal.configuration;
    for (const key of new Set([...Object.keys(base), ...Object.keys(next)])) if (!['connections', 'bindings', 'observations'].includes(key) && !equal(base[key], next[key])) throw Error('Подключение не может менять семантические сущности.');
    for (const connection of next.connections ?? []) {
        const before = base.connections?.find(c => c.id === connection.id);
        if (equal(before, connection)) continue;
        if (connection.type !== 'http' || connection.http?.base_url !== facts.endpoint || connection.http?.base_path) throw Error('AI предложил адрес или протокол вне явных фактов.');
        if ((base.bindings.some(b => b.connection_id === connection.id && b.target_id !== target) || base.observations?.some(o => o.connection_id === connection.id && o.target_id !== target)) && shared !== 'all') throw Error('Для изменения общего подключения нужен явный выбор всех потребителей.');
    }
    for (const before of base.connections ?? []) if (!next.connections?.some(c => c.id === before.id)) throw Error('Удаление подключения не запрошено.');
    for (const binding of next.bindings) {
        const before = base.bindings.find(b => b.target_id === binding.target_id && b.action_id === binding.action_id);
        if (equal(before, binding)) continue;
        const action = base.actions.find(a => a.id === binding.action_id);
        const assignment = action?.assignments?.find(a => a.field === 'power');
        const fact = assignment?.value === true ? facts.on : assignment?.value === false ? facts.off : null;
        const http = binding.http;
        if (shared === 'separate' && (base.bindings.some(b => b.connection_id === binding.connection_id && b.target_id !== target) || base.observations?.some(o => o.connection_id === binding.connection_id && o.target_id !== target))) throw Error('Выбрано отдельное подключение; AI не может привязать устройство к общему.');
        if (binding.target_id !== target || binding.executor !== 'http' || !fact || !http || http.method !== fact.method || http.path !== fact.path || !equal(http.body ?? {}, fact.body) || http.argument || http.url) throw Error('AI изменил действие или HTTP contract вне явных фактов.');
        if (http.headers && Object.keys(http.headers).length) throw Error('Незапрошенные headers/credentials отклонены.');
        if (next.connections?.find(c => c.id === binding.connection_id)?.http?.base_url !== facts.endpoint) throw Error('Адрес binding не совпадает с явными фактами.');
    }
    for (const before of base.bindings) if (!next.bindings.some(b => b.target_id === before.target_id && b.action_id === before.action_id)) throw Error('Удаление mappings не запрошено.');
    for (const observation of next.observations ?? []) {
        const before = base.observations?.find(o => o.target_id === observation.target_id);
        if (equal(before, observation)) continue;
        if (facts.noObserve || observation.target_id !== target || observation.http?.method !== 'GET' || observation.http.path !== facts.observe || !equal(observation.fields, { [facts.field]: 'power' }) || Object.keys(observation.http.headers ?? {}).length || Object.keys(observation.http.body ?? {}).length || next.connections?.find(c => c.id === observation.connection_id)?.http?.base_url !== facts.endpoint) throw Error('AI предложил чтение вне явных фактов.');
    }
    for (const before of base.observations ?? []) if (!next.observations?.some(o => o.target_id === before.target_id)) throw Error('Удаление Observe не запрошено.');
    return next;
}
export function verifySemantic(before: ConfigurationDto, after: ConfigurationDto) {
    for (const key of ['connections', 'bindings', 'observations']) if (!equal(before[key] ?? [], after[key] ?? [])) throw Error('Semantic proposal содержит физическое подключение. Предложение не принято.');
}
export function applyLanguage(entity: WireObject, suggestion: WireObject, kind: string): WireObject {
    const next = clone(entity);
    const strings = (key: string) => { const values = suggestion[key]; if (!Array.isArray(values) || values.some(v => typeof v !== 'string')) throw Error('Некорректное AI-предложение языка'); return values; };
    if (kind === 'action') next.speech_aliases = strings('aliases');
    else if (kind === 'device') next.spoken_aliases = strings('aliases');
    else { next.speech = { mentions: strings('mentions'), locations: strings('locations') }; next.metadata = { ...(next.metadata as WireObject), aliases: strings('aliases') }; }
    if (typeof suggestion.description === 'string' && kind !== 'action') next.metadata = { ...(next.metadata as WireObject), description: suggestion.description };
    return next;
}

export function setupChanges(before: ConfigurationDto, after: ConfigurationDto) {
    return readableDiff(before, after).map(change => {
        const id = (change.after as { id?: string } | undefined)?.id;
        const zone = after.rooms.find(z => z.id === id);
        if (zone) return { ...change, afterText: `${zone.name}. Родитель: ${after.rooms.find(z => z.id === zone.metadata?.parent_id)?.name ?? 'нет'}. ${zone.metadata?.description ?? ''} Обращения: ${zone.speech?.mentions.join(', ')}. Местоположение: ${zone.speech?.locations.join(', ')}.` };
        const device = after.devices.find(d => d.id === id);
        if (device) return { ...change, afterText: `${device.name} · ${after.rooms.find(z => z.id === device.room_id)?.name ?? 'Без зоны'}. Возможности: ${device.action_ids.map(id => after.actions.find(a => a.id === id)?.name).join(', ') || 'ещё не заданы'}. Подключение: ${after.bindings.some(b => b.target_id === device.id && b.executor === 'http') ? 'настроено' : 'пока не настроено'}.` };
        return change;
    });
}
