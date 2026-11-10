import { allocate, clone, projectConnectivity, type Mutable } from './editing.js';
import type { BindingDto, ConfigurationDto, ConnectionDto, ModbusMappingDto, ModbusTransport } from '../types/wire.js';

export function newModbusConnection(config: ConfigurationDto, transport: ModbusTransport): Mutable<ConnectionDto> {
    return { id: allocate(config, `connection_${transport}`), name: '', type: transport, enabled: true, timeout_ms: 2000,
        modbus: { unit_id: 1 }, ...(transport === 'modbus_tcp' ? { network: { host: '', port: 502 } }
            : { serial: { port: '', baud_rate: 9600, parity: 'even' as const, stop_bits: 1 } }) };
}
export interface ModbusDeviceDraft {
    mappings: Mutable<BindingDto>[];
    observing: boolean;
    observeConnectionId: string;
    replaceOtherObservation: boolean;
    observation: Mutable<ModbusMappingDto>;
}
export function initialModbusDeviceDraft(config: ConfigurationDto, target: string, transport: ModbusTransport): ModbusDeviceDraft {
    const old = config.observations?.find(o => o.target_id === target && o.executor === transport);
    return { mappings: config.bindings.filter(b => b.target_id === target && b.executor === transport).map(clone),
        observing: !!old, observeConnectionId: old?.connection_id ?? '', replaceOtherObservation: false,
        observation: clone(old?.modbus ?? { function: 1, address: 0, data_type: 'boolean', field: '' }) };
}
export class ModbusDeviceDrafts {
    private readonly saved = new Map<string, ModbusDeviceDraft>();
    constructor(private readonly config: ConfigurationDto, private readonly transport: ModbusTransport) {}
    remember(id: string, draft: ModbusDeviceDraft) { if (id) this.saved.set(id, clone(draft)); }
    restore(id: string) { return clone(this.saved.get(id) ?? initialModbusDeviceDraft(this.config, id, this.transport)); }
    changed(id: string, draft: ModbusDeviceDraft): [string, ModbusDeviceDraft][] {
        const pending = new Map(this.saved); if (id) pending.set(id, clone(draft));
        return [...pending].filter(([key, value]) => JSON.stringify(value) !== JSON.stringify(initialModbusDeviceDraft(this.config, key, this.transport))).map(([key, value]) => [key, clone(value)]);
    }
}
const integer = (value: unknown, min: number, max: number) => typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max;
export const modbusBaudRates = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400];
export function modbusConnectionError(connection: ConnectionDto): string {
    if (!['modbus_tcp', 'modbus_rtu'].includes(connection.type) || !connection.name.trim()) return 'Укажите название и протокол Modbus.';
    if (!integer(connection.modbus?.unit_id, 1, 247)) return 'Unit ID: целое число от 1 до 247; broadcast не поддерживается.';
    if (!integer(connection.timeout_ms, 50, 10000)) return 'Timeout: целое число от 50 до 10000 мс.';
    if (connection.type === 'modbus_tcp') {
        const host = connection.network?.host ?? '';
        if (!/^(?:0|[1-9]\d{0,2})(?:\.(?:0|[1-9]\d{0,2})){3}$/.test(host) || host.split('.').some(n => +n > 255) || !integer(connection.network?.port, 1, 65535)) return 'Укажите числовой IPv4 и TCP-порт устройства.';
    } else if (!connection.serial || !/^\/dev\/(?:ttyUSB|ttyACM|ttyS|ttyAMA|pts\/)\d{1,8}$/.test(connection.serial?.port ?? '') || !modbusBaudRates.includes(connection.serial?.baud_rate ?? 0) || !['none', 'even', 'odd'].includes(connection.serial.parity) || ![1, 2].includes(connection.serial.stop_bits)) return 'Укажите literal-порт /dev/ttyUSB, ttyACM, ttyS или ttyAMA с номером и поддерживаемые serial-параметры.';
    return '';
}
export function modbusMappingError(mapping: ModbusMappingDto, read = false): string {
    if (!integer(mapping.address, 0, 65535)) return 'Адрес Modbus должен быть целым числом от 0 до 65535 (нумерация с нуля).';
    if (!(read ? [1, 2, 3, 4] : [5, 6]).includes(mapping.function)) return 'Выберите поддерживаемую функцию Modbus.';
    const bit = [1, 2, 5].includes(mapping.function);
    if (bit ? mapping.data_type !== 'boolean' : !['uint16', 'int16'].includes(mapping.data_type)) return 'Тип данных не соответствует функции Modbus.';
    if (read) return mapping.field ? '' : 'Выберите свойство для результата чтения.';
    if (mapping.argument) return mapping.value === undefined ? '' : 'Укажите либо значение, либо параметр команды.';
    if (bit ? typeof mapping.value !== 'boolean' : !integer(mapping.value, mapping.data_type === 'int16' ? -32768 : 0, mapping.data_type === 'int16' ? 32767 : 65535)) return 'Значение не соответствует типу Modbus.';
    return '';
}
export function projectModbusWizard(config: ConfigurationDto, connection: ConnectionDto, transport: ModbusTransport,
    edits: readonly [string, ModbusDeviceDraft][]): Mutable<ConfigurationDto> {
    const error = modbusConnectionError(connection); if (error) throw Error(error);
    if (connection.type !== transport) throw Error('Протокол выбранного подключения изменился.');
    let next = projectConnectivity(config, connection, '', [], undefined, transport);
    for (const [target, draft] of edits) {
        const device = config.devices.find(d => d.id === target); if (!device) throw Error('Устройство не найдено.');
        for (const binding of draft.mappings) {
            if (binding.target_id !== target || binding.executor !== transport || !binding.modbus || !device.action_ids.includes(binding.action_id)) throw Error('Некорректное действие Modbus.');
            if (config.bindings.some(b => b.target_id === target && b.action_id === binding.action_id && b.executor !== transport)) throw Error('Сначала явно отвяжите действие от другого протокола.');
            if (!next.connections?.some(c => c.id === binding.connection_id && c.type === transport && c.enabled)) throw Error('Для действия требуется явное подключение Modbus.');
            const invalid = modbusMappingError(binding.modbus); if (invalid) throw Error(invalid);
            if (binding.modbus.argument) {
                const action = config.actions.find(a => a.id === binding.action_id)!;
                const param = action.parameters.find(p => p.id === binding.modbus!.argument);
                const low = binding.modbus.data_type === 'int16' ? -32768 : 0, high = binding.modbus.data_type === 'int16' ? 32767 : 65535;
                if (action.parameters.length !== 1 || !param?.required || param.type !== (binding.modbus.data_type === 'boolean' ? 'boolean' : 'integer') || (param.type === 'integer' && ((param.minimum ?? 0) < low || (param.maximum ?? 0) > high))) throw Error('Нужен один обязательный параметр команды, соответствующий типу и диапазону Modbus.');
            } else if (config.actions.find(a => a.id === binding.action_id)!.parameters.length) throw Error('Действие с параметром требует явного отображения этого параметра в Modbus.');
        }
        let observation: NonNullable<ConfigurationDto['observations']>[number] | undefined;
        if (draft.observing) {
            const invalid = modbusMappingError(draft.observation, true); if (invalid) throw Error(invalid);
            const field = device.state?.find(f => f.id === draft.observation.field);
            if (!field || field.type !== (draft.observation.data_type === 'boolean' ? 'boolean' : 'integer')) throw Error('Свойство состояния не соответствует типу Modbus.');
            if (!next.connections?.some(c => c.id === draft.observeConnectionId && c.type === transport && c.enabled)) throw Error('Для чтения требуется явное подключение Modbus.');
            if (config.observations?.some(o => o.target_id === target && o.executor !== transport) && !draft.replaceOtherObservation) throw Error('Подтвердите замену чтения состояния другого протокола.');
            observation = { ...clone(config.observations?.find(o => o.target_id === target && o.executor === transport) ?? {}), target_id: target, connection_id: draft.observeConnectionId, executor: transport, modbus: clone(draft.observation), fields: {} };
        }
        next = projectConnectivity(next, null, target, draft.mappings, observation, transport, draft.observing && draft.replaceOtherObservation);
    }
    return next;
}
