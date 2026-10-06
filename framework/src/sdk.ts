import type {Session, OnHeadersReceivedListenerDetails, HeadersReceivedResponse} from "electron";

import type {CloverLogger} from "./log";
import type {PluginInfo, PluginListing, LoaderConfig, LifecycleResult} from "./bridge";
import type {Packet, Credential, PacketApi, Encryption} from "./clovernt.gen";

export type {CloverLogger, PluginInfo, PluginListing, LoaderConfig, LifecycleResult};
export type {Packet, Credential, PacketApi, Encryption};

export interface Deasync {
    loopWhile(predicate: () => boolean): void;

    await<T>(promise: PromiseLike<T>): T;
}

export interface DllLoadPayload {
    baseName: string;
    fullPath: string;
    baseAddress: string;
    imageSize: number;
}

/**
 * Event payloads arrive via the JSON event bridge, so byte fields are lowercase
 * hex strings (unlike the direct `clovernt.packet` API, which uses `Uint8Array`).
 */
export interface PacketEventPayload {
    seq: number;
    command: string;
    uin: string;
    encryption: "none" | "d2key" | "zero" | string;
    body: string;
}

export interface O3EventPayload {
    command: string;
    body: string;
}

export interface CredentialEventPayload {
    uin: string;
    a2: string;
    d2: string;
    d2Key: string;
}

export interface CloverEventMap {
    "dll-load": DllLoadPayload;
    "packet-send": PacketEventPayload;
    "packet-recv": PacketEventPayload;
    "o3-send": O3EventPayload;
    "o3-recv": O3EventPayload;
    "credential": CredentialEventPayload;

    [name: string]: unknown;
}

export interface CloverEvents {
    on<K extends keyof CloverEventMap>(
        name: K,
        callback: (payload: CloverEventMap[K], control: { cancel(): void }) => void,
    ): number;

    once<K extends keyof CloverEventMap>(name: K, callback: (payload: CloverEventMap[K]) => void): number;

    off(id: number): boolean;

    emit(name: string, payload?: unknown): boolean;
}

export interface CloverApi {
    readonly version: string;

    readonly coreDir?: string;

    readonly plugins?: PluginInfo[];

    readonly deasync?: Deasync;

    readonly events?: CloverEvents;

    readonly packet?: PacketApi;

    log(scope: string): CloverLogger;

    invoke?(channel: string, ...args: unknown[]): Promise<unknown>;

    send?(channel: string, ...args: unknown[]): void;

    on?(channel: string, callback: (...args: unknown[]) => void): void;

    registerLoader?(config: LoaderConfig): boolean;

    unregisterLoader?(id: string): boolean;

    loadPlugin?(name: string): boolean;

    unloadPlugin?(name: string): boolean;

    reloadPlugin?(name: string): boolean;

    rescan?(): number;
}

export interface CloverContext {
    readonly name: string;
    readonly version: string;
    readonly coreDir: string;
    readonly plugins: PluginInfo[];
    readonly logger: CloverLogger;
    readonly events: CloverEvents;
    readonly packet: PacketApi;
    readonly electron: typeof import("electron");
    readonly app: {
        on(event: string, listener: (...args: any[]) => void): void;
    };
    readonly ipcMain: {
        on(channel: string, listener: (...args: any[]) => void): void;
        handle(channel: string, listener: (...args: any[]) => unknown): void;
    };

    onHeadersReceived(
        session: Session,
        listener: (
            details: OnHeadersReceivedListenerDetails,
            callback: (response: HeadersReceivedResponse) => void,
        ) => void,
    ): void;

    setTimeout(handler: (...args: any[]) => void, ms?: number, ...args: any[]): NodeJS.Timeout;

    setInterval(handler: (...args: any[]) => void, ms?: number, ...args: any[]): NodeJS.Timeout;

    onUnload(fn: () => void | Promise<void>): void;
}

export type PluginEntry =
    | ((ctx: CloverContext) => void | (() => void) | Promise<void>)
    | {
    onLoad(ctx: CloverContext): void | Promise<void>;
    onUnload?(): void | Promise<void>;
};

function runtime(): CloverApi {
    const api = (globalThis as { clovernt?: CloverApi }).clovernt;
    if (!api) {
        throw new Error("@clovernt/sdk: the CloverNT runtime is not available (the `clovernt` global is missing)");
    }
    return api;
}

export const clovernt: CloverApi = new Proxy({} as CloverApi, {
    get(_target, property) {
        return (runtime() as unknown as Record<string | symbol, unknown>)[property];
    },
});
