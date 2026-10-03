import type { ActionDto, CatalogDto, ConfigurationDto, DeviceDto, DeviceStateDto, PropertyDto, StatesDto } from '../types/wire.js';

export interface ZoneView { readonly id: string; readonly name: string; readonly parentId: string | null; readonly description: string; readonly path: string; readonly deviceCount: number }
export interface PropertyView { readonly id: string; readonly name: string; readonly value: string; readonly label: string; readonly tone: string; readonly stale: boolean; readonly timestamp: number | null }
export interface DeviceView {
    readonly id: string; readonly name: string; readonly locationId: string | null; readonly location: string;
    readonly enabled: boolean; readonly description: string; readonly actions: readonly ActionDto[];
    readonly properties: readonly PropertyView[]; readonly connection: string; readonly reachability: string;
    readonly connectionNames: readonly string[]; readonly observation: boolean; readonly lastSuccess: number | null;
}
export function sameGeneration(a: CatalogDto | null | undefined, b: CatalogDto | null | undefined): boolean {
    return !!a && !!b && a.hash === b.hash && a.revision === b.revision && a.generation_id === b.generation_id;
}
export function zones(config: ConfigurationDto): ZoneView[] {
    const path = (id: string, visited = new Set<string>()): string => {
        if (visited.has(id)) return 'Иерархия недоступна';
        visited.add(id);
        const zone = config.rooms.find(item => item.id === id);
        if (!zone) return 'Зона недоступна';
        return zone.metadata?.parent_id ? `${path(zone.metadata.parent_id, visited)} / ${zone.name}` : zone.name;
    };
    return config.rooms.map(zone => ({ id: zone.id, name: zone.name, parentId: zone.metadata?.parent_id ?? null,
        description: zone.metadata?.description ?? '', path: path(zone.id), deviceCount: config.devices.filter(d => d.room_id === zone.id).length }));
}
function propertyView(id: string, name: string, unit: string, property?: PropertyDto): PropertyView {
    const known = property && ['predicted', 'confirmed'].includes(property.knowledge) && property.value !== null;
    const value = !known ? '—' : typeof property.value === 'boolean' ? (id === 'power'
        ? (property.value ? 'Да / включено' : 'Нет / выключено') : (property.value ? 'Да' : 'Нет'))
        : String(property.value) + (unit ? ` ${unit === 'percent' ? '%' : unit}` : '');
    return { id, name, value,
        label: !known ? 'Состояние неизвестно' : property.knowledge === 'predicted' ? 'Ожидается · не подтверждено' : 'Подтверждено устройством',
        tone: !known ? 'unknown' : property.stale ? 'stale' : property.knowledge,
        stale: property?.stale ?? false,
        timestamp: !known ? null : (property.knowledge === 'confirmed' ? property.observed_at_ms : property.updated_at_ms) || null };
}
export function properties(device: DeviceDto, state?: DeviceStateDto): PropertyView[] {
    const fields = device.kind === 'light' ? [{ id: 'brightness', name: 'Яркость', unit: '%' }]
        : device.kind === 'entrance_door' ? [{ id: 'position', name: 'Положение', unit: '' }] : device.state ?? [];
    // Config initial and legacy/playground state are intentionally never read here.
    return fields.map(field => propertyView(field.id, field.name, field.unit ?? '', state?.properties[field.id]));
}
export function devices(config: ConfigurationDto, active: CatalogDto | null, state: StatesDto | null): DeviceView[] {
    const locations = zones(config);
    const coherent = sameGeneration(active, state?.active) && config === active?.configuration;
    return config.devices.map(device => {
        const evidence = coherent ? state?.devices.find(item => item.targetId === device.id) : undefined;
        const actions = config.actions.filter(action => device.action_ids.includes(action.id));
        const usable = actions.filter(action => action.enabled);
        const bindings = config.bindings.filter(binding => binding.target_id === device.id && usable.some(action => action.id === binding.action_id));
        const configured = bindings.length;
        const connectionIds = new Set(bindings.map(binding => binding.connection_id).filter(Boolean));
        const connectionNames = (config.connections ?? []).filter(connection => connectionIds.has(connection.id))
            .map(connection => connection.name + (connection.enabled ? '' : ' · отключено'));
        if (bindings.some(binding => !binding.connection_id)) connectionNames.push('HTTP · отдельный адрес');
        return { id: device.id, name: device.name, locationId: device.room_id,
            location: device.room_id === null ? 'Без зоны' : locations.find(zone => zone.id === device.room_id)?.path ?? 'Зона недоступна',
            enabled: device.enabled, description: device.metadata?.description ?? '', actions,
            properties: properties(device, evidence),
            connection: configured === 0 ? 'Подключение не настроено' : `Настроено действий: ${configured} из ${usable.length}`,
            connectionNames, observation: (config.observations ?? []).some(item => item.target_id === device.id),
            reachability: evidence?.connectivity === 'reachable' ? 'Последний обмен успешен' : evidence?.connectivity === 'unreachable' ? 'Последняя попытка связи неуспешна' : 'Связь не проверена',
            lastSuccess: evidence?.last_success_at_ms ?? null };
    });
}
export function formatTime(stamp: number | null): string {
    return stamp ? new Date(stamp).toLocaleString('ru-RU') : '—';
}
