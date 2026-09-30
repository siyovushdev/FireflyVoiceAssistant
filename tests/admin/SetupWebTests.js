'use strict';
const assert=require('node:assert/strict');
const M=require('../../src/admin/web/admin-model.js');
assert.equal(M.adminCharacterCount('Привет😀'),7);
assert.equal(M.adminCharacterCount('😀'.repeat(4000)),4000);
assert.equal(M.adminCharacterCount('я'.repeat(4001)),4001);
assert.equal(M.adminCharacterCount('е\u0301'),2); // Code points, not grapheme count.
assert.equal(M.MaxCopilotVoiceSeconds,15); // Domestic/legacy profile remains separate.
const wav=M.encodeWavPcm16(new Float32Array(60*16000),16000);
assert.equal(wav.byteLength,44+60*16000*2);
assert.ok(M.humanError({code:'setup_required'}).includes('Начать настройку'));
console.log('PASS Admin Unicode counter and bounded 60-second WAV encoding');
