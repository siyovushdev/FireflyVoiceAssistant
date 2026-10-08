import { computed, shallowRef } from 'vue';

export const operationLabels = {
    ai: 'Готовим предложение…', setup: 'Подготавливаем локальный AI…',
    cancelSetup: 'Завершаем настройку…', draft: 'Перечитываем черновик…',
    accept: 'Добавляем предложение в план…', reject: 'Отклоняем предложение…',
    edit: 'Подготавливаем редактор…', validate: 'Проверяем изменения…',
    save: 'Сохраняем конфигурацию…', apply: 'Применяем конфигурацию…',
    setupApply: 'Сохраняем и применяем конфигурацию…', verify: 'Проверяем активную версию…',
    probe: 'Проверяем подключение…', command: 'Отправляем команду…', poll: 'Читаем состояние…',
    preview: 'Проверяем команду…', refresh: 'Обновляем данные…',
    revisions: 'Загружаем версии…', restore: 'Восстанавливаем кандидат…',
    file: 'Читаем файл…', import: 'Проверяем импорт…', export: 'Готовим экспорт…', recovery: 'Восстанавливаем целостность хранилища…',
    microphone: 'Ожидаем доступ к микрофону…', stt: 'Распознаём речь локально…',
} as const;
export type OperationKind = keyof typeof operationLabels;
export interface ForegroundEntry { id: number; kind: OperationKind; label: string; visible: boolean }
export type Stage = (kind: OperationKind) => void;
// Explicit workflow admission, never an HTTP interceptor: polling stays outside.
// Visibility timers have no authority over pending state or request execution.
export function createForeground(delayMs = 250) {
    const entries = shallowRef<readonly ForegroundEntry[]>([]);
    let sequence = 0;
    function begin(kind: OperationKind) {
        const id = ++sequence;
        entries.value = [...entries.value, { id, kind, label: operationLabels[kind], visible: false }];
        const patch = (value: Partial<ForegroundEntry>) => { entries.value = entries.value.map(e => e.id === id ? { ...e, ...value } : e); };
        const timer = setTimeout(() => patch({ visible: true }), delayMs);
        return {
            stage: (next: OperationKind) => patch({ kind: next, label: operationLabels[next] }),
            finish: () => { clearTimeout(timer); entries.value = entries.value.filter(e => e.id !== id); },
        };
    }
    async function run<T>(kind: OperationKind, operation: (stage: Stage) => Promise<T>): Promise<T> {
        const ticket = begin(kind);
        try { return await operation(ticket.stage); } finally { ticket.finish(); }
    }
    return { entries: computed(() => entries.value), pending: computed(() => entries.value.length > 0),
        visible: computed(() => entries.value.filter(e => e.visible)), begin, run };
}
export const foreground = createForeground();
