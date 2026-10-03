<script setup lang="ts">
import type { PropertyView } from '../adapters/catalog.js';
import { formatTime } from '../adapters/catalog.js';
defineProps<{ property: PropertyView }>();
</script>
<template>
    <div class="property" :data-knowledge="property.tone">
        <div class="property-value"><span>{{ property.name }}</span><strong>{{ property.value }}</strong></div>
        <span class="evidence" :class="property.tone"><span class="evidence-dot"></span>{{ property.label }}</span>
        <p v-if="property.stale" class="stale-text">Данные устарели · последнее известное значение</p>
        <small v-if="property.timestamp">{{ formatTime(property.timestamp) }}</small>
    </div>
</template>
