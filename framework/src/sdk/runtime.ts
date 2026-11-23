import type {PacketApi} from "../clovernt.gen";
import type {CloverLogger} from "../shared/log";
import type {CloverFontsApi, CloverRuntimeApi} from "../shared/public";
import type {CloverUiApi} from "../shared/settings";
import type {CloverEvents} from "./events";
import type {LoadedPlugin, LoaderConfig} from "./plugin";

export interface Deasync {
    loopWhile(predicate: () => boolean): void;

    await<T>(promise: PromiseLike<T>): T;
}

export interface MainCloverApi {
    readonly version: string;
    readonly loaderVersion: string;
    readonly qqVersion: string;
    readonly coreDir: string;
    readonly pluginsDir: string;
    readonly plugins: LoadedPlugin[];
    readonly deasync: Deasync;
    readonly events: CloverEvents;
    readonly packet: PacketApi;

    log(scope: string): CloverLogger;

    loadPlugin(name: string): boolean;

    unloadPlugin(name: string): boolean;

    reloadPlugin(name: string): boolean;

    rescan(): number;

    registerLoader(config: LoaderConfig): boolean;

    unregisterLoader(id: string): boolean;
}

export interface RendererCloverApi {
    readonly ui: CloverUiApi;

    readonly runtime: CloverRuntimeApi;

    readonly fonts: CloverFontsApi;

    log(scope: string): CloverLogger;

    invoke(channel: string, ...args: unknown[]): Promise<unknown>;

    send(channel: string, ...args: unknown[]): void;

    on(channel: string, listener: (...args: unknown[]) => void): () => void;
}

export type CloverApi = MainCloverApi | RendererCloverApi;

function runtime(): CloverApi {
    const api = (globalThis as { clovernt?: CloverApi }).clovernt;
    if (!api) {
        throw new Error("@clovernt/sdk: the CloverNT runtime is not available (the `clovernt` global is missing)");
    }
    return api;
}

function proxy<T extends object>(): T {
    return new Proxy({} as T, {
        get: (_target, property) => (runtime() as unknown as Record<string | symbol, unknown>)[property],
    });
}

export const clovernt: MainCloverApi = proxy();

export const cloverntRenderer: RendererCloverApi = proxy();
