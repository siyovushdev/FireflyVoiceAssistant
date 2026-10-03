<script setup lang="ts">
import { useRoute } from 'vue-router';
const route = useRoute();
</script>
<template>
    <section class="placeholder-panel"><span class="placeholder-mark" aria-hidden="true">◌</span><p class="eyebrow">{{ route.meta.missing ? 'НАВИГАЦИЯ' : 'СЛЕДУЮЩИЕ ЭТАПЫ' }}</p><h2>{{ route.meta.missing ? 'Страница не найдена' : 'Будет в следующем этапе' }}</h2><p>{{ route.meta.missing ? 'Проверьте адрес или вернитесь к обзору системы.' : 'Этот раздел пока недоступен. В B1 можно просматривать зоны, устройства и их состояния.' }}</p><RouterLink to="/overview">К обзору системы →</RouterLink></section>
</template>
