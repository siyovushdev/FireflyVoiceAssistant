<script setup lang="ts">
import type { FieldDto, Scalar } from '../../types/wire.js';
const props = defineProps<{ field: FieldDto; modelValue: Scalar | undefined; label: string; path?: string }>();
const emit = defineEmits<{ 'update:modelValue': [value: Scalar] }>();
function input(event: Event) { const value = (event.target as HTMLInputElement).value; emit('update:modelValue', props.field.type === 'boolean' ? value === 'true' : ['integer', 'number'].includes(props.field.type) && value !== '' ? Number(value) : value); }
</script>
<template><label>{{ label }}<select v-if="field.type === 'boolean'" :value="String(modelValue)" :data-path="path" @change="input"><option value="false">Нет</option><option value="true">Да</option></select><select v-else-if="field.type === 'enum'" :value="modelValue" :data-path="path" @change="input"><option v-for="value in field.values" :key="value" :value="value">{{ value }}</option></select><input v-else :type="['integer', 'number'].includes(field.type) ? 'number' : 'text'" :step="field.type === 'integer' ? 1 : 'any'" :min="field.minimum" :max="field.maximum" :minlength="field.min_length" :maxlength="field.max_length" :value="modelValue" :data-path="path" @input="input"><small v-if="field.type === 'integer' || field.type === 'number'">{{ field.minimum }}…{{ field.maximum }} {{ field.unit }}</small></label></template>
