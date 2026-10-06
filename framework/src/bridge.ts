import type {PluginManager} from "./clovernt.gen";

export const LogLevel = {
    Debug: 0,
    Info: 1,
    Warning: 2,
    Error: 3,
    Fatal: 4,
} as const;

export type LogLevelValue = (typeof LogLevel)[keyof typeof LogLevel];

export type PluginInfo = ReturnType<PluginManager["list"]>[number];
export type PluginListing = ReturnType<PluginManager["scan"]>[number];

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
