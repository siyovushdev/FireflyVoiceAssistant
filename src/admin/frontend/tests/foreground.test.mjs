import test from 'node:test';
import assert from 'node:assert/strict';
import { createForeground, foreground } from '../.test-build/stores/foreground.js';
import { createManualStore } from '../.test-build/stores/manual.js';
import { createSystemStore } from '../.test-build/stores/system.js';
import * as setup from '../.test-build/stores/setup.js';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const deferred = () => { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return { promise, resolve, reject }; };

test('AI workflow locks immediately, suppresses duplicate, shows progress, clears on response/error', async () => {
    for (const error of [false, true]) {
        const gate = deferred(); let calls = 0;
        const operation = () => { calls++; return gate.promise; };
        const request = setup.setupRun(operation);
        assert.equal(setup.setupBusy.value, true);
        assert.equal(foreground.pending.value, true);
        assert.equal(foreground.visible.value.length, 0);
        await setup.setupRun(operation); assert.equal(calls, 1);
        await delay(280); assert.equal(foreground.visible.value[0].label, 'Готовим предложение…');
        error ? gate.reject(Error('AI unavailable')) : gate.resolve();
        await request;
        assert.equal(setup.setupBusy.value, false); assert.equal(foreground.pending.value, false);
        assert.equal(foreground.visible.value.length, 0);
        assert.equal(setup.setupIssues.value.length, error ? 1 : 0);
        if (error) assert.equal(setup.setupIssues.value[0].message, 'AI unavailable');
        await setup.setupRun(async () => { calls++; }); assert.equal(calls, 2);
    }
});

test('fast completion cancels delayed display; exception also clears pending', async () => {
    const state = createForeground(15);
    await state.run('probe', async () => {});
    await assert.rejects(state.run('command', async () => { throw Error('network'); }));
    await delay(30); assert.equal(state.pending.value, false); assert.deepEqual(state.visible.value, []);
});

test('concurrent completion cannot hide another operation; sequential stages retain identity and visibility', async () => {
    const state = createForeground(5), one = deferred(), two = deferred(); let stage;
    const first = state.run('save', async update => { stage = update; await one.promise; });
    const second = state.run('probe', () => two.promise);
    await delay(15); const id = state.visible.value[0].id; assert.equal(state.visible.value.length, 2);
    stage('apply'); assert.equal(state.visible.value[0].id, id); assert.equal(state.visible.value[0].label, 'Применяем конфигурацию…');
    one.resolve(); await first; assert.equal(state.pending.value, true); assert.equal(state.visible.value[0].kind, 'probe');
    two.resolve(); await second; assert.equal(state.pending.value, false);
    stage('verify'); assert.equal(state.pending.value, false, 'late stage cannot resurrect finished work');
});

test('background snapshot refresh never enters foreground registry, even when slow or failing', async () => {
    const gate = deferred(); const store = createSystemStore(async () => { await gate.promise; throw Error('offline'); });
    const request = store.refresh(); assert.equal(store.loading.value, true);
    await delay(280); assert.equal(foreground.pending.value, false);
    gate.resolve(); await request; assert.equal(store.loading.value, false); assert.equal(Object.keys(store.errors.value).length, 5);
});

test('manual review/CAS Save/Apply/verify retain one operation and reject double submission on success and failure', async () => {
    for (const failure of [false, true]) {
        const savedGate = deferred(), applyGate = deferred(), verifyGate = deferred(); const calls = [];
        let stored = { ok:true, revision:1, hash:'old', configuration:{ system:{name:'test'}, rooms:[], devices:[], actions:[], bindings:[] } };
        let active = stored, applied = false;
        const store = createManualStore(async (path, body) => {
            calls.push(path);
            if (path === '/api/setup/status') return { ok:true, active:false };
            if (path === '/api/config/validate') return { ok:true, apply_ready:true };
            if (path === '/api/config/diff') return { ok:true };
            if (path === '/api/config') { await savedGate.promise; return stored = {...stored, hash:'new', revision:2}; }
            if (path === '/api/config/apply') { await applyGate.promise; if (failure) throw Error('apply failed'); active=stored; applied=true; return {ok:true}; }
            throw Error(path);
        }, async path => { if (path === '/api/config') return stored; if (applied) await verifyGate.promise; return {ok:true, active}; }, async () => {});
        await store.prepare(); const request = store.confirm();
        assert.equal(store.busy.value, true); assert.equal(store.canConfirm.value, false);
        await store.confirm(); await delay(280);
        assert.equal(foreground.visible.value[0].label, 'Сохраняем конфигурацию…');
        savedGate.resolve(); await delay(0); assert.equal(foreground.visible.value[0].label, 'Применяем конфигурацию…');
        applyGate.resolve(); await delay(0);
        if (!failure) { assert.equal(foreground.visible.value[0].label, 'Проверяем активную версию…'); verifyGate.resolve(); }
        await request;
        assert.equal(store.busy.value, false); assert.equal(foreground.pending.value, false);
        assert.equal(calls.filter(p => p === '/api/config').length, 1); assert.equal(calls.filter(p => p === '/api/config/apply').length, 1);
        assert.equal(store.issues.value.length, failure ? 1 : 0);
    }
});
