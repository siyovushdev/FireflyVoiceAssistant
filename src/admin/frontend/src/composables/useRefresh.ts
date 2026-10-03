import { onMounted, onUnmounted } from 'vue';
import { manual } from '../stores/manual.js';
import { system } from '../stores/system.js';

export function useRefresh() {
    let timer: ReturnType<typeof setInterval> | undefined;
    const visibleRefresh = () => { if (!document.hidden) { void system.refresh(); void manual.refreshSetupStatus().catch(() => undefined); } };
    onMounted(() => {
        visibleRefresh();
        timer = setInterval(visibleRefresh, 10000);
        document.addEventListener('visibilitychange', visibleRefresh);
    });
    onUnmounted(() => {
        clearInterval(timer);
        document.removeEventListener('visibilitychange', visibleRefresh);
    });
}
