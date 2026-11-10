import { onMounted, onUnmounted } from 'vue';
import { refreshSession, setupBusy, session } from '../stores/setup.js';
import { system } from '../stores/system.js';

export function useRefresh() {
    let timer: ReturnType<typeof setInterval> | undefined;
    const visibleRefresh = () => {
        if (document.hidden) return;
        // A long Setup request already owns the mutation queue. Do not pile up
        // unrelated dashboard requests behind it; its status has a separate read path.
        if (!setupBusy.value && session.value?.status !== 'planning') void system.refresh();
        void refreshSession().catch(() => undefined);
    };
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
