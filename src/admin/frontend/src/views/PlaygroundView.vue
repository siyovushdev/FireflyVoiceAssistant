<script setup lang="ts">
import { foreground } from '../stores/foreground.js';
import { manual } from '../stores/manual.js';
import { ref } from 'vue';
import { adminRequest, issuesOf, type AdminReply, type Issue } from '../api/admin.js';
import { system } from '../stores/system.js';
import TaskErrors from '../components/forms/TaskErrors.vue';
import AdminVoice from '../components/AdminVoice.vue';
import CommandTrace from '../components/CommandTrace.vue';
const text = ref(''), busy = ref(false), voiceBusy = ref(false), issues = ref<Issue[]>([]), result = ref<AdminReply | null>(null), mode = ref('');
async function run(execute: boolean, production: boolean) {
    if (busy.value || voiceBusy.value || !text.value.trim()) return;
    if (production && !confirm('Production Execute отправит реальную команду оборудованию через SafetyGate и bindings. Выполнить этот текст?')) return;
    busy.value = true; issues.value = []; mode.value = !execute ? 'Preview · без исполнения' : production ? 'Production Execute · реальное оборудование' : 'Mock Execute · виртуальное состояние';
    const progress = foreground.begin(execute ? 'command' : 'preview');
    try { result.value = await adminRequest('/api/test-command', { text: text.value, execute, production }); }
    catch (error) { result.value = null; issues.value = issuesOf(error); }
    finally { progress.stage('refresh'); try { await system.refresh(); } finally { busy.value = false; progress.finish(); } }
}
</script>
<template><section class="panel" :aria-busy="busy"><h2>Command Playground</h2><p>Отдельный ручной ввод намерения. Production исполняет текущую активную конфигурацию, а не локальный план.</p><label>Команда<textarea v-model="text" placeholder="Введите явную команду" :disabled="busy"></textarea></label><AdminVoice @transcript="text = $event" @busy="voiceBusy = $event; manual.voiceBusy.value = $event" /><fieldset :disabled="busy || voiceBusy || manual.setup.value?.active || !text.trim()"><div class="inline-actions"><button @click="run(false, false)">Preview</button><button @click="run(true, false)">Mock Execute</button><button class="danger" @click="run(true, true)">Production Execute · оборудование</button></div></fieldset><TaskErrors :issues="issues" /><div v-if="result"><h3>{{ mode }}</h3><CommandTrace :record="result" :configuration="system.configuration.value" /><p>Команда и transport result не являются независимым подтверждением состояния.</p><pre>{{ JSON.stringify(result, null, 2) }}</pre></div></section></template>
