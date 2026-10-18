import type { ConnectionDto } from '../types/wire.js';

export function mqttProbeFingerprint(connection: ConnectionDto, host: string,
                                     port: number, tls: boolean, credentialsId: string): string {
    return JSON.stringify({ connection, host: host.trim(), port, tls,
                            credentials_id: credentialsId.trim() });
}

export function mqttProbeConfirmed(reply: unknown,
                                   tls: boolean, credentialsId: string): boolean {
    if (!reply || typeof reply !== 'object' || Array.isArray(reply)) return false;
    const result = reply as Record<string, unknown>;
    return result.ok === true && result.transport === 'mqtt' &&
        result.scope === 'mqtt_connack_only' && result.connack_accepted === true &&
        result.tls_verified === tls &&
        result.credentials_accepted === Boolean(credentialsId.trim()) &&
        result.saved === false && result.applied === false &&
        result.published === false && result.subscribed === false;
}
