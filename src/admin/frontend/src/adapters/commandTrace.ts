import type { ConfigurationDto, WireObject } from '../types/wire.js';

function object(value: unknown): WireObject {
    return value && typeof value === 'object' && !Array.isArray(value) ? value as WireObject : {};
}
function text(value: unknown): string { return typeof value === 'string' ? value : ''; }

export function commandTrace(value: unknown, configuration?: ConfigurationDto | null) {
    const record = object(value), voice = object(record.last_voice);
    const command = object(voice.command ?? record.last_command ?? value);
    const input = text(voice.transcript) || text(command.input);
    if (!input) return null;
    const llm = object(command.llm), result = object(command.result);
    const target = text(result.targetId), action = text(result.actionId);
    const route = llm.invoked === true
        ? llm.task === 'power_intent' ? 'Qwen · определение намерения' : 'Qwen · разбор команды'
        : command.route === 'deterministic' ? 'Правила · знакомая команда'
            : llm.invoked === false ? 'Отклонено до вызова Qwen' : 'Вызов Qwen не установлен';
    const stages: Record<string, string> = { grounding: 'Не удалось однозначно выбрать устройство или действие',
        safety: 'Команда отклонена проверкой безопасности', domain: 'Действие выходит за разрешённые возможности',
        decoder: 'Ответ модели не прошёл проверку формата', model_unsupported: 'Модель не нашла однозначной команды',
        runtime: 'Ошибка обработки команды', executor: 'Ошибка отправки команды устройству',
        command_guard: 'Команда не прошла проверку перед исполнением' };
    const stage = text(command.rejection_stage);
    return { input, route, target: configuration?.devices.find(d => d.id === target)?.name || target,
        action: configuration?.actions.find(a => a.id === action)?.name || action,
        reason: stages[stage] || text(command.reason) || text(command.error),
        executed: object(command.execution).success === true };
}
