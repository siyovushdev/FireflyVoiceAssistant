<script setup lang="ts">
import { computed } from 'vue';
import { foreground } from '../stores/foreground.js';
const props = defineProps<{ label?: string }>();
const visible = computed(() => props.label ? [{ id: 0, label: props.label }] : foreground.visible.value);
</script>
<template><div class="foreground-status" role="status" aria-live="polite" aria-atomic="true"><div v-if="visible.length" class="foreground-content" data-testid="foreground-progress"><span class="busy-spinner" aria-hidden="true"></span><div><p v-for="entry in visible" :key="entry.id">{{ entry.label }}</p></div></div></div></template>
