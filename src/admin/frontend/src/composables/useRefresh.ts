import { onMounted, onUnmounted } from 'vue';
import { refreshSession } from '../stores/setup.js';
import { system } from '../stores/system.js';

export function useRefresh() {
    let timer: ReturnType<typeof setInterval> | undefined;
    const visibleRefresh = () => { if (!document.hidden) { void system.refresh(); void refreshSession().catch(() => undefined); } };
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
