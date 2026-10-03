// One local queue matches legacy IPC admission. It never retries a request.
let tail: Promise<unknown> = Promise.resolve();
export function queued<T>(operation: () => Promise<T>): Promise<T> {
    const next = tail.then(operation, operation);
    tail = next.catch(() => undefined);
    return next;
}
