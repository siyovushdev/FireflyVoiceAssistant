import type { ConnectionDto } from '../types/wire.js';

export function mqttProbeFingerprint(connection: ConnectionDto, host: string,
                                     port: number, tls: boolean, credentialsId: string): string {
    return JSON.stringify({ connection, host: host.trim(), port, tls,
                            credentials_id: credentialsId.trim() });
}

export function mqttProbeConfirmed(reply: Record<string, unknown>,
                                   tls: boolean, credentialsId: string): boolean {
    return reply.ok === true && reply.transport === 'mqtt' &&
        reply.scope === 'mqtt_connack_only' && reply.connack_accepted === true &&
        reply.tls_verified === tls &&
        reply.credentials_accepted === Boolean(credentialsId.trim()) &&
        reply.saved === false && reply.applied === false &&
        reply.published === false && reply.subscribed === false;
}
