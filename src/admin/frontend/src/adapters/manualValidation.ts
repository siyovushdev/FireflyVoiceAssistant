import type { ConfigurationDto } from '../types/wire.js';
import type { Issue } from '../api/admin.js';

// Only detect incomplete local forms. The backend still validates the full candidate.
export function zoneSpeechIssues(configuration: ConfigurationDto, id: string): Issue[] {
    const index = configuration.rooms.findIndex(zone => zone.id === id);
    const zone = configuration.rooms[index];
    if (!zone) return []; // Deletion is validated by the backend's reference checks.
    const issues: Issue[] = [];
    for (const field of ['mentions', 'locations'] as const) {
        const values = zone.speech?.[field] ?? [];
        const path = `/rooms/${index}/speech/${field}`;
        if (zone.metadata?.container) {
            if (values.length) issues.push({ code: 'group_speech', path, message: 'Зона-группа служит только для объединения. Очистите речевые формы или снимите отметку «Зона-группа».' });
        } else if (!values.length || values.some(value => !value.trim())) {
            issues.push({ code: 'required_zone_speech', path, message: field === 'mentions'
                ? 'Укажите, как обращаться к этой зоне в командах.'
                : 'Укажите, как обозначать местоположение этой зоны в командах.' });
        }
    }
    return issues;
}

export function issueMessage(issue: Issue): string {
    if (issue.code === 'invalid_alias' && /^\/rooms\/\d+$/.test(issue.path)) {
        if (issue.message === 'Command location requires explicit mentions and phrases; use a container for grouping only.')
            return 'Укажите, как обращаться к этой зоне в командах и как обозначать её местоположение.';
        if (issue.message === 'Container locations have no command aliases.')
            return 'Зона-группа служит только для объединения. Очистите речевые формы или снимите отметку «Зона-группа».';
    }
    return issue.message;
}
