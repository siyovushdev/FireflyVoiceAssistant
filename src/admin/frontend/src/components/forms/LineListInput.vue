<script setup lang="ts">
import { lines, rawLines } from '../../adapters/editing.js';
defineProps<{ modelValue?: readonly string[] }>();
const emit = defineEmits<{ 'update:modelValue': [value: string[]] }>();
</script>

<template>
    <textarea :value="modelValue?.join('\n') ?? ''"
        @input="emit('update:modelValue', rawLines(($event.target as HTMLTextAreaElement).value))"
        @blur="emit('update:modelValue', lines(($event.target as HTMLTextAreaElement).value))"></textarea>
</template>
