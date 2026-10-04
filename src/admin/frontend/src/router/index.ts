import SetupView from '../views/SetupView.vue';
import ConnectionsView from '../views/ConnectionsView.vue';
import AdvancedView from '../views/AdvancedView.vue';
import DiagnosticsView from '../views/DiagnosticsView.vue';
import PlaygroundView from '../views/PlaygroundView.vue';
import { createRouter, createWebHashHistory } from 'vue-router';
import OverviewView from '../views/OverviewView.vue';
import ZonesView from '../views/ZonesView.vue';
import DevicesView from '../views/DevicesView.vue';
import DeviceDetailView from '../views/DeviceDetailView.vue';
import StatesView from '../views/StatesView.vue';

export const navigation = [
    { path: '/setup', title: 'AI Настройка', number: 'AI' },
    { path: '/overview', title: 'Обзор', number: '01' },
    { path: '/zones', title: 'Зоны', number: '02' },
    { path: '/devices', title: 'Устройства', number: '03' },
    { path: '/states', title: 'Состояния', number: '04' },
    { path: '/connections', title: 'Подключения', number: '06' },
    { path: '/advanced', title: 'Дополнительно', number: '07' },
    { path: '/diagnostics', title: 'Диагностика', number: '08' },
];
export const router = createRouter({
    history: createWebHashHistory('/'),
    routes: [
        { path: '/', redirect: '/overview' },
        { path: '/setup', component: SetupView, meta: { title: 'AI Настройка' } },
        { path: '/overview', component: OverviewView, meta: { title: 'Обзор системы' } },
        { path: '/zones', component: ZonesView, meta: { title: 'Зоны' } },
        { path: '/devices', component: DevicesView, meta: { title: 'Устройства' } },
        { path: '/devices/:id', component: DeviceDetailView, meta: { title: 'Устройство' } },
        { path: '/states', component: StatesView, meta: { title: 'Состояния' } },
        { path: '/connections', component: ConnectionsView, meta: { title: 'Подключения' } },
        { path: '/advanced', component: AdvancedView, meta: { title: 'Дополнительно' } },
        { path: '/advanced/playground', component: PlaygroundView, meta: { title: 'Command Playground' } },
        { path: '/diagnostics', component: DiagnosticsView, meta: { title: 'Диагностика' } },
        // Previous vanilla anchors remain safe bookmarks after upgrading.
        { path: '/copilot-section', redirect: '/setup' },
        { path: '/locations-section', redirect: '/zones' },
        { path: '/devices-section', redirect: '/devices' },
        { path: '/capabilities-section', redirect: '/advanced' },
        { path: '/playground', redirect: '/advanced/playground' },
        { path: '/states-section', redirect: '/states' },
        { path: '/revisions-section', redirect: '/advanced' },
        { path: '/:pathMatch(.*)*', redirect: '/overview' },
    ],
    scrollBehavior: () => ({ top: 0 }),
});
router.afterEach(to => { document.title = `${String(to.meta.title)} · Firefly`; });
