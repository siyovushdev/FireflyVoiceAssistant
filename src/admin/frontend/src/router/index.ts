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
import PlaceholderView from '../views/PlaceholderView.vue';

export const navigation = [
    { path: '/overview', title: 'Обзор', number: '01' },
    { path: '/zones', title: 'Зоны', number: '02' },
    { path: '/devices', title: 'Устройства', number: '03' },
    { path: '/states', title: 'Состояния', number: '04' },
    { path: '/setup', title: 'AI Настройка', number: '05', planned: true },
    { path: '/connections', title: 'Подключения', number: '06' },
    { path: '/advanced', title: 'Дополнительно', number: '07' },
    { path: '/diagnostics', title: 'Диагностика', number: '08' },
];
export const router = createRouter({
    history: createWebHashHistory('/v2/'),
    routes: [
        { path: '/', redirect: '/overview' },
        { path: '/overview', component: OverviewView, meta: { title: 'Обзор системы' } },
        { path: '/zones', component: ZonesView, meta: { title: 'Зоны' } },
        { path: '/devices', component: DevicesView, meta: { title: 'Устройства' } },
        { path: '/devices/:id', component: DeviceDetailView, meta: { title: 'Устройство' } },
        { path: '/states', component: StatesView, meta: { title: 'Состояния' } },
        { path: '/connections', component: ConnectionsView, meta: { title: 'Подключения' } },
        { path: '/advanced', component: AdvancedView, meta: { title: 'Дополнительно' } },
        { path: '/advanced/playground', component: PlaygroundView, meta: { title: 'Command Playground' } },
        { path: '/diagnostics', component: DiagnosticsView, meta: { title: 'Диагностика' } },
        ...navigation.filter(item => item.planned).map(item => ({ path: item.path, component: PlaceholderView, meta: { title: item.title } })),
        { path: '/:pathMatch(.*)*', component: PlaceholderView, meta: { title: 'Страница не найдена', missing: true } },
    ],
    scrollBehavior: () => ({ top: 0 }),
});
router.afterEach(to => { document.title = `${String(to.meta.title)} · Firefly`; });
