import { clone, readableDiff, newHttpConnection, defaultHttpMapping, projectConnectivity } from './editing.js';
import type { ConfigurationDto, WireObject, Scalar } from '../types/wire.js';
export interface HttpFact { method: string; path: string; body: Record<string, unknown> }
export interface ConnectionFacts { endpoint: string; on: HttpFact | null; off: HttpFact | null; observe: string; field: string; noObserve: boolean }
export const emptyFacts = (): ConnectionFacts => ({ endpoint: '', on: null, off: null, observe: '', field: '', noObserve: false });
// Conservative extraction of explicit user facts. No URL/body defaults or coercion.
export function collectFacts(previous: ConnectionFacts, text: string, asking = ''): ConnectionFacts {
    const next = clone(previous);
    const endpoints = [...text.matchAll(/https?:\/\/[^\s,;]+/g)].map(m => m[0].replace(/\.$/, ''));
    if (endpoints.length > 1 || endpoints.some(value => !/^http:\/\/(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?$/.test(value))) throw Error('Нужен один точный HTTP endpoint без пути. Другой контракт настройте вручную.');
    if (endpoints.length) next.endpoint = endpoints[0];
    if (/\{[^{}]*\{/.test(text)) throw Error('Вложенный JSON требует ручного B2 wizard.');
    const supplied = new Set<string>();
    for (const match of text.matchAll(/(POST|PUT|PATCH|GET)\s+(\/[^\s,;{}]+)\s*(\{[^{}]*\})?/g)) {
        const path = match[2].replace(/[.;]$/, '');
        const prefix = text.slice(Math.max(0, match.index! - 35), match.index).toLowerCase();
        if (!match[3] && match[1] !== 'GET') throw Error('Укажите точный JSON body, включая {} для пустого тела.');
        const body = match[3] ? JSON.parse(match[3]) : {};
        if (Object.values(body).some(v => v !== null && typeof v === 'object')) throw Error('Нужен JSON с простыми полями верхнего уровня.');
        const explicit = /выключ|\bOFF\b/i.test(prefix) ? 'off' : /включ|\bON\b/i.test(prefix) ? 'on' : '';
        const key = match[1] === 'GET' && (/состоя|чтени|observe/.test(prefix) || !explicit && asking === 'observe') ? 'observe' : explicit || asking;
        if (!['on', 'off', 'observe'].includes(key)) throw Error('Уточните, для чего указан HTTP contract: включение, выключение или чтение.');
        if (supplied.has(key)) throw Error('Несколько contracts для одного действия неоднозначны. Укажите один или настройте вручную.');
        supplied.add(key);
        if (key === 'observe') {
            if (Object.keys(body).length) throw Error('Чтение GET не может иметь тело; настройте контракт вручную.');
            next.observe = path; next.noObserve = false;
        } else if (key === 'on') next.on = { method: match[1], path, body };
        else next.off = { method: match[1], path, body };
    }
    const field = text.match(/(?:поле|field)\s+([^\s,;]+)/i);
    if (field) field[1] = field[1].replace(/\.$/, '');
    if (field && !/^[a-zA-Z_][\w]*$/.test(field[1])) throw Error('Нужно точное top-level поле; nested mapping доступен только вручную.');
    if (field) next.field = field[1];
    if (/без чтения|чтение не предоставляется|нет observe/i.test(text)) {
        if (supplied.has('observe')) throw Error('Чтение и «без чтения» противоречат друг другу. Уточните выбор.'); next.noObserve = true; next.observe = ''; next.field = ''; }
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
    return device?.kind === 'generic' && device.state?.length === 1 && device.state[0].id === 'power' && device.state[0].type === 'boolean'
        && device.action_ids.length === 2 && new Set(device.action_ids).size === 2
        && [true, false].every(value => config.actions.filter(a => device.action_ids.includes(a.id) && a.parameters.length === 0
            && a.assignments?.length === 1 && a.assignments[0].field === 'power' && a.assignments[0].value === value && !a.assignments[0].argument).length === 1);
}
export function targetConnectionIds(config: ConfigurationDto, target: string): string[] {
    return [...new Set([...config.bindings.filter(b => b.target_id === target).map(b => b.connection_id),
        ...(config.observations ?? []).filter(o => o.target_id === target).map(o => o.connection_id)].filter((id): id is string => !!id))];
}
// Only construction is local. ConfigLoader/ConfigValidator still validate the full
// candidate before presentation, acceptance and final setup Apply.
export function buildConnectivity(config: ConfigurationDto, target: string, facts: ConnectionFacts, shared: string): ConfigurationDto {
    if (missingFact(facts)) throw Error('Сначала уточните недостающие сведения.');
    if (!supportsPowerContract(config, target)) throw Error('Этот контракт требует ручного B2 wizard: нужны ровно ON/OFF и boolean Питание.');
    if (!['all', 'separate'].includes(shared)) throw Error('Выберите явно: существующее подключение или отдельное.');
    if (!/^http:\/\/(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?$/.test(facts.endpoint)) throw Error('Укажите полный HTTP endpoint без пути; другие контракты настройте вручную.');
    if (facts.noObserve && (facts.observe || facts.field)) throw Error('Выберите чтение или явно «без чтения», без противоречий.');
    for (const fact of [facts.on!, facts.off!]) {
        if (!['GET', 'POST', 'PUT', 'PATCH'].includes(fact.method) || !fact.path.startsWith('/') || !fact.body || Array.isArray(fact.body)
            || Object.values(fact.body).some(v => !['string', 'number', 'boolean'].includes(typeof v))
            || fact.method === 'GET' && Object.keys(fact.body).length) throw Error('Неподдерживаемый HTTP contract; используйте ручной B2 wizard.');
    }
    if (!facts.noObserve && (!facts.observe.startsWith('/') || !/^[a-zA-Z_][\w]*$/.test(facts.field))) throw Error('Нужны GET path и точное top-level поле ответа.');
    const device = config.devices.find(d => d.id === target)!;
    let connection = newHttpConnection(config);
    if (shared === 'all') {
        const ids = targetConnectionIds(config, target);
        const existing = ids.length === 1 && config.connections?.find(c => c.id === ids[0]);
        if (!existing || existing.type !== 'http' || !existing.http || existing.http.base_path) throw Error('Подключение неоднозначно или имеет отдельный prefix; используйте ручной B2 wizard.');
        connection = clone(existing);
    } else connection.name = `${device.name} HTTP`;
    connection.http!.base_url = facts.endpoint;
    const mappings = [true, false].map(on => {
        const action = config.actions.find(a => device.action_ids.includes(a.id) && a.assignments?.[0]?.value === on)!;
        const fact = on ? facts.on! : facts.off!;
        return { target_id: target, action_id: action.id, connection_id: connection.id, executor: 'http',
            http: { ...defaultHttpMapping(), method: fact.method, path: fact.path, body: clone(fact.body) as Record<string, Scalar> } };
    });
    const observation = facts.noObserve ? undefined : { target_id: target, connection_id: connection.id, executor: 'http',
        http: { ...defaultHttpMapping(), method: 'GET', path: facts.observe }, fields: { [facts.field]: 'power' } };
    return projectConnectivity(config, connection, target, mappings, observation);
}
export function verifyConnectivity(base: ConfigurationDto, candidate: ConfigurationDto, target: string, facts: ConnectionFacts, shared: string) {
    if (JSON.stringify(buildConnectivity(base, target, facts, shared)) !== JSON.stringify(candidate)) throw Error('Предложение не соответствует явным фактам или принятому плану. Подготовьте его заново.');
}
const equal = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
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
        if (device) return { ...change, afterText: `${device.name} · ${after.rooms.find(z => z.id === device.room_id)?.name ?? 'Без зоны'}. Речевые формы: ${device.spoken_aliases?.join(', ') || 'ещё не заданы'}. Возможности: ${device.action_ids.map(id => after.actions.find(a => a.id === id)?.name).join(', ') || 'ещё не заданы'}. Подключение: ${after.bindings.some(b => b.target_id === device.id && ['http', 'mqtt', 'modbus_tcp', 'modbus_rtu'].includes(b.executor ?? '')) ? 'настроено' : 'пока не настроено'}.` };
        return change;
    });
}
