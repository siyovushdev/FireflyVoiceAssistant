export interface PromptLimits { message_codepoints: number; message_bytes: number }
export const fallbackPromptLimits: PromptLimits = { message_codepoints: 4000, message_bytes: 16000 };
// These are transport admission limits, not a promise about model context capacity.
export function promptLimits(value?: Partial<PromptLimits>): PromptLimits {
    const valid = (n: unknown) => typeof n === 'number' && Number.isSafeInteger(n) && n > 0;
    return {
        message_codepoints: valid(value?.message_codepoints) ? value!.message_codepoints! : fallbackPromptLimits.message_codepoints,
        message_bytes: valid(value?.message_bytes) ? value!.message_bytes! : fallbackPromptLimits.message_bytes,
    };
}
export function measurePrompt(text: string, limits: PromptLimits) {
    const codepoints = [...text].length, bytes = new TextEncoder().encode(text).length;
    return { codepoints, bytes, exceeds: codepoints > limits.message_codepoints || bytes > limits.message_bytes };
}
export function planningLabel(progress?: { phase?: string; step?: number; total_steps?: number }): string {
    const phase = progress?.phase === 'reading' ? 'Разбираем описание'
        : progress?.phase === 'language_profile' ? 'Подбираем речевые фразы' : 'Готовим план';
    const step = progress?.step, total = progress?.total_steps;
    return typeof step === 'number' && typeof total === 'number' && Number.isInteger(step) && Number.isInteger(total) && step > 0 && total >= step
        ? `${phase}: шаг ${step} из ${total}…` : `${phase}…`;
}
// Credentials belong to the dedicated secret form, never the model or retry storage.
// Reference IDs such as credentials_id remain ordinary configuration facts.
export function containsPrivateCredentials(text: string): boolean {
    const value = text.toLowerCase().replace(/(?:без пароля|без паролей|пароль введу отдельно|пароли введу отдельно|no password|without password)([ \t]*[.,;!?]|$)/g, '$1');
    return /парол|password|passwd/i.test(value) || /(?:secret|token|username|login|логин)["' \t]*[=:]/i.test(value);
}
