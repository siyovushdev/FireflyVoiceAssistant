import test from 'node:test';
import assert from 'node:assert/strict';
import { adminRequest } from '../.test-build/api/admin.js';
import { queued } from '../.test-build/api/transport.js';
import { setupAuth } from '../.test-build/api/setup.js';

test('speech stop bypasses blocked foreground queue and sends only one guarded audio request', async () => {
    const oldFetch = globalThis.fetch, oldToken = setupAuth.token, calls = [];
    let release, entered;
    const started = new Promise(resolve => { entered = resolve; });
    const blocker = queued(async () => { entered(); await new Promise(resolve => { release = resolve; }); });
    await started;
    try {
        setupAuth.token = 'owned-setup-fixture';
        globalThis.fetch = async (path, options) => { calls.push({ path, options }); return new Response('{"ok":true}', { status: 200 }); };
        await Promise.race([adminRequest('/api/voice/stop', {}), new Promise((_, reject) => setTimeout(() => reject(Error('stop queued behind inference')), 1000))]);
        assert.equal(calls.length, 1); assert.equal(calls[0].path, '/api/voice/stop');
        assert.equal(calls[0].options.headers['X-Firefly-Admin'], '1');
        assert.equal(calls[0].options.body, '{}');
    } finally { release(); await blocker; globalThis.fetch = oldFetch; setupAuth.token = oldToken; }
});
