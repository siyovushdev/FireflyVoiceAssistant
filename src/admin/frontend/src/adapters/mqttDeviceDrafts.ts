import { clone, type Mutable } from './editing.js';
import type { BindingDto, ConfigurationDto, MqttObservationDto } from '../types/wire.js';

export interface MqttDeviceDraft {
    mappings: Mutable<BindingDto>[];
    observing: boolean;
    observeConnectionId: string;
    replaceOtherObservation: boolean;
    observation: Mutable<MqttObservationDto>;
    fields: [string, string][];
}

// Keep device-local form state outside the active displayed device. Merely
// selecting another device or returning to the protocol chooser must never
// discard command topics, payloads, or partially edited observation fields.
export function initialMqttDeviceDraft(config: ConfigurationDto, targetId: string): MqttDeviceDraft {
    const previous = config.observations?.find(o =>
        o.target_id === targetId && o.executor === 'mqtt');
    const powerField = config.devices.find(d => d.id === targetId)?.state
        ?.find(field => field.type === 'boolean')?.id ?? '';
    return {
        mappings: config.bindings.filter(b => b.target_id === targetId &&
            b.executor === 'mqtt').map(b => clone(b)),
        observing: !!previous,
        observeConnectionId: previous?.connection_id ?? '',
        replaceOtherObservation: false,
        observation: clone(previous?.mqtt ?? {
            topic: '', format: 'text' as const, qos: 0,
            field: powerField, true_payload: 'ON', false_payload: 'OFF'
        }),
        fields: Object.entries(previous?.fields ?? {}),
    };
}

export class MqttDeviceDrafts {
    private readonly saved = new Map<string, MqttDeviceDraft>();
    constructor(private readonly original: ConfigurationDto) {}

    remember(targetId: string, draft: MqttDeviceDraft): void {
        if (targetId) this.saved.set(targetId, clone(draft));
    }

    restore(targetId: string): MqttDeviceDraft {
        return clone(this.saved.get(targetId) ?? initialMqttDeviceDraft(this.original, targetId));
    }

    // Only edited devices are projected. Visiting a device must not rewrite
    // an untouched observation (including opaque/future transport fields).
    // The currently displayed fields take precedence over its last snapshot.
    changed(currentId: string, current: MqttDeviceDraft): [string, MqttDeviceDraft][] {
        const pending = new Map(this.saved);
        if (currentId) pending.set(currentId, clone(current));
        return [...pending.entries()]
            .filter(([id, draft]) =>
                JSON.stringify(draft) !== JSON.stringify(initialMqttDeviceDraft(this.original, id)))
            .map(([id, draft]) => [id, clone(draft)]);
    }
}

export function observationFromMqttDraft(
    targetId: string, draft: MqttDeviceDraft, chosenConnectionId: string
): NonNullable<ConfigurationDto['observations']>[number] | undefined {
    if (!targetId || !draft.observing) return undefined;
    const observe = clone(draft.observation);
    return {
        target_id: targetId,
        connection_id: draft.observeConnectionId || chosenConnectionId,
        executor: 'mqtt',
        mqtt: observe.format === 'text'
            ? { topic: observe.topic, format: 'text', qos: observe.qos,
                field: observe.field, true_payload: observe.true_payload,
                false_payload: observe.false_payload }
            : { topic: observe.topic, format: 'json', qos: observe.qos },
        fields: observe.format === 'json' ? Object.fromEntries(draft.fields) : {}
    };
}
