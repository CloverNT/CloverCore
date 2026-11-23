import type {EventMap, InvokeChannel, InvokeMap} from "../../shared/ipc";

interface PreloadApi {
    invoke(channel: string, ...args: unknown[]): Promise<unknown>;

    on(channel: string, listener: (...args: unknown[]) => void): () => void;
}

function preload(): PreloadApi {
    return (window as unknown as { clovernt: PreloadApi }).clovernt;
}

export function invoke<C extends InvokeChannel>(
    channel: C,
    ...args: InvokeMap[C]["args"]
): Promise<InvokeMap[C]["result"]> {
    return preload().invoke(channel, ...args) as Promise<InvokeMap[C]["result"]>;
}

export function subscribe<E extends keyof EventMap>(channel: E, listener: (...args: EventMap[E]) => void): () => void {
    return preload().on(channel, listener as (...args: unknown[]) => void);
}
