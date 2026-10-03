import { onMounted, onUnmounted } from 'vue';
import { system } from '../stores/system.js';

export function useRefresh() {
    let timer: ReturnType<typeof setInterval> | undefined;
    const visibleRefresh = () => { if (!document.hidden) void system.refresh(); };
    onMounted(() => {
        void system.refresh();
        timer = setInterval(visibleRefresh, 10000);
        document.addEventListener('visibilitychange', visibleRefresh);
    });
    onUnmounted(() => {
        clearInterval(timer);
        document.removeEventListener('visibilitychange', visibleRefresh);
    });
}
