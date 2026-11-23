import type {HeadersReceivedResponse, OnHeadersReceivedListenerDetails, Session} from "electron";

import type {PacketApi, PluginListing, PluginScan} from "../clovernt.gen";
import type {CloverLogger} from "../shared/log";
import type {CloverEvents} from "./events";

export type LoadedPlugin = PluginListing;

export type DiscoveredPlugin = PluginScan;

export interface LifecycleResult {
    ok: boolean;
    error?: string;
}

export interface LoaderConfig {
    id: string;

    load(name: string): LifecycleResult;

    unload(name: string): LifecycleResult;

    reload(name: string): LifecycleResult;
}

export interface CloverContext {
    readonly name: string;
    readonly version: string;
    readonly loaderVersion: string;
    readonly qqVersion: string;
    readonly coreDir: string;
    readonly pluginsDir: string;
    readonly plugins: LoadedPlugin[];
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

    onUnload(fn: () => void): void;
}

export interface PluginHooks {
    onLoad(ctx: CloverContext): void;

    onUnload?(): void;
}

export type PluginSetup = (ctx: CloverContext) => void | (() => void);

export type PluginDefinition = PluginHooks | PluginSetup;

export function definePlugin<T extends PluginDefinition>(plugin: T): T {
    return plugin;
}
