import test from 'node:test';
import assert from 'node:assert/strict';
import { commandTrace } from '../.test-build/adapters/commandTrace.js';

const config = { devices: [{ id: 'light', name: 'Свет на кухне' }], actions: [{ id: 'off', name: 'Выключить' }] };
test('command diagnostics distinguish actual Qwen intent inference from fast path and pre-inference rejection', () => {
    const command = { input: 'вырубай свет на кухне', route: 'rkllm', llm: { invoked: true, task: 'power_intent' },
        result: { targetId: 'light', actionId: 'off' }, execution: { success: true } };
    const trace = commandTrace({ last_voice: { transcript: command.input, command } }, config);
    assert.match(trace.route, /Qwen.*намерения/);
    assert.equal(trace.target, 'Свет на кухне'); assert.equal(trace.action, 'Выключить'); assert.equal(trace.executed, true);
    assert.match(commandTrace({ ...command, route: 'deterministic', llm: { invoked: false } }).route, /Правила/);
    const rejected = commandTrace({ input: 'не выключай свет', route: 'grounding_rejected', llm: { invoked: false }, rejection_stage: 'grounding', result: { unsupported: true } });
    assert.match(rejected.route, /до вызова Qwen/); assert.ok(rejected.reason); assert.equal(rejected.executed, false);
});
test('old diagnostic responses never invent a Qwen invocation or physical confirmation', () => {
    const trace = commandTrace({ input: 'свет', route: 'rkllm', result: { unsupported: true } });
    assert.equal(trace.route, 'Вызов Qwen не установлен'); assert.equal(trace.executed, false);
    assert.equal(commandTrace(null), null); assert.equal(commandTrace({}), null);
    const preview = commandTrace({ input: 'выключи свет', route: 'deterministic', llm: { invoked: false }, predicted_execution: { executed: false } });
    assert.equal(preview.executed, false);
});
