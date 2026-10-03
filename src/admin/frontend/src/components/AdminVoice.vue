<script setup lang="ts">
import { onUnmounted, ref } from 'vue';
import { queued } from '../api/transport.js';
const emit = defineEmits<{ transcript: [text: string]; busy: [value: boolean] }>();
const recording = ref(false), processing = ref(false), error = ref('');
const supported = window.isSecureContext && !!navigator.mediaDevices?.getUserMedia;
let context: AudioContext | undefined, stream: MediaStream | undefined, processor: ScriptProcessorNode | undefined, source: MediaStreamAudioSourceNode | undefined;
let chunks: Float32Array[] = [], samples = 0, attempt = 0, timer: ReturnType<typeof setTimeout> | undefined;
async function release() { recording.value = false; clearTimeout(timer); processor?.disconnect(); source?.disconnect(); stream?.getTracks().forEach(t => t.stop()); await context?.close(); processor = undefined; source = undefined; stream = undefined; context = undefined; }
async function transcribe(wav: Blob) {
    const current = ++attempt; processing.value = true; emit('busy', true); error.value = '';
    try {
        if (wav.size > 15 * 48000 * 2 + 256) throw Error('WAV превышает лимит: 15 секунд mono PCM16, до 48 kHz.');
        const reply = await queued(async () => { const response = await fetch('/api/admin/transcribe', { method: 'POST', headers: { 'Content-Type': 'audio/wav', 'X-Firefly-Admin': '1' }, body: wav, credentials: 'same-origin', redirect: 'error', signal: AbortSignal.timeout(120000) }); const data = await response.json(); if (!response.ok || data.ok === false) throw Error(data.errors?.map((e: { message: string }) => e.message).join('; ') ?? 'Локальное STT недоступно'); return data; });
        if (current === attempt) { emit('transcript', String(reply.text ?? '')); error.value = 'Текст распознан. Исправьте его перед отдельным запуском Preview или команды.'; }
    } catch (failure) { if (current === attempt) error.value = failure instanceof Error ? failure.message : 'STT недоступно'; }
    finally { if (current === attempt) { processing.value = false; emit('busy', false); } }
}
async function start() {
    if (processing.value || recording.value) return;
    const current = ++attempt; processing.value = true; emit('busy', true); error.value = '';
    try {
        const obtained = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true } });
        if (current !== attempt) { obtained.getTracks().forEach(t => t.stop()); return; }
        stream = obtained; context = new AudioContext({ sampleRate: 48000 });
        source = context.createMediaStreamSource(stream); processor = context.createScriptProcessor(4096, 1, 1); chunks = []; samples = 0; recording.value = true;
        processor.onaudioprocess = event => { if (!recording.value) return; const data = event.inputBuffer.getChannelData(0).slice(0, Math.max(0, context!.sampleRate * 15 - samples)); chunks.push(data); samples += data.length; };
        source.connect(processor); processor.connect(context.destination); timer = setTimeout(stop, 15000);
    } catch (failure) { await release(); error.value = failure instanceof Error ? failure.message : 'Микрофон недоступен'; emit('busy', false); }
    finally { processing.value = false; }
}
async function stop() {
    if (!context || !recording.value) return;
    const rate = context.sampleRate, total = samples, parts = chunks; await release();
    const buffer = new ArrayBuffer(44 + total * 2), view = new DataView(buffer);
    const word = (offset: number, text: string) => [...text].forEach((c, i) => view.setUint8(offset + i, c.charCodeAt(0)));
    word(0, 'RIFF'); view.setUint32(4, 36 + total * 2, true); word(8, 'WAVE'); word(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true); view.setUint32(24, rate, true); view.setUint32(28, rate * 2, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true); word(36, 'data'); view.setUint32(40, total * 2, true);
    let offset = 44; for (const part of parts) for (const value of part) { const sample = Math.max(-1, Math.min(1, value)); view.setInt16(offset, Math.round(sample * (sample < 0 ? 32768 : 32767)), true); offset += 2; }
    await transcribe(new Blob([buffer], { type: 'audio/wav' }));
}
onUnmounted(() => { attempt++; void release(); emit('busy', false); });
</script>
<template><details><summary>Локальный голосовой ввод</summary><p>Запись до 15 секунд → локальный WAV/STT → редактируемый текст. Распознавание ничего не выполняет и не применяет.</p><p v-if="!supported" class="notice warning">Микрофон требует secure context: используйте localhost или SSH tunnel. LAN HTTPS/mobile deployment не входит в V2.</p><button :disabled="!supported || processing" @click="recording ? stop() : start()">{{ recording ? 'Остановить и распознать' : 'Записать голос' }}</button><label>Или локальный WAV mono PCM16<input type="file" accept="audio/wav,.wav" :disabled="processing || recording" @change="($event.target as HTMLInputElement).files?.[0] && transcribe(($event.target as HTMLInputElement).files![0])"></label><p v-if="processing">Обработка…</p><p v-if="error" role="status">{{ error }}</p></details></template>
