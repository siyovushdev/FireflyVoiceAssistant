import type { ActionDto, ConfigurationDto, DeviceDto, Scalar } from '../types/wire.js';
export interface CommandIntent { text: string; expected_action: { target_id: string; action_id: string; arguments: Record<string, Scalar> } }
export function commandIntent(config: ConfigurationDto, device: DeviceDto, action: ActionDto, args: Record<string, Scalar>): CommandIntent | null {
    // Protected entry door always needs explicitly authored manual text.
    if (device.kind === 'entrance_door' || action.operation === 'entrance_door.open.v1') return null;
    const alias = action.speech_aliases?.[0], mention = device.spoken_aliases?.[0];
    if (!alias || !mention || action.parameters.length > 1) return null;
    const zone = config.rooms.find(z => z.id === device.room_id);
    const location = zone?.speech?.locations[0];
    if (device.room_id && !location) return null;
    if (action.parameters.some(p => !Object.hasOwn(args, p.id))) return null;
    const values = action.parameters.map(p => String(args[p.id]));
    const text = [alias, mention, location, ...values].filter(Boolean).join(' ');
    if (new TextEncoder().encode(text).length > 192) return null;
    return { text, expected_action: { target_id: device.id, action_id: action.id, arguments: { ...args } } };
}
