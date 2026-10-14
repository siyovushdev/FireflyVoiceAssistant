'use strict';
// Transport for the existing controlled scenario; no alternate AI/UI driver.
const fs = require('node:fs');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const config = process.env.FIREFLY_M19_REMOTE_PLAN
    ? JSON.parse(fs.readFileSync(process.env.FIREFLY_M19_REMOTE_PLAN, 'utf8')) : null;

function call(action, detail = {}) {
    if (!config) throw Error('Remote plan is required');
    return new Promise((resolve, reject) => {
        const argv = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5'];
        if (config.controlPath) argv.push('-S', config.controlPath);
        argv.push(config.host, 'sudo -n /usr/bin/python3 -B ' + config.bridge);
        const child = spawn('/usr/bin/ssh', argv, { stdio:['pipe','pipe','pipe'] });
        let out = '', err = '';
        const timer = setTimeout(() => { child.kill('SIGTERM'); reject(Error('SSH operation timeout: ' + action)); }, 25000);
        child.stdout.on('data', b => out += b); child.stderr.on('data', b => err += b);
        child.on('error', e => { clearTimeout(timer); reject(e); });
        child.on('exit', code => {
            clearTimeout(timer);
            if (code !== 0) return reject(Error('Firefly ' + action + ': ' + err));
            try { resolve(JSON.parse(out)); } catch (e) { reject(e); }
        });
        child.stdin.end(JSON.stringify({ action, root:config.remoteRoot, ...detail }));
    });
}
function verifyVoiceEvidence(status, executions, beforeCount, expectedText, hash, ownerLive, targetId, actionId) {
    const voice = status.last_voice;
    assert.ok(voice?.audio_detected, 'real voice PCM must be detected');
    assert.equal(voice.command_text, expectedText, 'keep original STT command text');
    assert.ok(voice.transcript && voice.stt_ms > 0, 'real STT transcript/timing required');
    if (ownerLive) {
        assert.equal(voice.transcript, expectedText, 'keep raw STT text');
        assert.equal(voice.inline, false, 'owner uses separated wake and command');
        assert.equal(voice.wake_prefix_removed, false, 'no transcript repair');
    }
    assert.equal(voice.command.hash, hash, 'voice must use the applied private configuration');
    assert.equal(voice.command.input, expectedText);
    assert.equal(voice.command.safety, 'PASS');
    assert.equal(voice.command.domain, 'PASS');
    assert.equal(voice.command.execution?.success, true);
    assert.equal(executions.length, beforeCount + 1, 'one execution per utterance');
    assert.equal(executions.at(-1).source, 'voice', 'text/API execution cannot pass voice acceptance');
    assert.equal(executions.at(-1).execution?.success, true);
    assert.ok(Number.isInteger(voice.command.generation_id) && voice.command.generation_id > 0);
    assert.equal(voice.command.generation_id, status.active.generation_id);
    assert.equal(executions.at(-1).generation_id, voice.command.generation_id);
    for (const [key, value] of [['targetId',targetId],['actionId',actionId]]) {
        assert.ok(value, 'expected private target/action required');
        assert.equal(voice.command.execution[key], value);
        assert.equal(executions.at(-1).execution[key], value);
    }
    return voice;
}
function consumeOwnerReady(file, requestId, on) {
    if (!fs.existsSync(file)) return false;
    const answer = JSON.parse(fs.readFileSync(file,'utf8'));
    assert.deepEqual(answer,{requestId,on},'readiness must acknowledge the current ON/OFF request');
    fs.renameSync(file,file + '.consumed');
    return true;
}
function verifyOwnerBaseline(rows, executions, rowCount, executionCount) {
    assert.ok(rows.length>=rowCount,'mock log must not shrink');
    const reads=rows.slice(rowCount);
    for (const row of reads) {
        assert.equal(row.method,'GET','no HTTP mutation while waiting for owner readiness');
        assert.equal(row.path,'/relay/1','only the configured state read is allowed');
        assert.ok(row.body === '' || row.body === undefined,'state read must not have a body');
    }
    assert.equal(executions.length,executionCount,'no execution while waiting for owner readiness');
    return reads;
}
function ownerReadyTimeoutSeconds(ready) {
    assert.ok(Number.isFinite(ready.remainingSeconds));
    assert.ok(ready.readyReserveSeconds >= 90+5+15+15+15,'voice reserve must cover all bounded steps');
    const seconds = Math.floor(Math.min(120,ready.remainingSeconds-ready.readyReserveSeconds-10));
    assert.ok(seconds>=5,'insufficient window for an owner readiness handshake');
    return seconds;
}
module.exports = { config, call, verifyVoiceEvidence, consumeOwnerReady, verifyOwnerBaseline, ownerReadyTimeoutSeconds };
